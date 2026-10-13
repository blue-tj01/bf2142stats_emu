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
#include <ctype.h>
#include <strings.h>
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

// account text (names, e-mails) can contain anything: control characters
// and '%' are sent %XX-escaped, so a newline can't start a new field
static void add_text(answer *a, const char *key, const char *value) {
    static const char   hex[] = "0123456789abcdef";
    char                buf[3 * 128 + 1];
    size_t              n = 0;
    const unsigned char *p;

    for(p = (const unsigned char *)value; *p && (n + 3 < sizeof(buf)); p++) {
        if((*p < 0x20) || (*p == 0x7f) || (*p == '%')) {
            buf[n++] = '%';
            buf[n++] = hex[*p >> 4];
            buf[n++] = hex[*p & 15];
        } else {
            buf[n++] = *p;
        }
    }
    buf[n] = 0;
    add(a, key, "%s", buf);
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

// field names the request field that caused the error, or NULL; its
// fieldError is field_error
static int reply_field_reason(fesl_output *out, const fesl_packet *req, int code, const char *message,
        const char *field, int field_error) {
    answer  a = { .len = 0 };

    add(&a, "TXN", "%s", fesl_txn(req) ? fesl_txn(req) : "");
    add(&a, "localizedMessage", "\"%s\"", message);
    if(field) {
        add(&a, "errorContainer.[]", "1");
        add(&a, "errorContainer.0.fieldName", "%s", field);
        add(&a, "errorContainer.0.fieldError", "%d", field_error);
    } else {
        add(&a, "errorContainer.[]", "0");
    }
    add(&a, "errorCode", "%d", code);
    return reply(out, req, &a);
}

// as captured from other games: the fieldError of a login error is its code
static int reply_field_error(fesl_output *out, const fesl_packet *req, int code, const char *message, const char *field) {
    return reply_field_reason(out, req, code, message, field, code);
}

static int reply_error(fesl_output *out, const fesl_packet *req, int code, const char *message) {
    return reply_field_error(out, req, code, message, NULL);
}

static int reply_store_error(fesl_output *out, const fesl_packet *req) {
    // not the player's fault, so not counted as a failed attempt
    return reply_error(out, req, FESL_ERROR_SYSTEM, "The account service is unavailable, try again later");
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
    const fesl_account  *acc = &s->account_data;
    const char          *name,
                        *password,
                        *encrypted;
    char                info[ENCRYPTED_LOGIN_INFO_SIZE + 1];

    s->account     = NULL;  // a failed new login logs out
    s->sub_account = NULL;
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
    switch(s->accounts->find(s->accounts, name, &s->account_data)) {
        case FESL_ACCOUNT_FOUND:
            break;
        case FESL_ACCOUNT_NOT_FOUND:
            s->failed_logins++;
            return reply_error(out, req, FESL_ERROR_USER_NOT_FOUND, "The user was not found");
        default:
            return reply_store_error(out, req);
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
    add_text(&a, "displayName", acc->name);
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

// field order and spelling (profileID) as in the captured answer; the table
// has no gender or country description, the capture sent U and the code
static int handle_get_account(fesl_session *s, const fesl_packet *req, fesl_output *out) {
    answer              a = { .len = 0 };
    const fesl_account  *acc = s->account;

    if(!acc) return reply_error(out, req, FESL_ERROR_SYSTEM, "Login required");
    add(&a, "TXN", "GetAccount");
    add_text(&a, "name", acc->name);
    add(&a, "profileID", "%u", acc->user_id);
    add(&a, "userId", "%u", acc->user_id);
    add_text(&a, "email", acc->email);
    add_text(&a, "countryCode", acc->country);
    add_text(&a, "countryDesc", acc->country);
    add(&a, "dobDay", "%u", acc->dob_day);
    add(&a, "dobMonth", "%u", acc->dob_month);
    add(&a, "dobYear", "%u", acc->dob_year);
    add_text(&a, "zipCode", acc->zip_code);
    add(&a, "gender", "U");
    add(&a, "eaMailFlag", "%u", acc->ea_mail_flag);
    add(&a, "thirdPartyMailFlag", "%u", acc->third_party_mail_flag);
    return reply(out, req, &a);
}

// the unlock bundles (REG-PC-BF2142-UNLOCK-1 to 3): no entitlement, word for
// word like the captured answers, whose message isn't quoted
static int handle_entitlement_by_bundle(fesl_session *s, const fesl_packet *req, fesl_output *out) {
    answer  a = { .len = 0 };

    if(!s->account) return reply_error(out, req, FESL_ERROR_SYSTEM, "Login required");
    add(&a, "TXN", "GetEntitlementByBundle");
    add(&a, "localizedMessage", "The customer has never had entitlement for this bundle.");
    add(&a, "errorContainer.[]", "0");
    add(&a, "errorCode", "%d", FESL_ERROR_NO_ENTITLEMENT);
    return reply(out, req, &a);
}

// the soldiers of the account, the count comes last as in the capture
static int handle_get_sub_accounts(fesl_session *s, const fesl_packet *req, fesl_output *out) {
    answer              a = { .len = 0 };
    fesl_sub_account    list[FESL_MAX_SUB_ACCOUNTS];
    size_t              count,
                        i;
    char                key[32];

    if(!s->account) return reply_error(out, req, FESL_ERROR_SYSTEM, "Login required");
    if(s->accounts->sub_accounts(s->accounts, s->account, list, FESL_MAX_SUB_ACCOUNTS, &count) != FESL_ACCOUNT_FOUND) {
        return reply_store_error(out, req);
    }
    add(&a, "TXN", "GetSubAccounts");
    for(i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "subAccounts.%u", (unsigned)i);
        add_text(&a, key, list[i].name);
    }
    add(&a, "subAccounts.[]", "%u", (unsigned)count);
    return reply(out, req, &a);
}

// profileId is the soldier, userId the account (both 10 in the capture)
static int handle_login_sub_account(fesl_session *s, const fesl_packet *req, fesl_output *out) {
    answer              a = { .len = 0 };
    fesl_sub_account    list[FESL_MAX_SUB_ACCOUNTS];
    const char          *name = fesl_get(req, "name");
    size_t              count,
                        i;

    s->sub_account = NULL;
    if(!s->account) return reply_error(out, req, FESL_ERROR_SYSTEM, "Login required");
    if(s->accounts->sub_accounts(s->accounts, s->account, list, FESL_MAX_SUB_ACCOUNTS, &count) != FESL_ACCOUNT_FOUND) {
        return reply_store_error(out, req);
    }
    for(i = 0; i < count; i++) {
        if(name && !strcasecmp(name, list[i].name)) break;
    }
    if(i == count) return reply_field_error(out, req, FESL_ERROR_USER_NOT_FOUND, "The user was not found", "name");

    s->sub_account_data = list[i];
    s->sub_account      = &s->sub_account_data;
    make_lkey(s);       // a new key for the soldier's session

    add(&a, "TXN", "LoginSubAccount");
    add(&a, "lkey", "%s", s->lkey);
    add(&a, "profileId", "%u", s->sub_account->id);
    add(&a, "userId", "%u", s->account->user_id);
    return reply(out, req, &a);
}

static void random_text(fesl_session *s, char *buf, size_t len, const char *chars) {
    size_t  n = strlen(chars),
            i;

    for(i = 0; i < len; i++) buf[i] = chars[s->random(s->random_ctx) % n];
    buf[len] = 0;
}

// the ticket the game shows the GameSpy login next; the database store saves
// it where fesl_login_server.exe's GameSpy login looks for it. A failure to
// save it is only logged by the store: the FESL login itself goes on
static int handle_gamespy_preauth(fesl_session *s, const fesl_packet *req, fesl_output *out) {
    answer  a = { .len = 0 };
    char    challenge[FESL_CHALLENGE_SIZE + 1],
            ticket[FESL_TICKET_SIZE + 1];

    if(!s->sub_account) return reply_error(out, req, FESL_ERROR_SYSTEM, "LoginSubAccount required");
    random_text(s, challenge, FESL_CHALLENGE_SIZE, "abcdefghijklmnopqrstuvwxyz");
    random_text(s, ticket, FESL_TICKET_SIZE, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+");
    if(s->accounts->save_preauth) s->accounts->save_preauth(s->accounts, s->sub_account, challenge, ticket);

    add(&a, "TXN", "GameSpyPreAuth");
    add(&a, "challenge", "%s", challenge);
    add(&a, "ticket", "%s", ticket);
    return reply(out, req, &a);
}

// a bad field of AddAccount or AddSubAccount
typedef struct {
    const char  *field;     // BF2142's name of the field, as listed by openspy
    int         reason;     // FESL_FIELD_*
} field_problem;

static int name_char(int c) {
    return ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) ||
           (c == '_') || (c == '-') || (c == '.') || (c == '[') || (c == ']');
}

// account and soldier names become GameSpy nicks too, which can't contain
// '\\', so only a safe set of characters is allowed in new names
static int check_name(const char *name, size_t max) {
    size_t  len = name ? strlen(name) : 0,
            i;

    if(!len) return FESL_FIELD_REQUIRED;
    if(len < 3) return FESL_FIELD_TOO_SHORT;
    if(len > max) return FESL_FIELD_TOO_LONG;
    if(!(((name[0] >= 'A') && (name[0] <= 'Z')) || ((name[0] >= 'a') && (name[0] <= 'z')))) return FESL_FIELD_START_LETTER;
    for(i = 0; i < len; i++) {
        if(!name_char((unsigned char)name[i])) return FESL_FIELD_INVALID;
    }
    return 0;
}

static int is_leap(unsigned int y) {
    return ((y % 4) == 0) && (((y % 100) != 0) || ((y % 400) == 0));
}

static int parse_uint(const char *s, unsigned int max, unsigned int *out) {
    char            *end;
    unsigned long   n;

    if(!s || !*s || (*s == '-') || (*s == '+')) return -1;
    n = strtoul(s, &end, 10);
    if(*end || (n > max)) return -1;
    *out = n;
    return 0;
}

// fills acc from an AddAccount request, or returns the first bad field
static int read_new_account(const fesl_packet *req, fesl_account *acc, field_problem *bad) {
    static const unsigned char days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    const char  *name = fesl_get(req, "name"),
                *password = fesl_get(req, "password"),
                *email = fesl_get(req, "email"),
                *country = fesl_get(req, "countryCode"),
                *zip = fesl_get(req, "zipCode"),
                *flag,
                *at;
    size_t      i,
                len;
    int         reason;

    memset(acc, 0, sizeof(fesl_account));
    if((reason = check_name(name, ACCOUNT_NAME_SIZE))) {
        bad->field  = "Account.ScreenName";
        bad->reason = reason;
        return -1;
    }

    // "The password must contain only alphabetic and numeric characters, and
    // must be between four and sixteen characters long"
    len = password ? strlen(password) : 0;
    bad->field = "Account.Password";
    if(!len) return bad->reason = FESL_FIELD_REQUIRED, -1;
    if(len < 4) return bad->reason = FESL_FIELD_TOO_SHORT, -1;
    if(len > 16) return bad->reason = FESL_FIELD_TOO_LONG, -1;
    for(i = 0; i < len; i++) {
        if(!isalnum((unsigned char)password[i])) return bad->reason = FESL_FIELD_INVALID, -1;
    }

    len = email ? strlen(email) : 0;
    bad->field = "Account.EmailAddress";
    if(!len) return bad->reason = FESL_FIELD_REQUIRED, -1;
    if(len > ACCOUNT_EMAIL_SIZE) return bad->reason = FESL_FIELD_TOO_LONG, -1;
    at = strchr(email, '@');
    if(!at || (at == email) || strchr(at + 1, '@') || !strchr(at + 1, '.') || (email[len - 1] == '.')) {
        return bad->reason = FESL_FIELD_INVALID, -1;
    }
    for(i = 0; i < len; i++) {
        if(((unsigned char)email[i] <= ' ') || strchr("\\\"'%<>", email[i])) return bad->reason = FESL_FIELD_INVALID, -1;
    }

    len = country ? strlen(country) : 0;
    bad->field = "Account.Address.Country";
    if(!len) return bad->reason = FESL_FIELD_REQUIRED, -1;
    if(len > ACCOUNT_COUNTRY_SIZE) return bad->reason = FESL_FIELD_TOO_LONG, -1;
    for(i = 0; i < len; i++) {
        if(!isalnum((unsigned char)country[i])) return bad->reason = FESL_FIELD_INVALID, -1;
    }

    bad->field = "Account.BirthDate";
    if(parse_uint(fesl_get(req, "DOBYear"), 2100, &acc->dob_year) || (acc->dob_year < 1900) ||
       parse_uint(fesl_get(req, "DOBMonth"), 12, &acc->dob_month) || !acc->dob_month ||
       parse_uint(fesl_get(req, "DOBDay"), 31, &acc->dob_day) || !acc->dob_day ||
       (acc->dob_day > (unsigned int)(days[acc->dob_month - 1] + ((acc->dob_month == 2) && is_leap(acc->dob_year))))) {
        return bad->reason = FESL_FIELD_INVALID, -1;
    }

    len = zip ? strlen(zip) : 0;
    bad->field = "Account.Address.Zip";
    if(len > ACCOUNT_ZIP_SIZE) return bad->reason = FESL_FIELD_TOO_LONG, -1;
    for(i = 0; i < len; i++) {
        if(((unsigned char)zip[i] < ' ') || (zip[i] == '\\')) return bad->reason = FESL_FIELD_INVALID, -1;
    }

    strcpy(acc->name, name);
    strcpy(acc->password, password);
    strcpy(acc->email, email);
    strcpy(acc->country, country);
    if(zip) strcpy(acc->zip_code, zip);
    flag = fesl_get(req, "eaMailFlag");
    acc->ea_mail_flag = flag && !strcmp(flag, "1");
    flag = fesl_get(req, "thirdPartyMailFlag");
    acc->third_party_mail_flag = flag && !strcmp(flag, "1");
    acc->status = ACCOUNT_ACTIVE;
    return 0;
}

static int reply_cannot_create(fesl_output *out, const fesl_packet *req) {
    return reply_error(out, req, FESL_ERROR_SYSTEM, "Accounts and soldiers can only be created with the database");
}

// after Hello, before or instead of Login; the game logs in afterwards
static int handle_add_account(fesl_session *s, const fesl_packet *req, fesl_output *out) {
    answer          a = { .len = 0 };
    fesl_account    acc;
    field_problem   bad;
    uint32_t        id = 0;

    if(!s->hello_done) return reply_error(out, req, FESL_ERROR_SYSTEM, "Hello expected");
    if(!s->accounts->add_account) return reply_cannot_create(out, req);
    if(read_new_account(req, &acc, &bad) < 0) {
        return reply_field_reason(out, req, FESL_ERROR_FIELD, "The required parameters for this call are missing or invalid",
            bad.field, bad.reason);
    }
    switch(s->accounts->add_account(s->accounts, &acc, &id)) {
        case FESL_ACCOUNT_FOUND:
            break;
        case FESL_ACCOUNT_EXISTS:
            return reply_error(out, req, FESL_ERROR_NAME_TAKEN, "That account name is already taken");
        default:
            return reply_store_error(out, req);
    }
    add(&a, "TXN", "AddAccount");
    add(&a, "userId", "%u", id);
    add(&a, "profileId", "%u", id);
    return reply(out, req, &a);
}

static int handle_add_sub_account(fesl_session *s, const fesl_packet *req, fesl_output *out) {
    answer              a = { .len = 0 };
    fesl_sub_account    sub;
    const char          *name = fesl_get(req, "name");
    int                 reason;

    if(!s->account) return reply_error(out, req, FESL_ERROR_SYSTEM, "Login required");
    if(!s->accounts->add_sub_account) return reply_cannot_create(out, req);
    if((reason = check_name(name, FESL_SOLDIER_NAME_SIZE))) {
        return reply_field_reason(out, req, FESL_ERROR_FIELD, "The required parameters for this call are missing or invalid",
            "Account.ScreenName", reason);
    }
    switch(s->accounts->add_sub_account(s->accounts, s->account, name, FESL_MAX_SOLDIERS, &sub)) {
        case FESL_ACCOUNT_FOUND:
            break;
        case FESL_ACCOUNT_EXISTS:
            return reply_error(out, req, FESL_ERROR_NAME_TAKEN, "That account name is already taken");
        case FESL_ACCOUNT_LIMIT:
            return reply_error(out, req, FESL_ERROR_TOO_MANY_SOLDIERS,
                "You can only have four (4) active Soldiers at the same time. If you want to create a new Soldier, you need to delete an existing one first.");
        default:
            return reply_store_error(out, req);
    }
    // success as openspy, which works with BF2142, answers it: "no error"
    add(&a, "TXN", "AddSubAccount");
    add(&a, "errorContainer.[]", "0");
    add(&a, "errorCode", "%d", FESL_ERROR_NONE);
    return reply(out, req, &a);
}

static int handle_disable_sub_account(fesl_session *s, const fesl_packet *req, fesl_output *out) {
    answer              a = { .len = 0 };
    fesl_sub_account    list[FESL_MAX_SUB_ACCOUNTS];
    const char          *name = fesl_get(req, "name");
    size_t              count,
                        i;

    if(!s->account) return reply_error(out, req, FESL_ERROR_SYSTEM, "Login required");
    if(!s->accounts->remove_sub_account) return reply_cannot_create(out, req);
    if(s->accounts->sub_accounts(s->accounts, s->account, list, FESL_MAX_SUB_ACCOUNTS, &count) != FESL_ACCOUNT_FOUND) {
        return reply_store_error(out, req);
    }
    for(i = 0; i < count; i++) {
        if(name && !strcasecmp(name, list[i].name)) break;
    }
    if(i == count) return reply_field_error(out, req, FESL_ERROR_USER_NOT_FOUND, "The user was not found", "name");

    switch(s->accounts->remove_sub_account(s->accounts, s->account, &list[i])) {
        case FESL_ACCOUNT_FOUND:
            break;
        case FESL_ACCOUNT_NOT_FOUND:     // removed meanwhile
            return reply_field_error(out, req, FESL_ERROR_USER_NOT_FOUND, "The user was not found", "name");
        default:
            return reply_store_error(out, req);
    }
    if(s->sub_account && (s->sub_account->id == list[i].id)) s->sub_account = NULL;

    add(&a, "TXN", "DisableSubAccount");
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

void fesl_session_init(fesl_session *s, fesl_account_store *accounts, uint32_t (*random)(void *), void *random_ctx) {
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
        if(!strcmp(txn, "GetAccount")) return handle_get_account(s, req, out);
        if(!strcmp(txn, "GetSubAccounts")) return handle_get_sub_accounts(s, req, out);
        if(!strcmp(txn, "LoginSubAccount")) return handle_login_sub_account(s, req, out);
        if(!strcmp(txn, "GameSpyPreAuth")) return handle_gamespy_preauth(s, req, out);
        if(!strcmp(txn, "AddAccount")) return handle_add_account(s, req, out);
        if(!strcmp(txn, "AddSubAccount")) return handle_add_sub_account(s, req, out);
        if(!strcmp(txn, "DisableSubAccount")) return handle_disable_sub_account(s, req, out);
    } else if(!strcmp(req->command, "subs")) {
        if(!strcmp(txn, "GetEntitlementByBundle")) return handle_entitlement_by_bundle(s, req, out);
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
