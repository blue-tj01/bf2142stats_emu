/*
    Accounts from the `account` table, see accounts_db.h

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "accounts_db.h"

#ifndef FESL_WITH_MYSQL

int fesl_accounts_db_open(fesl_account_store *store, const fesl_db_config *config) {
    (void)store;
    (void)config;
    fprintf(stderr, "Error: this server was built without database support, install the MySQL or MariaDB client library and rebuild\n");
    return -1;
}

void fesl_accounts_db_close(fesl_account_store *store) {
    (void)store;
}

int fesl_accounts_db_available(void) {
    return 0;
}

#else

#include <pthread.h>
#include <stdbool.h>
#include <mysql.h>

// MySQL 8 replaced my_bool with bool, MariaDB still has it
#if !defined(MARIADB_VERSION_ID) && defined(LIBMYSQL_VERSION_ID) && (LIBMYSQL_VERSION_ID >= 80001)
typedef bool my_bool;
#endif

#define DB_TIMEOUT  5   // seconds for connecting, reading and writing

// the name is bound as a parameter, never pasted into the query
#define FIND_QUERY  "SELECT profileid, account, password FROM account WHERE account = ? ORDER BY profileid LIMIT 1"

typedef struct {
    fesl_db_config  config;
    MYSQL           *conn;      // NULL while disconnected
    pthread_mutex_t lock;       // one connection shared by all client threads
} db_store;

static void db_disconnect(db_store *db) {
    if(db->conn) mysql_close(db->conn);
    db->conn = NULL;
}

static int db_connect(db_store *db) {
    unsigned int    timeout = DB_TIMEOUT;

    if(db->conn) return 0;
    db->conn = mysql_init(NULL);
    if(!db->conn) return -1;
    mysql_options(db->conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(db->conn, MYSQL_OPT_READ_TIMEOUT, &timeout);
    mysql_options(db->conn, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
    mysql_options(db->conn, MYSQL_SET_CHARSET_NAME, "latin1");     // the charset of the table
    if(!mysql_real_connect(db->conn, db->config.host, db->config.user, db->config.password,
                           db->config.name, db->config.port, NULL, 0)) {
        fprintf(stderr, "database: can't connect to %s: %s\n", db->config.host, mysql_error(db->conn));
        db_disconnect(db);
        return -1;
    }
    return 0;
}

// FESL_ACCOUNT_ERROR on database errors, the connection is then dropped
static int db_query(db_store *db, const char *name, fesl_account *out) {
    MYSQL_STMT      *stmt;
    MYSQL_BIND      param,
                    result[3];
    unsigned long   name_len = strlen(name),
                    lengths[3];
    unsigned int    profileid = 0;
    char            account[ACCOUNT_NAME_SIZE + 1],
                    password[ACCOUNT_PASSWORD_SIZE + 1];
    my_bool         is_null[3];
    int             ret = FESL_ACCOUNT_ERROR,
                    fetched;

    stmt = mysql_stmt_init(db->conn);
    if(!stmt) return FESL_ACCOUNT_ERROR;
    if(mysql_stmt_prepare(stmt, FIND_QUERY, strlen(FIND_QUERY))) goto quit;

    memset(&param, 0, sizeof(param));
    param.buffer_type   = MYSQL_TYPE_STRING;
    param.buffer        = (char *)name;
    param.buffer_length = name_len;
    param.length        = &name_len;
    if(mysql_stmt_bind_param(stmt, &param)) goto quit;

    memset(result, 0, sizeof(result));
    result[0].buffer_type   = MYSQL_TYPE_LONG;
    result[0].is_unsigned   = 1;
    result[0].buffer        = &profileid;
    result[1].buffer_type   = MYSQL_TYPE_STRING;
    result[1].buffer        = account;
    result[1].buffer_length = sizeof(account);
    result[2].buffer_type   = MYSQL_TYPE_STRING;
    result[2].buffer        = password;
    result[2].buffer_length = sizeof(password);
    for(int i = 0; i < 3; i++) {
        result[i].length  = &lengths[i];
        result[i].is_null = &is_null[i];
    }
    if(mysql_stmt_bind_result(stmt, result)) goto quit;
    if(mysql_stmt_execute(stmt)) goto quit;

    fetched = mysql_stmt_fetch(stmt);
    if(fetched == MYSQL_NO_DATA) {
        ret = FESL_ACCOUNT_NOT_FOUND;
    } else if((fetched == 0) || (fetched == MYSQL_DATA_TRUNCATED)) {
        // a value too long for our buffers can't belong to a usable account
        if((fetched == MYSQL_DATA_TRUNCATED) || is_null[0] || is_null[1] || is_null[2] || !profileid) {
            fprintf(stderr, "database: account %u has an invalid row, ignored\n", profileid);
            ret = FESL_ACCOUNT_NOT_FOUND;
        } else {
            memset(out, 0, sizeof(fesl_account));
            memcpy(out->name, account, lengths[1]);
            memcpy(out->password, password, lengths[2]);
            out->user_id = profileid;
            out->status  = ACCOUNT_ACTIVE;
            ret = FESL_ACCOUNT_FOUND;
        }
    }

quit:
    if(ret == FESL_ACCOUNT_ERROR) fprintf(stderr, "database: account lookup failed: %s\n", mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return ret;
}

static int db_find(fesl_account_store *store, const char *name, fesl_account *out) {
    db_store    *db = store->ctx;
    int         ret = FESL_ACCOUNT_ERROR,
                attempt;

    if(strlen(name) > ACCOUNT_NAME_SIZE) return FESL_ACCOUNT_NOT_FOUND;
    mysql_thread_init();

    pthread_mutex_lock(&db->lock);
    // a second attempt on a fresh connection, if the server restarted or
    // dropped an idle connection
    for(attempt = 0; attempt < 2; attempt++) {
        if(db_connect(db) < 0) break;
        ret = db_query(db, name, out);
        if(ret != FESL_ACCOUNT_ERROR) break;
        db_disconnect(db);
    }
    pthread_mutex_unlock(&db->lock);
    return ret;
}

static void db_thread_end(fesl_account_store *store) {
    (void)store;
    mysql_thread_end();
}

int fesl_accounts_db_open(fesl_account_store *store, const fesl_db_config *config) {
    db_store    *db;
    MYSQL_RES   *res;
    MYSQL_ROW   row;

    memset(store, 0, sizeof(fesl_account_store));
    if(mysql_library_init(0, NULL, NULL)) {
        fprintf(stderr, "Error: can't initialize the MySQL client library\n");
        return -1;
    }
    db = calloc(1, sizeof(db_store));
    if(!db) return -1;
    db->config = *config;
    pthread_mutex_init(&db->lock, NULL);

    // fail early if the database or the table can't be used
    if(db_connect(db) < 0) goto fail;
    if(mysql_query(db->conn, "SELECT COUNT(*) FROM account") || !(res = mysql_store_result(db->conn))) {
        fprintf(stderr, "database: can't read the account table: %s\n", mysql_error(db->conn));
        goto fail;
    }
    row = mysql_fetch_row(res);
    printf("database: %s on %s, %s accounts\n", config->name, config->host, (row && row[0]) ? row[0] : "?");
    mysql_free_result(res);

    store->find       = db_find;
    store->thread_end = db_thread_end;
    store->ctx        = db;
    return 0;

fail:
    db_disconnect(db);
    pthread_mutex_destroy(&db->lock);
    free(db);
    return -1;
}

void fesl_accounts_db_close(fesl_account_store *store) {
    db_store    *db = store->ctx;

    if(!db) return;
    db_disconnect(db);
    pthread_mutex_destroy(&db->lock);
    free(db);
    store->ctx = NULL;
    mysql_library_end();
}

int fesl_accounts_db_available(void) {
    return 1;
}

#endif
