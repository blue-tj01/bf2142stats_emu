/*
    FESL session, see fesl_session.h

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "fesl_session.h"

// size of the placeholder encryptedLoginInfo, as in the captured answers
#define ENCRYPTED_LOGIN_INFO_SIZE   464

// "key=value\n" text of an answer being built
typedef struct {
    char        buf[2048];
    size_t      len;
    int         overflow;
} answer;

static void add(answer *a, const char *key, const char *fmt, ...) {
    va_list     ap;
    int         n;
    size_t      room;

    if(a->overflow) return;
    room = sizeof(a->buf) - a->len;
    n = snprintf(a->buf + a->len, room, "%s=", key);
    if((n < 0) || ((size_t)n >= room)) goto overflow;
    a->len += n;
    room   -= n;

    va_start(ap, fmt);
    n = vsnprintf(a->buf + a->len, room, fmt, ap);
    va_end(ap);
    if((n < 0) || ((size_t)n + 1 >= room)) goto overflow;
    a->len += n;
    a->buf[a->len++] = '\n';
    a->buf[a->len]   = 0;
    return;

overflow:
    a->overflow = 1;
}

static int output_append(fesl_output *out, const char *command, uint8_t type, uint32_t number, const answer *a) {
    fesl_packet pkt;
    uint8_t     *tmp;
    size_t      size,
                cap;
    int         ret;

    if(a->overflow) return FESL_SESSION_ERROR;
    if(fesl_packet_init(&pkt, command, type, number, a->buf, a->len) != FESL_OK) return FESL_SESSION_ERROR;
    size = fesl_encoded_size(&pkt);
    if(out->len + size > out->cap) {
        cap = out->cap ? out->cap : 1024;
        while(cap < out->len + size) cap *= 2;
        tmp = realloc(out->data, cap);
        if(!tmp) {
            fesl_packet_free(&pkt);
            return FESL_SESSION_ERROR;
        }
        out->data = tmp;
        out->cap  = cap;
    }
    ret = fesl_encode(&pkt, out->data + out->len, out->cap - out->len);
    fesl_packet_free(&pkt);
    if(ret < 0) return FESL_SESSION_ERROR;
    out->len += ret;
    return FESL_SESSION_CONTINUE;
}

static int reply(fesl_output *out, const fesl_packet *req, const answer *a) {
    return output_append(out, req->command, FESL_TYPE_SINGLE_SERVER, req->number, a);
}

// field names the request field that caused the error, or NULL
static int reply_field_error(fesl_output *out, const fesl_packet *req, int code, const char *message, const char *field) {
    answer  a = { .len = 0 };

    add(&a, "TXN", "%s", fesl_txn(req) ? fesl_txn(req) : "");
    add(&a, "localizedMessage", "\"%s\"", message);
    if(field) {
        add(&a, "errorContainer.[]", "1");
        add(&a, "errorContainer.0.fieldName", "%s", field);
        add(&a, "errorContainer.0.fieldError", "%d", code);
    } else {
        add(&a, "errorContainer.[]", "0");
    }
    add(&a, "errorCode", "%d", code);
    return reply(out, req, &a);
}

static int reply_error(fesl_output *out, const fesl_packet *req, int code, const char *message) {
    return reply_field_error(out, req, code, message, NULL);
}

static int handle_hello(fesl_session *s, const fesl_packet *req, time_t now, fesl_output *out) {
    answer      a = { .len = 0 };
    struct tm   tm;
    char        curtime[64];
    int         ret;

    // the colons are sent escaped, like the captured answers do
    gmtime_r(&now, &tm);
    strftime(curtime, sizeof(curtime), "%b-%d-%Y %H%%3a%M%%3a%S UTC", &tm);

    add(&a, "TXN", "Hello");
    add(&a, "domainPartition.domain", "eagames");
    add(&a, "messengerIp", "0.0.0.0");
    add(&a, "messengerPort", "0");
    add(&a, "domainPartition.subDomain", "battlefield2142-2006");
    add(&a, "activityTimeoutSecs", "0");
    add(&a, "curTime", "\"%s\"", curtime);
    add(&a, "theaterIp", "0.0.0.0");
    add(&a, "theaterPort", "0");
    ret = reply(out, req, &a);
    if(ret != FESL_SESSION_CONTINUE) return ret;
    s->hello_done = 1;

    // then an unsolicited MemCheck with packet number 0, like the real EA
    // servers did for NFS Carbon (the BF2142 capture, made against an
    // emulator, reuses the Hello number). The client answers it with
    // number 0 and doesn't expect anything back.
    memset(&a, 0, sizeof(a));
    add(&a, "TXN", "MemCheck");
    add(&a, "memcheck.[]", "0");
    add(&a, "type", "0");
    add(&a, "salt", "%u", s->random(s->random_ctx) & 0x7fffffff);
    return output_append(out, "fsys", FESL_TYPE_SINGLE_SERVER, 0, &a);
}

static void make_lkey(fesl_session *s) {
    static const char   chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    int     i;

    for(i = 0; i < FESL_LKEY_SIZE - 1; i++) {
        s->lkey[i] = chars[s->random(s->random_ctx) % (sizeof(chars) - 1)];
    }
    s->lkey[i++] = '.';
    s->lkey[i]   = 0;
}

static int handle_login(fesl_session *s, const fesl_packet *req, fesl_output *out) {
    answer              a = { .len = 0 };
    const fesl_account  *acc;
    const char          *name,
                        *password,
                        *encrypted;
    char                info[ENCRYPTED_LOGIN_INFO_SIZE + 1];

    s->account = NULL;  // a failed new login logs out
    if(!s->hello_done) return reply_error(out, req, FESL_ERROR_SYSTEM, "Hello expected");
    if(s->failed_logins >= FESL_MAX_LOGIN_ATTEMPTS) {
        return reply_error(out, req, FESL_ERROR_TOO_MANY_ATTEMPTS, "Too many failed login attempts");
    }

    if(fesl_get(req, "encryptedInfo")) {
        // "remember me" logins send back the encryptedLoginInfo of a previous login
        return reply_error(out, req, FESL_ERROR_USER_NOT_FOUND, "Saved logins are not supported, enter the password");
    }
    name     = fesl_get(req, "name");
    password = fesl_get(req, "password");
    if(!name || !*name) {
        s->failed_logins++;
        return reply_field_error(out, req, FESL_ERROR_USER_NOT_FOUND, "The user was not found", "name");
    }
    acc = fesl_accounts_find(s->accounts, name);
    if(!acc) {
        s->failed_logins++;
        return reply_error(out, req, FESL_ERROR_USER_NOT_FOUND, "The user was not found");
    }
    if(!password || strcmp(password, acc->password)) {
        s->failed_logins++;
        return reply_field_error(out, req, FESL_ERROR_BAD_PASSWORD, "The password the user specified is incorrect", "password");
    }

    // checked after the password so the status isn't revealed without it
    switch(acc->status) {
        case ACCOUNT_DISABLED:
            return reply_error(out, req, FESL_ERROR_ACCOUNT_DISABLED, "The account has been disabled");
        case ACCOUNT_BANNED:
            return reply_error(out, req, FESL_ERROR_ACCOUNT_BANNED, "The account has been banned");
        case ACCOUNT_PENDING:
            return reply_error(out, req, FESL_ERROR_ACCOUNT_PENDING, "The account is pending");
        default:
            break;
    }

    s->failed_logins = 0;
    s->account = acc;
    make_lkey(s);

    add(&a, "TXN", "Login");
    add(&a, "profileId", "%u", acc->user_id);
    add(&a, "userId", "%u", acc->user_id);
    add(&a, "displayName", "%s", acc->name);
    add(&a, "lkey", "%s", s->lkey);
    encrypted = fesl_get(req, "returnEncryptedInfo");
    if(encrypted && !strcmp(encrypted, "1")) {
        // placeholder, the client only stores it for "remember me" logins
        memset(info, '1', ENCRYPTED_LOGIN_INFO_SIZE);
        info[ENCRYPTED_LOGIN_INFO_SIZE] = 0;
        add(&a, "encryptedLoginInfo", "%s", info);
    }
    add(&a, "entitledGameFeatureWrappers.0.gameFeatureId", "2590");
    add(&a, "entitledGameFeatureWrappers.0.status", "0");
    add(&a, "entitledGameFeatureWrappers.0.entitlementExpirationDate", "");
    add(&a, "entitledGameFeatureWrappers.0.message", "");
    add(&a, "entitledGameFeatureWrappers.0.entitlementExpirationDays", "-1");
    return reply(out, req, &a);
}

// BF2142 asks for the booster pack (objectIds.0=bf2142_bp1); both the
// captured server and EA's NFS Carbon server answered with no entitlements
static int handle_object_inventory(const fesl_packet *req, fesl_output *out) {
    answer  a = { .len = 0 };

    add(&a, "TXN", "GetObjectInventory");
    add(&a, "entitlements.[]", "0");
    return reply(out, req, &a);
}

void fesl_session_init(fesl_session *s, const fesl_accounts *accounts, uint32_t (*random)(void *), void *random_ctx) {
    memset(s, 0, sizeof(fesl_session));
    s->accounts   = accounts;
    s->random     = random;
    s->random_ctx = random_ctx;
}

int fesl_session_handle(fesl_session *s, const fesl_packet *req, time_t now, fesl_output *out) {
    const char  *txn = fesl_txn(req);

    if(!txn) return reply_error(out, req, FESL_ERROR_SYSTEM, "Missing TXN");

    if(!strcmp(req->command, "fsys")) {
        if(!strcmp(txn, "Hello"))    return handle_hello(s, req, now, out);
        if(!strcmp(txn, "MemCheck")) return FESL_SESSION_CONTINUE;  // answer to ours
        if(!strcmp(txn, "Ping"))     return FESL_SESSION_CONTINUE;  // answer to ours
        if(!strcmp(txn, "Goodbye"))  return FESL_SESSION_CLOSE;
    } else if(!strcmp(req->command, "dobj")) {
        if(!strcmp(txn, "GetObjectInventory")) return handle_object_inventory(req, out);
    } else if(!strcmp(req->command, "acct")) {
        if(!strcmp(txn, "Login"))    return handle_login(s, req, out);
    }
    return reply_error(out, req, FESL_ERROR_SYSTEM, "Not implemented");
}

int fesl_session_ping(fesl_output *out) {
    answer  a = { .len = 0 };

    add(&a, "TXN", "Ping");
    return output_append(out, "fsys", 0, 0, &a);
}

void fesl_output_reset(fesl_output *out) {
    out->len = 0;
}

void fesl_output_free(fesl_output *out) {
    free(out->data);
    memset(out, 0, sizeof(fesl_output));
}
