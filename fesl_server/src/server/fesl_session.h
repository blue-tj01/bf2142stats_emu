/*
    FESL session: answers the packets of one client connection

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    The handlers don't do any I/O: fesl_session_handle() gets one decoded
    client packet and appends the encoded answers to a fesl_output buffer,
    which the caller sends. This keeps them testable against the captures.
*/

#ifndef FESL_SESSION_H
#define FESL_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "fesl.h"
#include "accounts.h"

#define FESL_LKEY_SIZE      28      // 27 characters + '.', like the captured answers

// Login error codes: 101 and 122 are used by the BFBC2 and NFS Carbon
// emulators, the others were checked against the NFS Carbon client by
// NFS-Online-Server. None of them is verified against BF2142 yet.
#define FESL_ERROR_SYSTEM           99
#define FESL_ERROR_USER_NOT_FOUND   101
#define FESL_ERROR_ACCOUNT_DISABLED 102
#define FESL_ERROR_ACCOUNT_BANNED   103
#define FESL_ERROR_ACCOUNT_PENDING  105
#define FESL_ERROR_TOO_MANY_ATTEMPTS 121
#define FESL_ERROR_BAD_PASSWORD     122

#define FESL_MAX_LOGIN_ATTEMPTS     5   // failed Logins per connection before 121

enum {
    FESL_SESSION_CONTINUE = 0,
    FESL_SESSION_CLOSE    = 1,  // client said Goodbye
    FESL_SESSION_ERROR    = -1  // out of memory
};

typedef struct {
    uint8_t     *data;
    size_t      len;
    size_t      cap;
} fesl_output;

typedef struct {
    const fesl_accounts *accounts;
    uint32_t    (*random)(void *ctx);   // randomness for salts and keys
    void        *random_ctx;

    int         hello_done;
    const fesl_account *account;        // logged in account or NULL
    int         failed_logins;
    char        lkey[FESL_LKEY_SIZE + 1];
} fesl_session;

void fesl_session_init(fesl_session *s, const fesl_accounts *accounts, uint32_t (*random)(void *), void *random_ctx);

// Handles one client packet, now is used for the Hello curTime.
int fesl_session_handle(fesl_session *s, const fesl_packet *req, time_t now, fesl_output *out);

// Appends the keep-alive Ping the server sends to idle clients.
int fesl_session_ping(fesl_output *out);

void fesl_output_reset(fesl_output *out);
void fesl_output_free(fesl_output *out);

#endif
