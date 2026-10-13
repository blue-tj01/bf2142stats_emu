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
#define FIND_QUERY  "SELECT profileid, account, password, email, countryCode, DOBYear, DOBMonth, DOBDay, " \
                    "zipCode, eaMailFlag, thirdPartyMailFlag FROM account WHERE account = ? ORDER BY profileid LIMIT 1"

#define SUB_ACCOUNTS_QUERY  "SELECT id, subaccount FROM subaccount WHERE profileid = ? ORDER BY id LIMIT ?"

// read by the GameSpy login of fesl_login_server.exe, which looks the
// player up by authtoken; gamespy_players.id is the sub-account id
#define PREAUTH_QUERY       "UPDATE gamespy_players SET challenge = ?, authtoken = ? WHERE id = ?"

// the name is only inserted when no account has it yet (case-insensitively,
// by the table's collation); there is no unique key to rely on
#define ADD_ACCOUNT_QUERY   "INSERT INTO account (account, password, email, countryCode, DOBYear, DOBMonth, " \
                            "DOBDay, zipCode, eaMailFlag, thirdPartyMailFlag) " \
                            "SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ? FROM DUAL " \
                            "WHERE NOT EXISTS (SELECT 1 FROM account WHERE account = ?)"

// soldier names are GameSpy nicks, unique among all players
#define SUB_NAME_TAKEN_QUERY "SELECT (SELECT COUNT(*) FROM subaccount WHERE subaccount = ?) + " \
                            "(SELECT COUNT(*) FROM gamespy_players WHERE nick = ?)"
#define SUB_COUNT_QUERY     "SELECT COUNT(*) FROM subaccount WHERE profileid = ?"
#define ADD_SUB_QUERY       "INSERT INTO subaccount (profileid, subaccount) VALUES (?, ?)"
#define REMOVE_SUB_QUERY    "DELETE FROM subaccount WHERE id = ? AND profileid = ?"
// like fesl_login_server.exe, which also deletes the soldier's GameSpy player
#define REMOVE_PLAYER_QUERY "DELETE FROM gamespy_players WHERE id = ?"

enum {  // result columns
    COL_ID, COL_NAME, COL_PASSWORD, COL_EMAIL, COL_COUNTRY, COL_DOB_YEAR, COL_DOB_MONTH,
    COL_DOB_DAY, COL_ZIP, COL_EA_MAIL, COL_THIRD_PARTY_MAIL, COLUMNS
};

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
    // CLIENT_FOUND_ROWS: UPDATE reports the rows it matched, not only the changed ones
    if(!mysql_real_connect(db->conn, db->config.host, db->config.user, db->config.password,
                           db->config.name, db->config.port, NULL, CLIENT_FOUND_ROWS)) {
        fprintf(stderr, "database: can't connect to %s: %s\n", db->config.host, mysql_error(db->conn));
        db_disconnect(db);
        return -1;
    }
    return 0;
}

static void bind_string(MYSQL_BIND *b, char *buf, size_t size) {
    b->buffer_type   = MYSQL_TYPE_STRING;
    b->buffer        = buf;
    b->buffer_length = size;
}

static void bind_uint(MYSQL_BIND *b, unsigned int *value) {
    b->buffer_type = MYSQL_TYPE_LONG;
    b->is_unsigned = 1;
    b->buffer      = value;
}

static void bind_param_string(MYSQL_BIND *b, const char *value, unsigned long *len) {
    *len = strlen(value);
    b->buffer_type   = MYSQL_TYPE_STRING;
    b->buffer        = (char *)value;
    b->buffer_length = *len;
    b->length        = len;
}

typedef struct {
    const char      *name;
    fesl_account    *out;
} find_args;

