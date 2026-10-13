/*
    Account lookup: the accounts file, or the database (accounts_db.h)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    One account per line: "name password user_id [status]", '#' starts a
    comment. The status is active (default), disabled, banned or pending.
    Names and passwords can't contain spaces. Passwords are stored in clear
    text, so this is only meant for testing.
*/

#ifndef ACCOUNTS_H
#define ACCOUNTS_H

#include <stddef.h>
#include <stdint.h>

#define ACCOUNT_NAME_SIZE       45      // varchar(45) in the database
#define ACCOUNT_PASSWORD_SIZE   64
#define ACCOUNT_EMAIL_SIZE      45
#define ACCOUNT_COUNTRY_SIZE    3
#define ACCOUNT_ZIP_SIZE        45

typedef enum {
    ACCOUNT_ACTIVE = 0,
    ACCOUNT_DISABLED,
    ACCOUNT_BANNED,
    ACCOUNT_PENDING
} fesl_account_status;

typedef struct {
    char        name[ACCOUNT_NAME_SIZE + 1];
    char        password[ACCOUNT_PASSWORD_SIZE + 1];
    uint32_t    user_id;
    fesl_account_status status;

    // profile for GetAccount, only the database has it (empty or 0 otherwise)
    char        email[ACCOUNT_EMAIL_SIZE + 1];
    char        country[ACCOUNT_COUNTRY_SIZE + 1];
    unsigned int dob_year,
                dob_month,
                dob_day;
    char        zip_code[ACCOUNT_ZIP_SIZE + 1];
    unsigned int ea_mail_flag,
                third_party_mail_flag;
} fesl_account;

typedef struct {
    fesl_account *list;
    size_t      count;
} fesl_accounts;

// Parses accounts from a text buffer. Returns 0, or the 1-based number of
// the first invalid line, or -1 if out of memory.
int fesl_accounts_parse(fesl_accounts *a, const char *text);

// Same from a file, returns -1 also if it can't be read.
int fesl_accounts_load(fesl_accounts *a, const char *path);

// Case-insensitive lookup by name, NULL if missing.
const fesl_account *fesl_accounts_find(const fesl_accounts *a, const char *name);

void fesl_accounts_free(fesl_accounts *a);

// A sub-account (soldier) of an account. With the database its id is the
// `id` of the subaccount table, otherwise the account's own id.
#define FESL_MAX_SUB_ACCOUNTS   16

typedef struct {
    uint32_t    id;
    char        name[ACCOUNT_NAME_SIZE + 1];
} fesl_sub_account;

// Where the session looks accounts up: the accounts file or the database.
typedef struct fesl_account_store fesl_account_store;

enum {
    FESL_ACCOUNT_FOUND     = 1,
    FESL_ACCOUNT_NOT_FOUND = 0,
    FESL_ACCOUNT_ERROR     = -1     // the store can't be reached
};

struct fesl_account_store {
    // fills out on FESL_ACCOUNT_FOUND, names are case-insensitive
    int         (*find)(fesl_account_store *store, const char *name, fesl_account *out);
    // the sub-accounts of an account, at most max of them: FESL_ACCOUNT_FOUND
    // (even with *count 0) or FESL_ACCOUNT_ERROR
    int         (*sub_accounts)(fesl_account_store *store, const fesl_account *acc,
                    fesl_sub_account *list, size_t max, size_t *count);
    // saves the GameSpy pre-auth of a sub-account for the GameSpy login, may
    // be NULL: FESL_ACCOUNT_FOUND, FESL_ACCOUNT_NOT_FOUND if there is no
    // GameSpy player to save it to, or FESL_ACCOUNT_ERROR
    int         (*save_preauth)(fesl_account_store *store, const fesl_sub_account *sub,
                    const char *challenge, const char *ticket);
    // called by each connection thread before it exits, may be NULL
    void        (*thread_end)(fesl_account_store *store);
    void        *ctx;
};

// A store backed by a parsed accounts file, which must outlive it.
void fesl_accounts_store_init(fesl_account_store *store, const fesl_accounts *a);

#endif
