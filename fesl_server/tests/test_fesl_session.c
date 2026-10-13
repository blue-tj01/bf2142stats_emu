/*
    FESL session tests: the captured client packets go through the handlers
    and the answers are compared with the captured server answers

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "fesl.h"
#include "fesl_session.h"
#include "accounts.h"
#include "pcap_stream.h"

static int  g_checks   = 0,
            g_failures = 0;

#define CHECK(X) do { \
        g_checks++; \
        if(!(X)) { \
            g_failures++; \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #X); \
        } \
    } while(0)

#define CHECK_STR(A, B) do { \
        const char *a_ = (A), *b_ = (B); \
        g_checks++; \
        if(!a_ || !b_ || strcmp(a_, b_)) { \
            g_failures++; \
            fprintf(stderr, "%s:%d: %s is \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #A, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
        } \
    } while(0)

#define D1_HELLO_TIME   1441296072  // when the d1 server answered Hello, Sep-03-2015 16:01:12 UTC

static const char   *ACCOUNTS =
    "# test accounts\n"
    "Tema567 123456 10\n"
    "other   secret  11   # comment\n";

static uint32_t counter_random(void *ctx) {
    uint32_t    *n = ctx;

    return (*n)++ * 2654435761u;
}

typedef struct {
    fesl_packet *pkt;
    size_t      count;
} packet_list;

static int decode_all(const uint8_t *data, size_t len, packet_list *list) {
    size_t  consumed;

    memset(list, 0, sizeof(packet_list));
    while(len) {
        list->pkt = realloc(list->pkt, (list->count + 1) * sizeof(fesl_packet));
        if(fesl_decode(data, len, &list->pkt[list->count], &consumed) != FESL_OK) return -1;
        list->count++;
        data += consumed;
        len  -= consumed;
    }
    return 0;
}

static void free_list(packet_list *list) {
    size_t  i;

    for(i = 0; i < list->count; i++) fesl_packet_free(&list->pkt[i]);
    free(list->pkt);
    memset(list, 0, sizeof(packet_list));
}

static const fesl_packet *find_txn(const packet_list *list, const char *txn) {
    size_t  i;

    for(i = 0; i < list->count; i++) {
        if(fesl_txn(&list->pkt[i]) && !strcmp(fesl_txn(&list->pkt[i]), txn)) return &list->pkt[i];
    }
    return NULL;
}

// same header and same keys in the same order, values compared unless listed in skip
static void compare_answer(const fesl_packet *ours, const fesl_packet *captured, const char **skip, int same_number) {
    size_t  i,
            j;
    int     skipped;

    CHECK_STR(ours->command, captured->command);
    CHECK(ours->type == captured->type);
    if(same_number) CHECK(ours->number == captured->number);
    CHECK(ours->count == captured->count);
    for(i = 0; (i < ours->count) && (i < captured->count); i++) {
        CHECK_STR(ours->pairs[i].key, captured->pairs[i].key);
        skipped = 0;
        for(j = 0; skip && skip[j]; j++) {
            if(!strcmp(captured->pairs[i].key, skip[j])) skipped = 1;
        }
        if(!skipped) CHECK_STR(ours->pairs[i].value, captured->pairs[i].value);
    }
}

// our answer to the request with this command and number
static const fesl_packet *find_answer(const packet_list *list, const char *command, uint32_t number, const char *txn) {
    size_t  i;

    for(i = 0; i < list->count; i++) {
        if(!strcmp(list->pkt[i].command, command) && (list->pkt[i].number == number) &&
           fesl_txn(&list->pkt[i]) && !strcmp(fesl_txn(&list->pkt[i]), txn)) return &list->pkt[i];
    }
    return NULL;
}

// the account of the captures, with the profile the captured GetAccount sent
static int captured_find(fesl_account_store *store, const char *name, fesl_account *out) {
    (void)store;
    if(strcasecmp(name, "Tema567")) return FESL_ACCOUNT_NOT_FOUND;
    memset(out, 0, sizeof(*out));
    strcpy(out->name, "Tema567");
    strcpy(out->password, "123456");
    out->user_id = 10;
    strcpy(out->email, "tema567@list.ru");
    strcpy(out->country, "RU");
    out->dob_year  = 1991;
    out->dob_month = 1;
    out->dob_day   = 7;
    strcpy(out->zip_code, "624760");
    out->ea_mail_flag          = 1;
    out->third_party_mail_flag = 1;
    return FESL_ACCOUNT_FOUND;
}

// one sub-account per account, the account itself, like the accounts file
static int self_sub_accounts(fesl_account_store *store, const fesl_account *acc,
        fesl_sub_account *list, size_t max, size_t *count) {
    (void)store;
    (void)max;
    memset(list, 0, sizeof(*list));
    list->id = acc->user_id;
    strcpy(list->name, acc->name);
    *count = 1;
    return FESL_ACCOUNT_FOUND;
}

static int is_text(const char *s, size_t len, const char *chars) {
    return s && (strlen(s) == len) && (strspn(s, chars) == len);
}

static void test_replay(const char *path, int exact_time) {
    static const char   *hello_skip[] = { "curTime", NULL },
                        *memcheck_skip[] = { "salt", NULL };
    pcap_tcp_streams    s;
    packet_list client,
                server,
                ours;
    fesl_accounts accounts;
    fesl_account_store store;
    fesl_session session;
    fesl_output out = { 0 };
    const fesl_packet *captured,
                *mine;
    uint32_t    rnd = 1;
    size_t      i;
    int         matched = 0;
    char        login_lkey[FESL_LKEY_SIZE + 1] = "";
    char        curtime[64];

    printf("session replay %s\n", path);
    CHECK(fesl_accounts_parse(&accounts, ACCOUNTS) == 0);
    CHECK(pcap_read_tcp_streams(path, &s) == 0);
    if(!s.to_server.len) return;
    CHECK(decode_all(s.to_server.data, s.to_server.len, &client) == 0);
    CHECK(decode_all(s.to_client.data, s.to_client.len, &server) == 0);

    // every client packet in capture order; no pre-auth to save
    memset(&store, 0, sizeof(store));
    store.find         = captured_find;
    store.sub_accounts = self_sub_accounts;
    fesl_session_init(&session, &store, counter_random, &rnd);
    for(i = 0; i < client.count; i++) {
        CHECK(fesl_session_handle(&session, &client.pkt[i], D1_HELLO_TIME, &out) == FESL_SESSION_CONTINUE);
    }
    CHECK(session.account && !strcmp(session.account->name, "Tema567"));
    CHECK(decode_all(out.data, out.len, &ours) == 0);

    // Hello is answered first, then comes our MemCheck with number 0 while the
    // capture (made against an emulator) reused the Hello number
    CHECK(ours.count >= 2);
    if(ours.count >= 2) {
        captured = find_txn(&server, "Hello");
        CHECK(captured != NULL);
        if(captured) compare_answer(&ours.pkt[0], captured, exact_time ? NULL : hello_skip, 1);
        captured = find_txn(&server, "MemCheck");
        CHECK(captured != NULL);
        if(captured) compare_answer(&ours.pkt[1], captured, memcheck_skip, 0);
        CHECK(ours.pkt[1].number == 0);
        CHECK(ours.pkt[1].type == FESL_TYPE_SINGLE_SERVER);
        CHECK(strtoul(fesl_get(&ours.pkt[1], "salt"), NULL, 10) <= 0x7fffffff);

        // curTime keeps the captured format with escaped colons
        snprintf(curtime, sizeof(curtime), "%s", fesl_get(&ours.pkt[0], "curTime"));
        CHECK_STR(curtime, "\"Sep-03-2015 16%3a01%3a12 UTC\"");
    }

    // every captured request is handled now: no "not implemented" answers
    for(i = 0; i < ours.count; i++) {
        if(fesl_get(&ours.pkt[i], "errorCode")) CHECK_STR(fesl_get(&ours.pkt[i], "errorCode"), "3012");
    }
    CHECK(session.sub_account && !strcmp(session.sub_account->name, "Tema567"));

    // and the answers match the captured ones; the random values only in format
    for(i = 0; i < server.count; i++) {
        static const char   *lkey_skip[] = { "lkey", NULL },
                            *preauth_skip[] = { "challenge", "ticket", NULL };
        const char  *txn;

        captured = &server.pkt[i];
        txn = fesl_txn(captured);
        if(!strcmp(txn, "Hello") || !strcmp(txn, "MemCheck") || !strcmp(txn, "Ping")) continue;   // checked above
        mine = find_answer(&ours, captured->command, captured->number, txn);
        CHECK(mine != NULL);
        if(!mine) {
            fprintf(stderr, "no answer to %s #%u\n", txn, captured->number);
            continue;
        }
        matched++;
        if(!strcmp(txn, "Login")) {
            compare_answer(mine, captured, lkey_skip, 1);
            CHECK(strlen(fesl_get(mine, "lkey")) == FESL_LKEY_SIZE);
            CHECK(fesl_get(mine, "lkey")[FESL_LKEY_SIZE - 1] == '.');
            snprintf(login_lkey, sizeof(login_lkey), "%s", fesl_get(mine, "lkey"));
        } else if(!strcmp(txn, "LoginSubAccount")) {
            // a new key for the soldier
            compare_answer(mine, captured, lkey_skip, 1);
            CHECK(strlen(fesl_get(mine, "lkey")) == FESL_LKEY_SIZE);
            CHECK_STR(fesl_get(mine, "lkey"), session.lkey);
            CHECK(strcmp(fesl_get(mine, "lkey"), login_lkey));
        } else if(!strcmp(txn, "GameSpyPreAuth")) {
            compare_answer(mine, captured, preauth_skip, 1);
            CHECK(is_text(fesl_get(mine, "challenge"), FESL_CHALLENGE_SIZE, "abcdefghijklmnopqrstuvwxyz"));
            CHECK(is_text(fesl_get(mine, "ticket"), FESL_TICKET_SIZE,
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"));
        } else {
            // GetEntitlementByBundle, GetObjectInventory, GetSubAccounts,
            // GetAccount: every field, in order
            compare_answer(mine, captured, NULL, 1);
        }
    }
    // Login, 3 bundles, 3 inventories, GetSubAccounts, LoginSubAccount, GetAccount, GameSpyPreAuth
    CHECK(matched == 11);

    free_list(&ours);
    free_list(&client);
    free_list(&server);
    fesl_output_free(&out);
    fesl_accounts_free(&accounts);
    pcap_free_tcp_streams(&s);
}

// the server Ping must match the captured one byte for byte
static void test_ping(const char *path) {
    pcap_tcp_streams    s;
    packet_list server;
    fesl_output out = { 0 };
    size_t      i,
                off = 0,
                size = 0;

    CHECK(pcap_read_tcp_streams(path, &s) == 0);
    if(!s.to_client.len) return;
    CHECK(decode_all(s.to_client.data, s.to_client.len, &server) == 0);
    CHECK(fesl_session_ping(&out) == FESL_SESSION_CONTINUE);
    for(i = 0; i < server.count; i++) {
        size = fesl_encoded_size(&server.pkt[i]);
        if(!strcmp(fesl_txn(&server.pkt[i]), "Ping")) break;
        off += size;
    }
    CHECK(i < server.count);
    if(i < server.count) {
        CHECK(out.len == size);
        CHECK(!memcmp(out.data, s.to_client.data + off, size));
    }
    free_list(&server);
    fesl_output_free(&out);
    pcap_free_tcp_streams(&s);
}

static int build(fesl_packet *pkt, const char *command, uint32_t number, const char *data) {
    return fesl_packet_init(pkt, command, FESL_TYPE_SINGLE_SERVER, number, data, strlen(data));
}

// handles one request and returns its single answer in ans (count 0 if none)
static int handle_one(fesl_session *s, const char *command, uint32_t number, const char *data, packet_list *ans) {
    fesl_packet req;
    fesl_output out = { 0 };
    int         ret;

    CHECK(build(&req, command, number, data) == FESL_OK);
    ret = fesl_session_handle(s, &req, D1_HELLO_TIME, &out);
    fesl_packet_free(&req);
    CHECK(decode_all(out.data, out.len, ans) == 0);
    fesl_output_free(&out);
    return ret;
}

// field is the expected errorContainer field name, or NULL for none
static void check_field_error(const packet_list *ans, const char *command, uint32_t number, const char *txn, const char *code, const char *field) {
    CHECK(ans->count == 1);
    if(ans->count != 1) return;
    CHECK_STR(ans->pkt[0].command, command);
    CHECK(ans->pkt[0].number == number);
    CHECK_STR(fesl_txn(&ans->pkt[0]), txn);
    CHECK_STR(fesl_get(&ans->pkt[0], "errorCode"), code);
    CHECK(fesl_get(&ans->pkt[0], "localizedMessage") != NULL);
    if(field) {
        CHECK_STR(fesl_get(&ans->pkt[0], "errorContainer.[]"), "1");
        CHECK_STR(fesl_get(&ans->pkt[0], "errorContainer.0.fieldName"), field);
        CHECK_STR(fesl_get(&ans->pkt[0], "errorContainer.0.fieldError"), code);
    } else {
        CHECK_STR(fesl_get(&ans->pkt[0], "errorContainer.[]"), "0");
        CHECK(fesl_get(&ans->pkt[0], "errorContainer.0.fieldName") == NULL);
    }
}

static void check_error(const packet_list *ans, const char *command, uint32_t number, const char *txn, const char *code) {
    check_field_error(ans, command, number, txn, code, NULL);
}

static void test_session_errors(void) {
    fesl_accounts accounts;
    fesl_account_store store;
    fesl_session s;
    packet_list ans;
    uint32_t    rnd = 1;

    CHECK(fesl_accounts_parse(&accounts, ACCOUNTS) == 0);
    fesl_accounts_store_init(&store, &accounts);
    fesl_session_init(&s, &store, counter_random, &rnd);

    // Login before Hello
    CHECK(handle_one(&s, "acct", 2, "TXN=Login\nname=Tema567\npassword=123456\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 2, "Login", "99");
    CHECK(!s.account);
    free_list(&ans);

    CHECK(handle_one(&s, "fsys", 1, "TXN=Hello\nclientString=bf2142-pc\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 2);
    free_list(&ans);

    CHECK(handle_one(&s, "acct", 3, "TXN=Login\nname=nobody\npassword=123456\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 3, "Login", "101");
    free_list(&ans);

    CHECK(handle_one(&s, "acct", 4, "TXN=Login\nname=Tema567\npassword=wrong\n", &ans) == FESL_SESSION_CONTINUE);
    check_field_error(&ans, "acct", 4, "Login", "122", "password");
    CHECK(!s.account);
    free_list(&ans);

    CHECK(handle_one(&s, "acct", 5, "TXN=Login\nname=Tema567\n", &ans) == FESL_SESSION_CONTINUE);
    check_field_error(&ans, "acct", 5, "Login", "122", "password");
    free_list(&ans);

    CHECK(handle_one(&s, "acct", 6, "TXN=Login\nencryptedInfo=1111\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 6, "Login", "101");
    free_list(&ans);

    CHECK(handle_one(&s, "acct", 12, "TXN=Login\nname=\npassword=123456\n", &ans) == FESL_SESSION_CONTINUE);
    check_field_error(&ans, "acct", 12, "Login", "101", "name");
    free_list(&ans);
    CHECK(s.failed_logins == 4);

    // names are case-insensitive, no encryptedLoginInfo unless asked for
    CHECK(handle_one(&s, "acct", 7, "TXN=Login\nname=OTHER\npassword=secret\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1);
    if(ans.count == 1) {
        CHECK(fesl_get(&ans.pkt[0], "errorCode") == NULL);
        CHECK_STR(fesl_get(&ans.pkt[0], "userId"), "11");
        CHECK_STR(fesl_get(&ans.pkt[0], "displayName"), "other");
        CHECK(fesl_get(&ans.pkt[0], "encryptedLoginInfo") == NULL);
    }
    CHECK(s.account && (s.account->user_id == 11));
    CHECK(s.failed_logins == 0);    // reset by a successful login
    free_list(&ans);

    // a failed new login logs out
    CHECK(handle_one(&s, "acct", 8, "TXN=Login\nname=other\npassword=bad\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(!s.account);
    free_list(&ans);

    // the inventory is always empty for now
    CHECK(handle_one(&s, "dobj", 13, "TXN=GetObjectInventory\ndomainId=eagames\nsubdomainId=bf2142\n"
        "partitionKey=online_content\nobjectIds.[]=1\nobjectIds.0=bf2142_bp1\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1);
    if(ans.count == 1) {
        CHECK_STR(ans.pkt[0].command, "dobj");
        CHECK(ans.pkt[0].number == 13);
        CHECK_STR(fesl_txn(&ans.pkt[0]), "GetObjectInventory");
        CHECK_STR(fesl_get(&ans.pkt[0], "entitlements.[]"), "0");
        CHECK(ans.pkt[0].count == 2);
    }
    free_list(&ans);

    // not implemented yet
    CHECK(handle_one(&s, "subs", 9, "TXN=GetEntitlementByBundle\nbundleId=REG-PC-BF2142-UNLOCK-1\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "subs", 9, "GetEntitlementByBundle", "99");
    free_list(&ans);

    CHECK(handle_one(&s, "fsys", 10, "foo=bar\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "fsys", 10, "", "99");
    free_list(&ans);

    // answers to our own packets get nothing back
    CHECK(handle_one(&s, "fsys", 0, "TXN=MemCheck\nresult=\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 0);
    free_list(&ans);
    CHECK(handle_one(&s, "fsys", 0, "TXN=Ping\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 0);
    free_list(&ans);

    CHECK(handle_one(&s, "fsys", 11, "TXN=Goodbye\nreason=GOODBYE_CLIENT_NORMAL\n", &ans) == FESL_SESSION_CLOSE);
    CHECK(ans.count == 0);
    free_list(&ans);

    fesl_accounts_free(&accounts);
}

static void test_account_status(void) {
    static const struct {
        const char  *name,
                    *code;
    } cases[] = {
        { "dis", "102" },
        { "ban", "103" },
        { "pend", "105" }
    };
    fesl_accounts accounts;
    fesl_account_store store;
    fesl_session s;
    packet_list ans;
    uint32_t    rnd = 1;
    char        login[128];
    size_t      i;

    CHECK(fesl_accounts_parse(&accounts, "dis pw 1 disabled\nban pw 2 BANNED\npend pw 3 pending\nok pw 4 active\n") == 0);
    fesl_accounts_store_init(&store, &accounts);
    fesl_session_init(&s, &store, counter_random, &rnd);
    CHECK(handle_one(&s, "fsys", 1, "TXN=Hello\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);

    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        // the status is only revealed with the right password
        snprintf(login, sizeof(login), "TXN=Login\nname=%s\npassword=bad\n", cases[i].name);
        CHECK(handle_one(&s, "acct", 2, login, &ans) == FESL_SESSION_CONTINUE);
        check_field_error(&ans, "acct", 2, "Login", "122", "password");
        free_list(&ans);

        snprintf(login, sizeof(login), "TXN=Login\nname=%s\npassword=pw\n", cases[i].name);
        CHECK(handle_one(&s, "acct", 3, login, &ans) == FESL_SESSION_CONTINUE);
        check_error(&ans, "acct", 3, "Login", cases[i].code);
        CHECK(!s.account);
        free_list(&ans);
    }
    CHECK(s.failed_logins == 3);

    // after FESL_MAX_LOGIN_ATTEMPTS failures even the right password gets 121
    CHECK(handle_one(&s, "acct", 4, "TXN=Login\nname=nobody\npassword=pw\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 5, "TXN=Login\nname=ok\npassword=bad\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(s.failed_logins == FESL_MAX_LOGIN_ATTEMPTS);
    CHECK(handle_one(&s, "acct", 6, "TXN=Login\nname=ok\npassword=pw\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 6, "Login", "121");
    CHECK(!s.account);
    free_list(&ans);

    // a new connection starts over
    fesl_accounts_store_init(&store, &accounts);
    fesl_session_init(&s, &store, counter_random, &rnd);
    CHECK(handle_one(&s, "fsys", 1, "TXN=Hello\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 2, "TXN=Login\nname=ok\npassword=pw\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1 && !fesl_get(&ans.pkt[0], "errorCode"));
    CHECK(s.account && (s.account->status == ACCOUNT_ACTIVE));
    free_list(&ans);

    fesl_accounts_free(&accounts);
}

// a store whose database is down, then comes back
static int down_find(fesl_account_store *store, const char *name, fesl_account *out) {
    int     *up = store->ctx;

    if(!*up) return FESL_ACCOUNT_ERROR;
    if(strcmp(name, "Tema567")) return FESL_ACCOUNT_NOT_FOUND;
    memset(out, 0, sizeof(*out));
    strcpy(out->name, "Tema567");
    strcpy(out->password, "123456");
    out->user_id = 10000009;
    return FESL_ACCOUNT_FOUND;
}

// an e-mail with a newline must not add a field to the answer
static int odd_find(fesl_account_store *store, const char *name, fesl_account *out) {
    (void)store;
    (void)name;
    memset(out, 0, sizeof(*out));
    strcpy(out->name, "odd%name");
    strcpy(out->password, "pw");
    out->user_id = 7;
    strcpy(out->email, "a\nTXN=Login\r%b");
    strcpy(out->country, "US");
    return FESL_ACCOUNT_FOUND;
}

static void test_get_account(void) {
    fesl_accounts accounts;
    fesl_account_store store;
    fesl_session s;
    packet_list ans;
    uint32_t    rnd = 1;

    // only after a Login
    CHECK(fesl_accounts_parse(&accounts, ACCOUNTS) == 0);
    fesl_accounts_store_init(&store, &accounts);
    fesl_session_init(&s, &store, counter_random, &rnd);
    CHECK(handle_one(&s, "fsys", 1, "TXN=Hello\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 2, "TXN=GetAccount\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 2, "GetAccount", "99");
    free_list(&ans);

    // the accounts file has no profile: empty strings and zeros
    CHECK(handle_one(&s, "acct", 3, "TXN=Login\nname=other\npassword=secret\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 4, "TXN=GetAccount\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1);
    if(ans.count == 1) {
        CHECK(ans.pkt[0].number == 4);
        CHECK(ans.pkt[0].count == 14);
        CHECK_STR(fesl_get(&ans.pkt[0], "name"), "other");
        CHECK_STR(fesl_get(&ans.pkt[0], "profileID"), "11");
        CHECK_STR(fesl_get(&ans.pkt[0], "userId"), "11");
        CHECK_STR(fesl_get(&ans.pkt[0], "email"), "");
        CHECK_STR(fesl_get(&ans.pkt[0], "dobYear"), "0");
        CHECK_STR(fesl_get(&ans.pkt[0], "gender"), "U");
    }
    free_list(&ans);

    // a failed new Login logs out, so GetAccount fails again
    CHECK(handle_one(&s, "acct", 5, "TXN=Login\nname=other\npassword=bad\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 6, "TXN=GetAccount\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 6, "GetAccount", "99");
    free_list(&ans);
    fesl_accounts_free(&accounts);

    // text from the store is escaped, in Login and GetAccount
    memset(&store, 0, sizeof(store));
    store.find         = odd_find;
    store.sub_accounts = self_sub_accounts;
    fesl_session_init(&s, &store, counter_random, &rnd);
    CHECK(handle_one(&s, "fsys", 1, "TXN=Hello\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 2, "TXN=Login\nname=odd\npassword=pw\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1);
    if(ans.count == 1) CHECK_STR(fesl_get(&ans.pkt[0], "displayName"), "odd%25name");
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 3, "TXN=GetAccount\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1);
    if(ans.count == 1) {
        CHECK(ans.pkt[0].count == 14);
        CHECK_STR(fesl_txn(&ans.pkt[0]), "GetAccount");
        CHECK_STR(fesl_get(&ans.pkt[0], "email"), "a%0aTXN=Login%0d%25b");
        CHECK_STR(fesl_get(&ans.pkt[0], "name"), "odd%25name");
    }
    free_list(&ans);
}

// an account with three soldiers, one of them with a name to escape;
// sub_down makes the sub-account lookup fail, and the pre-auth is recorded
static int  sub_down = 0,
            preauth_calls = 0,
            preauth_result = FESL_ACCOUNT_FOUND;
static char preauth_challenge[FESL_CHALLENGE_SIZE + 1],
            preauth_ticket[FESL_TICKET_SIZE + 1];
static uint32_t preauth_id;

static int three_sub_accounts(fesl_account_store *store, const fesl_account *acc,
        fesl_sub_account *list, size_t max, size_t *count) {
    static const char   *names[] = { "Alpha", "Bravo", "Char%lie" };
    size_t  i;

    (void)store;
    (void)acc;
    *count = 0;
    if(sub_down) return FESL_ACCOUNT_ERROR;
    for(i = 0; (i < 3) && (i < max); i++) {
        memset(&list[i], 0, sizeof(list[i]));
        list[i].id = 20000 + i;
        strcpy(list[i].name, names[i]);
        (*count)++;
    }
    return FESL_ACCOUNT_FOUND;
}

static int record_preauth(fesl_account_store *store, const fesl_sub_account *sub, const char *challenge, const char *ticket) {
    (void)store;
    preauth_calls++;
    preauth_id = sub->id;
    snprintf(preauth_challenge, sizeof(preauth_challenge), "%s", challenge);
    snprintf(preauth_ticket, sizeof(preauth_ticket), "%s", ticket);
    return preauth_result;
}

static void test_sub_accounts(void) {
    fesl_accounts accounts;
    fesl_account_store store;
    fesl_session s;
    packet_list ans;
    uint32_t    rnd = 1;
    char        lkey[FESL_LKEY_SIZE + 1];

    CHECK(fesl_accounts_parse(&accounts, ACCOUNTS) == 0);
    fesl_accounts_store_init(&store, &accounts);
    store.sub_accounts = three_sub_accounts;
    store.save_preauth = record_preauth;
    fesl_session_init(&s, &store, counter_random, &rnd);
    CHECK(handle_one(&s, "fsys", 1, "TXN=Hello\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);

    // nothing before a Login, no pre-auth before a LoginSubAccount
    CHECK(handle_one(&s, "subs", 2, "TXN=GetEntitlementByBundle\nbundleId=REG-PC-BF2142-UNLOCK-1\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "subs", 2, "GetEntitlementByBundle", "99");
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 3, "TXN=GetSubAccounts\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 3, "GetSubAccounts", "99");
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 4, "TXN=LoginSubAccount\nname=Alpha\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 4, "LoginSubAccount", "99");
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 5, "TXN=Login\nname=other\npassword=secret\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 6, "TXN=GameSpyPreAuth\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 6, "GameSpyPreAuth", "99");
    free_list(&ans);
    CHECK(preauth_calls == 0);

    // unlock bundles: never entitled, unquoted message as captured
    CHECK(handle_one(&s, "subs", 7, "TXN=GetEntitlementByBundle\nbundleId=REG-PC-BF2142-UNLOCK-2\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "subs", 7, "GetEntitlementByBundle", "3012");
    if(ans.count == 1) CHECK_STR(fesl_get(&ans.pkt[0], "localizedMessage"), "The customer has never had entitlement for this bundle.");
    free_list(&ans);

    // the list, in order, escaped, with the count last
    CHECK(handle_one(&s, "acct", 8, "TXN=GetSubAccounts\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1);
    if(ans.count == 1) {
        CHECK(ans.pkt[0].count == 5);
        CHECK_STR(ans.pkt[0].pairs[1].key, "subAccounts.0");
        CHECK_STR(ans.pkt[0].pairs[1].value, "Alpha");
        CHECK_STR(fesl_get(&ans.pkt[0], "subAccounts.1"), "Bravo");
        CHECK_STR(fesl_get(&ans.pkt[0], "subAccounts.2"), "Char%25lie");
        CHECK_STR(ans.pkt[0].pairs[4].key, "subAccounts.[]");
        CHECK_STR(ans.pkt[0].pairs[4].value, "3");
    }
    free_list(&ans);

    // only a soldier of the account, case-insensitively
    snprintf(lkey, sizeof(lkey), "%s", s.lkey);
    CHECK(handle_one(&s, "acct", 9, "TXN=LoginSubAccount\nname=Delta\n", &ans) == FESL_SESSION_CONTINUE);
    check_field_error(&ans, "acct", 9, "LoginSubAccount", "101", "name");
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 10, "TXN=LoginSubAccount\n", &ans) == FESL_SESSION_CONTINUE);
    check_field_error(&ans, "acct", 10, "LoginSubAccount", "101", "name");
    free_list(&ans);
    CHECK(!s.sub_account);
    CHECK(handle_one(&s, "acct", 11, "TXN=LoginSubAccount\nname=bravo\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1);
    if(ans.count == 1) {
        CHECK(!fesl_get(&ans.pkt[0], "errorCode"));
        CHECK_STR(fesl_get(&ans.pkt[0], "profileId"), "20001");    // the soldier
        CHECK_STR(fesl_get(&ans.pkt[0], "userId"), "11");          // the account
        CHECK_STR(fesl_get(&ans.pkt[0], "lkey"), s.lkey);
        CHECK(strcmp(s.lkey, lkey));
    }
    CHECK(s.sub_account && (s.sub_account->id == 20001) && !strcmp(s.sub_account->name, "Bravo"));
    free_list(&ans);

    // the pre-auth answered is the one saved, even when saving fails
    CHECK(handle_one(&s, "acct", 12, "TXN=GameSpyPreAuth\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1);
    if(ans.count == 1) {
        CHECK(ans.pkt[0].count == 3);
        CHECK_STR(fesl_get(&ans.pkt[0], "challenge"), preauth_challenge);
        CHECK_STR(fesl_get(&ans.pkt[0], "ticket"), preauth_ticket);
    }
    CHECK((preauth_calls == 1) && (preauth_id == 20001));
    free_list(&ans);
    preauth_result = FESL_ACCOUNT_ERROR;
    CHECK(handle_one(&s, "acct", 13, "TXN=GameSpyPreAuth\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1 && !fesl_get(&ans.pkt[0], "errorCode"));
    if(ans.count == 1) CHECK_STR(fesl_get(&ans.pkt[0], "ticket"), preauth_ticket);  // the store got the new one
    CHECK(preauth_calls == 2);
    free_list(&ans);
    preauth_result = FESL_ACCOUNT_FOUND;

    // a store error is a system error; a failed LoginSubAccount logs the soldier out
    sub_down = 1;
    CHECK(handle_one(&s, "acct", 14, "TXN=GetSubAccounts\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 14, "GetSubAccounts", "99");
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 15, "TXN=LoginSubAccount\nname=Alpha\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 15, "LoginSubAccount", "99");
    free_list(&ans);
    CHECK(!s.sub_account);
    sub_down = 0;

    // a new Login logs the soldier out too
    CHECK(handle_one(&s, "acct", 16, "TXN=LoginSubAccount\nname=Alpha\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(s.sub_account && (s.sub_account->id == 20000));
    CHECK(handle_one(&s, "acct", 17, "TXN=Login\nname=other\npassword=secret\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(!s.sub_account);
    CHECK(handle_one(&s, "acct", 18, "TXN=GameSpyPreAuth\n", &ans) == FESL_SESSION_CONTINUE);
    check_error(&ans, "acct", 18, "GameSpyPreAuth", "99");
    free_list(&ans);

    // the accounts file: one soldier, the account itself, and no pre-auth to save
    fesl_accounts_store_init(&store, &accounts);
    fesl_session_init(&s, &store, counter_random, &rnd);
    CHECK(handle_one(&s, "fsys", 1, "TXN=Hello\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 2, "TXN=Login\nname=Tema567\npassword=123456\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 3, "TXN=GetSubAccounts\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1 && (ans.pkt[0].count == 3));
    if(ans.count == 1) CHECK_STR(fesl_get(&ans.pkt[0], "subAccounts.0"), "Tema567");
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 4, "TXN=LoginSubAccount\nname=Tema567\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1);
    if(ans.count == 1) {
        CHECK_STR(fesl_get(&ans.pkt[0], "profileId"), "10");
        CHECK_STR(fesl_get(&ans.pkt[0], "userId"), "10");
    }
    free_list(&ans);
    CHECK(handle_one(&s, "acct", 5, "TXN=GameSpyPreAuth\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1 && !fesl_get(&ans.pkt[0], "errorCode"));
    free_list(&ans);
    CHECK(preauth_calls == 2);      // nothing to save
    fesl_accounts_free(&accounts);
}

static void test_store_errors(void) {
    fesl_account_store store = { .find = down_find, .sub_accounts = self_sub_accounts };
    fesl_session s;
    packet_list ans;
    uint32_t    rnd = 1;
    int         up = 0,
                i;

    store.ctx = &up;
    fesl_session_init(&s, &store, counter_random, &rnd);
    CHECK(handle_one(&s, "fsys", 1, "TXN=Hello\n", &ans) == FESL_SESSION_CONTINUE);
    free_list(&ans);

    // a store error is a system error and not a failed attempt
    for(i = 0; i < FESL_MAX_LOGIN_ATTEMPTS + 2; i++) {
        CHECK(handle_one(&s, "acct", 2, "TXN=Login\nname=Tema567\npassword=123456\n", &ans) == FESL_SESSION_CONTINUE);
        check_error(&ans, "acct", 2, "Login", "99");
        free_list(&ans);
    }
    CHECK(!s.account);
    CHECK(s.failed_logins == 0);

    up = 1;
    CHECK(handle_one(&s, "acct", 3, "TXN=Login\nname=Tema567\npassword=123456\n", &ans) == FESL_SESSION_CONTINUE);
    CHECK(ans.count == 1 && !fesl_get(&ans.pkt[0], "errorCode"));
    if(ans.count == 1) CHECK_STR(fesl_get(&ans.pkt[0], "userId"), "10000009");
    CHECK(s.account && (s.account->user_id == 10000009));
    free_list(&ans);
}

static void test_accounts(void) {
    fesl_accounts a;
    char        longname[64];

    CHECK(fesl_accounts_parse(&a, "") == 0);
    CHECK(a.count == 0);
    fesl_accounts_free(&a);

    CHECK(fesl_accounts_parse(&a, "\n# only comments\n   \r\n  # indented\n") == 0);
    CHECK(a.count == 0);
    fesl_accounts_free(&a);

    CHECK(fesl_accounts_parse(&a, "a pw 1\r\n\tb\tpw2\t2 #x\nc pw3 4294967295 Pending\r\n") == 0);
    CHECK(a.count == 3);
    if(a.count == 3) {
        CHECK(a.list[0].status == ACCOUNT_ACTIVE);
        CHECK(a.list[2].status == ACCOUNT_PENDING);
        CHECK_STR(a.list[0].name, "a");
        CHECK_STR(a.list[0].password, "pw");
        CHECK(a.list[0].user_id == 1);
        CHECK_STR(a.list[1].password, "pw2");
        CHECK(a.list[2].user_id == 4294967295u);
    }
    CHECK(fesl_accounts_find(&a, "B") == &a.list[1]);
    CHECK(fesl_accounts_find(&a, "d") == NULL);
    fesl_accounts_free(&a);

    // the 1-based number of the first bad line is returned
    CHECK(fesl_accounts_parse(&a, "a pw 1\nb pw\n") == 2);
    CHECK(fesl_accounts_parse(&a, "a pw 1 extra\n") == 1);     // unknown status
    CHECK(fesl_accounts_parse(&a, "a pw 1 x\n") == 1);
    CHECK(fesl_accounts_parse(&a, "a pw 1 banned extra\n") == 1);
    CHECK(fesl_accounts_parse(&a, "a pw 1 averyveryverylongstatus\n") == 1);
    CHECK(fesl_accounts_parse(&a, "a pw 0\n") == 1);
    CHECK(fesl_accounts_parse(&a, "a pw 12x\n") == 1);
    CHECK(fesl_accounts_parse(&a, "a pw 4294967296\n") == 1);
    CHECK(fesl_accounts_parse(&a, "# c\na pw 1\nA pw 2\n") == 3);   // duplicate
    memset(longname, 'n', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = 0;
    CHECK(sizeof(longname) - 1 > ACCOUNT_NAME_SIZE);
    {
        char    line[128];

        snprintf(line, sizeof(line), "%s pw 1\n", longname);
        CHECK(fesl_accounts_parse(&a, line) == 1);
    }
    CHECK(a.count == 0);
    CHECK(fesl_accounts_load(&a, "/nonexistent/accounts.txt") == -1);
}

int main(int argc, char *argv[]) {
    const char  *dir = (argc > 1) ? argv[1] : "data";
    char        path[1024];

    test_accounts();
    test_session_errors();
    test_account_status();
    test_store_errors();
    test_get_account();
    test_sub_accounts();

    snprintf(path, sizeof(path), "%s/d1/dump2_ssl.pcap", dir);
    test_replay(path, 1);
    snprintf(path, sizeof(path), "%s/d2/dump2_ssl.pcap", dir);
    test_replay(path, 0);   // answered at another time
    test_ping(path);        // only d2 has the server Ping

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
