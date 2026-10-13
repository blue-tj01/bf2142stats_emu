/*
    Accounts file, a stand-in until database support exists

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

#define ACCOUNT_NAME_SIZE       32
#define ACCOUNT_PASSWORD_SIZE   64

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

#endif