// the operations return FESL_ACCOUNT_ERROR on database errors, the
// connection is then dropped and the operation tried once more
static int op_find(db_store *db, void *arg) {
    const char      *name = ((find_args *)arg)->name;
    fesl_account    *out = ((find_args *)arg)->out;
    MYSQL_STMT      *stmt;
    MYSQL_BIND      param,
                    result[COLUMNS];
    unsigned long   name_len = strlen(name),
                    lengths[COLUMNS];
    my_bool         is_null[COLUMNS];
    fesl_account    acc;
    int             ret = FESL_ACCOUNT_ERROR,
                    fetched,
                    i;

    stmt = mysql_stmt_init(db->conn);
    if(!stmt) return FESL_ACCOUNT_ERROR;
    if(mysql_stmt_prepare(stmt, FIND_QUERY, strlen(FIND_QUERY))) goto quit;

    memset(&param, 0, sizeof(param));
    param.buffer_type   = MYSQL_TYPE_STRING;
    param.buffer        = (char *)name;
    param.buffer_length = name_len;
    param.length        = &name_len;
    if(mysql_stmt_bind_param(stmt, &param)) goto quit;

    // the buffers are one byte larger than the columns, for the NUL
    memset(&acc, 0, sizeof(acc));
    memset(result, 0, sizeof(result));
    bind_uint(&result[COL_ID], &acc.user_id);
    bind_string(&result[COL_NAME], acc.name, sizeof(acc.name));
    bind_string(&result[COL_PASSWORD], acc.password, sizeof(acc.password));
    bind_string(&result[COL_EMAIL], acc.email, sizeof(acc.email));
    bind_string(&result[COL_COUNTRY], acc.country, sizeof(acc.country));
    bind_uint(&result[COL_DOB_YEAR], &acc.dob_year);
    bind_uint(&result[COL_DOB_MONTH], &acc.dob_month);
    bind_uint(&result[COL_DOB_DAY], &acc.dob_day);
    bind_string(&result[COL_ZIP], acc.zip_code, sizeof(acc.zip_code));
    bind_uint(&result[COL_EA_MAIL], &acc.ea_mail_flag);
    bind_uint(&result[COL_THIRD_PARTY_MAIL], &acc.third_party_mail_flag);
    for(i = 0; i < COLUMNS; i++) {
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
        if((fetched == MYSQL_DATA_TRUNCATED) || is_null[COL_ID] || is_null[COL_NAME] ||
           is_null[COL_PASSWORD] || !acc.user_id) {
            fprintf(stderr, "database: account %u has an invalid row, ignored\n", acc.user_id);
            ret = FESL_ACCOUNT_NOT_FOUND;
        } else {
            // strings are NUL terminated by the memset, NULLs become empty or 0
            for(i = 0; i < COLUMNS; i++) {
                if(!is_null[i]) continue;
                if(result[i].buffer_type == MYSQL_TYPE_STRING) ((char *)result[i].buffer)[0] = 0;
                else *(unsigned int *)result[i].buffer = 0;
            }
            for(i = 0; i < COLUMNS; i++) {
                if((result[i].buffer_type == MYSQL_TYPE_STRING) && !is_null[i]) ((char *)result[i].buffer)[lengths[i]] = 0;
            }
            acc.status = ACCOUNT_ACTIVE;
            *out = acc;
            ret = FESL_ACCOUNT_FOUND;
        }
    }

quit:
    if(ret == FESL_ACCOUNT_ERROR) fprintf(stderr, "database: account lookup failed: %s\n", mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return ret;
}

typedef struct {
    const fesl_account *acc;
    fesl_sub_account *list;
    unsigned int    max;
    size_t          *count;
} sub_accounts_args;

static int op_sub_accounts(db_store *db, void *arg) {
    sub_accounts_args *args = arg;
    MYSQL_STMT      *stmt;
    MYSQL_BIND      param[2],
                    result[2];
    unsigned int    profileid = args->acc->user_id,
                    id;
    char            name[ACCOUNT_NAME_SIZE + 1];
    unsigned long   name_len;
    my_bool         is_null[2];
    int             ret = FESL_ACCOUNT_ERROR,
                    fetched;

    *args->count = 0;
    stmt = mysql_stmt_init(db->conn);
    if(!stmt) return FESL_ACCOUNT_ERROR;
    if(mysql_stmt_prepare(stmt, SUB_ACCOUNTS_QUERY, strlen(SUB_ACCOUNTS_QUERY))) goto quit;

    memset(param, 0, sizeof(param));
    bind_uint(&param[0], &profileid);
    bind_uint(&param[1], &args->max);
    if(mysql_stmt_bind_param(stmt, param)) goto quit;

    memset(result, 0, sizeof(result));
    bind_uint(&result[0], &id);
    bind_string(&result[1], name, sizeof(name));
    result[1].length  = &name_len;
    result[0].is_null = &is_null[0];
    result[1].is_null = &is_null[1];
    if(mysql_stmt_bind_result(stmt, result)) goto quit;
    if(mysql_stmt_execute(stmt)) goto quit;

    while(((fetched = mysql_stmt_fetch(stmt)) == 0) || (fetched == MYSQL_DATA_TRUNCATED)) {
        if((fetched == MYSQL_DATA_TRUNCATED) || is_null[0] || is_null[1] || !id || !name_len) {
            fprintf(stderr, "database: sub-account %u of account %u has an invalid row, ignored\n", id, profileid);
            continue;
        }
        if(*args->count >= args->max) break;
        memset(&args->list[*args->count], 0, sizeof(fesl_sub_account));
        args->list[*args->count].id = id;
        memcpy(args->list[*args->count].name, name, name_len);
        (*args->count)++;
    }
    if(fetched == MYSQL_NO_DATA) ret = FESL_ACCOUNT_FOUND;

quit:
    if(ret == FESL_ACCOUNT_ERROR) fprintf(stderr, "database: sub-account lookup failed: %s\n", mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return ret;
}

typedef struct {
    const fesl_sub_account *sub;
    const char      *challenge,
                    *ticket;
} preauth_args;

static int op_preauth(db_store *db, void *arg) {
    preauth_args    *args = arg;
    MYSQL_STMT      *stmt;
    MYSQL_BIND      param[3];
    unsigned long   challenge_len,
                    ticket_len;
    unsigned int    id = args->sub->id;
    int             ret = FESL_ACCOUNT_ERROR;

    stmt = mysql_stmt_init(db->conn);
    if(!stmt) return FESL_ACCOUNT_ERROR;
    if(mysql_stmt_prepare(stmt, PREAUTH_QUERY, strlen(PREAUTH_QUERY))) goto quit;

    memset(param, 0, sizeof(param));
    bind_param_string(&param[0], args->challenge, &challenge_len);
    bind_param_string(&param[1], args->ticket, &ticket_len);
    bind_uint(&param[2], &id);
    if(mysql_stmt_bind_param(stmt, param)) goto quit;
    if(mysql_stmt_execute(stmt)) goto quit;

    if(mysql_stmt_affected_rows(stmt) > 0) {
        ret = FESL_ACCOUNT_FOUND;
    } else {
        fprintf(stderr, "database: no gamespy_players row with id %u for sub-account %s, "
                        "its GameSpy login will fail\n", id, args->sub->name);
        ret = FESL_ACCOUNT_NOT_FOUND;
    }

quit:
    if(ret == FESL_ACCOUNT_ERROR) fprintf(stderr, "database: saving the GameSpy pre-auth failed: %s\n", mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return ret;
}

// runs a statement with params; affected and insert_id may be NULL
static int db_exec(db_store *db, const char *query, MYSQL_BIND *params,
        unsigned long long *affected, unsigned long long *insert_id) {
    MYSQL_STMT  *stmt;
    int         ret = FESL_ACCOUNT_ERROR;

    stmt = mysql_stmt_init(db->conn);
    if(!stmt) return FESL_ACCOUNT_ERROR;
    if(mysql_stmt_prepare(stmt, query, strlen(query)) ||
       (params && mysql_stmt_bind_param(stmt, params)) ||
       mysql_stmt_execute(stmt)) {
        fprintf(stderr, "database: %s failed: %s\n", query, mysql_stmt_error(stmt));
    } else {
        if(affected)  *affected  = mysql_stmt_affected_rows(stmt);
        if(insert_id) *insert_id = mysql_stmt_insert_id(stmt);
        ret = FESL_ACCOUNT_FOUND;
    }
    mysql_stmt_close(stmt);
    return ret;
}

// runs a query returning one number
static int db_count(db_store *db, const char *query, MYSQL_BIND *params, unsigned long long *count) {
    MYSQL_STMT  *stmt;
    MYSQL_BIND  result;
    my_bool     is_null = 0;
    int         ret = FESL_ACCOUNT_ERROR;

    *count = 0;
    stmt = mysql_stmt_init(db->conn);
    if(!stmt) return FESL_ACCOUNT_ERROR;
    memset(&result, 0, sizeof(result));
    result.buffer_type = MYSQL_TYPE_LONGLONG;
    result.is_unsigned = 1;
    result.buffer      = count;
    result.is_null     = &is_null;
    if(mysql_stmt_prepare(stmt, query, strlen(query)) ||
       (params && mysql_stmt_bind_param(stmt, params)) ||
       mysql_stmt_bind_result(stmt, &result) ||
       mysql_stmt_execute(stmt) ||
       mysql_stmt_fetch(stmt)) {
        fprintf(stderr, "database: %s failed: %s\n", query, mysql_stmt_error(stmt));
    } else {
        if(is_null) *count = 0;
        ret = FESL_ACCOUNT_FOUND;
    }
    mysql_stmt_close(stmt);
    return ret;
}

typedef struct {
    const fesl_account *acc;
    uint32_t        *id;
} add_account_args;

static int op_add_account(db_store *db, void *arg) {
    add_account_args *args = arg;
    const fesl_account *acc = args->acc;
    MYSQL_BIND      param[11];
    unsigned long   len[5];
    unsigned int    year = acc->dob_year,
                    month = acc->dob_month,
                    day = acc->dob_day,
                    ea_mail = acc->ea_mail_flag,
                    third_party = acc->third_party_mail_flag;
    unsigned long long affected = 0,
                    id = 0;
    unsigned long   name_len2;
    int             ret;

    memset(param, 0, sizeof(param));
    bind_param_string(&param[0], acc->name, &len[0]);
    bind_param_string(&param[1], acc->password, &len[1]);
    bind_param_string(&param[2], acc->email, &len[2]);
    bind_param_string(&param[3], acc->country, &len[3]);
    bind_uint(&param[4], &year);
    bind_uint(&param[5], &month);
    bind_uint(&param[6], &day);
    bind_param_string(&param[7], acc->zip_code, &len[4]);
    bind_uint(&param[8], &ea_mail);
    bind_uint(&param[9], &third_party);
    bind_param_string(&param[10], acc->name, &name_len2);

    ret = db_exec(db, ADD_ACCOUNT_QUERY, param, &affected, &id);
    if(ret != FESL_ACCOUNT_FOUND) return ret;
    if(!affected) return FESL_ACCOUNT_EXISTS;
    *args->id = (uint32_t)id;
    printf("database: account %s created with id %u\n", acc->name, *args->id);
    return FESL_ACCOUNT_FOUND;
}

typedef struct {
    const fesl_account *acc;
    const char      *name;
    size_t          max;
    fesl_sub_account *out;
} add_sub_args;

static int op_add_sub_account(db_store *db, void *arg) {
    add_sub_args    *args = arg;
    MYSQL_BIND      param[2];
    unsigned long   len[2];
    unsigned int    profileid = args->acc->user_id;
    unsigned long long count = 0,
                    id = 0;
    int             ret;

    // the checks and the insert run under the store's lock, so this server
    // can't race itself; fesl_login_server.exe on the same database could
    memset(param, 0, sizeof(param));
    bind_param_string(&param[0], args->name, &len[0]);
    bind_param_string(&param[1], args->name, &len[1]);
    if((ret = db_count(db, SUB_NAME_TAKEN_QUERY, param, &count)) != FESL_ACCOUNT_FOUND) return ret;
    if(count) return FESL_ACCOUNT_EXISTS;

    memset(param, 0, sizeof(param));
    bind_uint(&param[0], &profileid);
    if((ret = db_count(db, SUB_COUNT_QUERY, param, &count)) != FESL_ACCOUNT_FOUND) return ret;
    if(count >= args->max) return FESL_ACCOUNT_LIMIT;

    memset(param, 0, sizeof(param));
    bind_uint(&param[0], &profileid);
    bind_param_string(&param[1], args->name, &len[0]);
    if((ret = db_exec(db, ADD_SUB_QUERY, param, NULL, &id)) != FESL_ACCOUNT_FOUND) return ret;

    memset(args->out, 0, sizeof(fesl_sub_account));
    args->out->id = (uint32_t)id;
    snprintf(args->out->name, sizeof(args->out->name), "%s", args->name);
    // its gamespy_players row (needed by fesl_login_server.exe's GameSpy
    // login) isn't created yet: how that server fills passwordenc is unknown
    printf("database: sub-account %s created with id %u for account %u, without a gamespy_players row\n",
        args->name, args->out->id, profileid);
    return FESL_ACCOUNT_FOUND;
}

typedef struct {
    const fesl_account *acc;
    const fesl_sub_account *sub;
} remove_sub_args;

static int op_remove_sub_account(db_store *db, void *arg) {
    remove_sub_args *args = arg;
    MYSQL_BIND      param[2];
    unsigned int    id = args->sub->id,
                    profileid = args->acc->user_id;
    unsigned long long affected = 0;
    int             ret;

    memset(param, 0, sizeof(param));
    bind_uint(&param[0], &id);
    bind_uint(&param[1], &profileid);
    if((ret = db_exec(db, REMOVE_SUB_QUERY, param, &affected, NULL)) != FESL_ACCOUNT_FOUND) return ret;
    if(!affected) return FESL_ACCOUNT_NOT_FOUND;

    // the soldier is gone either way, a GameSpy player left behind is only logged
    memset(param, 0, sizeof(param));
    bind_uint(&param[0], &id);
    if(db_exec(db, REMOVE_PLAYER_QUERY, param, NULL, NULL) != FESL_ACCOUNT_FOUND) {
        fprintf(stderr, "database: the gamespy_players row %u of removed sub-account %s is left\n", id, args->sub->name);
    }
    printf("database: sub-account %s (%u) of account %u removed\n", args->sub->name, id, profileid);
    return FESL_ACCOUNT_FOUND;
}

// runs op on the shared connection, a second time on a fresh connection if
// the server restarted or dropped an idle connection
static int db_run(db_store *db, int (*op)(db_store *, void *), void *arg) {
    int     ret = FESL_ACCOUNT_ERROR,
            attempt;

    mysql_thread_init();
    pthread_mutex_lock(&db->lock);
    for(attempt = 0; attempt < 2; attempt++) {
        if(db_connect(db) < 0) break;
        ret = op(db, arg);
        if(ret != FESL_ACCOUNT_ERROR) break;
        db_disconnect(db);
    }
    pthread_mutex_unlock(&db->lock);
    return ret;
}

static int db_find(fesl_account_store *store, const char *name, fesl_account *out) {
    find_args   args = { name, out };

    if(strlen(name) > ACCOUNT_NAME_SIZE) return FESL_ACCOUNT_NOT_FOUND;
    return db_run(store->ctx, op_find, &args);
}

static int db_sub_accounts(fesl_account_store *store, const fesl_account *acc,
        fesl_sub_account *list, size_t max, size_t *count) {
    sub_accounts_args args = { acc, list, (unsigned int)max, count };

    return db_run(store->ctx, op_sub_accounts, &args);
}

static int db_save_preauth(fesl_account_store *store, const fesl_sub_account *sub,
        const char *challenge, const char *ticket) {
    preauth_args args = { sub, challenge, ticket };

    return db_run(store->ctx, op_preauth, &args);
}

static int db_add_account(fesl_account_store *store, const fesl_account *acc, uint32_t *id) {
    add_account_args args = { acc, id };

    return db_run(store->ctx, op_add_account, &args);
}

static int db_add_sub_account(fesl_account_store *store, const fesl_account *acc, const char *name,
        size_t max, fesl_sub_account *out) {
    add_sub_args args = { acc, name, max, out };

    return db_run(store->ctx, op_add_sub_account, &args);
}

static int db_remove_sub_account(fesl_account_store *store, const fesl_account *acc, const fesl_sub_account *sub) {
    remove_sub_args args = { acc, sub };

    return db_run(store->ctx, op_remove_sub_account, &args);
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

    store->find         = db_find;
    store->sub_accounts = db_sub_accounts;
    store->save_preauth = db_save_preauth;
    store->add_account  = db_add_account;
    store->add_sub_account = db_add_sub_account;
    store->remove_sub_account = db_remove_sub_account;
    store->thread_end   = db_thread_end;
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
