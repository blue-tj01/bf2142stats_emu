/*
    Accounts from the `account` table of the stats database

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Uses the table that database/bf2142.sql creates and fesl_login_server.exe
    uses: the login name is `account`, the password `password` (clear text)
    and the profile and user id `profileid`. The table has no status column,
    so these accounts are always active.

    Needs the MySQL or MariaDB client library; without it FESL_WITH_MYSQL is
    not defined and fesl_accounts_db_open() always fails.
*/

#ifndef ACCOUNTS_DB_H
#define ACCOUNTS_DB_H

#include "accounts.h"

typedef struct {
    const char  *host;
    unsigned int port;      // 0 for the default
    const char  *name;
    const char  *user;
    const char  *password;
} fesl_db_config;

// Connects and checks the account table. Returns 0, or -1 after printing
// the reason to stderr. Call before starting threads.
int fesl_accounts_db_open(fesl_account_store *store, const fesl_db_config *config);

void fesl_accounts_db_close(fesl_account_store *store);

// 1 if the server was built with database support
int fesl_accounts_db_available(void);

#endif
