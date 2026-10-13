/*
    FESL decoder tests: unit tests plus replay of the decrypted captures

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fesl.h"
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
        if(!a_ || strcmp(a_, b_)) { \
            g_failures++; \
            fprintf(stderr, "%s:%d: %s is \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #A, a_ ? a_ : "(null)", b_); \
        } \
    } while(0)

static size_t make_packet(uint8_t *out, const char *cmd, uint32_t word, const char *data, size_t datalen) {
    uint32_t    size = FESL_HEADER_SIZE + datalen;

    memcpy(out, cmd, 4);
    out[4] = word >> 24; out[5] = word >> 16; out[6] = word >> 8; out[7] = word;
    out[8] = size >> 24; out[9] = size >> 16; out[10] = size >> 8; out[11] = size;
    memcpy(out + FESL_HEADER_SIZE, data, datalen);
    return size;
}

static void test_decode_basic(void) {
    static const char   data[] = "TXN=Hello\nclientString=bf2142-pc\nresult=\n";   // sizeof includes the NUL
    uint8_t     buf[256],
                enc[256];
    size_t      size,
                i,
                consumed = 0;
    fesl_packet pkt;

    size = make_packet(buf, "fsys", 0x80000001, data, sizeof(data));

    for(i = 0; i < size; i++) {
        CHECK(fesl_decode(buf, i, &pkt, &consumed) == FESL_NEED_MORE);
    }
    CHECK(fesl_decode(buf, size, &pkt, &consumed) == FESL_OK);
    CHECK(consumed == size);
    CHECK_STR(pkt.command, "fsys");
    CHECK(pkt.type == FESL_TYPE_SINGLE_SERVER);
    CHECK(pkt.number == 1);
    CHECK(pkt.count == 3);
    CHECK_STR(fesl_txn(&pkt), "Hello");
    CHECK_STR(fesl_get(&pkt, "clientString"), "bf2142-pc");
    CHECK_STR(fesl_get(&pkt, "result"), "");
    CHECK(fesl_get(&pkt, "missing") == NULL);

    CHECK(fesl_encoded_size(&pkt) == size);
    CHECK(fesl_encode(&pkt, enc, sizeof(enc)) == (int)size);
    CHECK(!memcmp(enc, buf, size));
    CHECK(fesl_encode(&pkt, enc, size - 1) == FESL_ERR_SIZE);
    fesl_packet_free(&pkt);
}

static void test_decode_errors(void) {
    uint8_t     buf[64];
    fesl_packet pkt;

    make_packet(buf, "fsys", 0x80000001, "", 0);
    buf[11] = 11;   // smaller than the header
    CHECK(fesl_decode(buf, sizeof(buf), &pkt, NULL) == FESL_ERR_SIZE);

    buf[8] = 0x7f;  // absurdly big
    CHECK(fesl_decode(buf, sizeof(buf), &pkt, NULL) == FESL_ERR_SIZE);
}

static void test_decode_lenient(void) {
    static const char   data[] = "\nTXN=Ping\n\nflag\nk=a=b";    // empty lines, no '=', no final newline
    uint8_t     buf[64];
    fesl_packet pkt;

    make_packet(buf, "fsys", 0, data, sizeof(data) - 1);   // no terminating NUL either
    CHECK(fesl_decode(buf, FESL_HEADER_SIZE + sizeof(data) - 1, &pkt, NULL) == FESL_OK);
    CHECK(pkt.type == 0);
    CHECK(pkt.count == 3);
    CHECK_STR(fesl_txn(&pkt), "Ping");
    CHECK_STR(fesl_get(&pkt, "flag"), "");
    CHECK_STR(fesl_get(&pkt, "k"), "a=b");
    fesl_packet_free(&pkt);
}

static void test_url_decode(void) {
    char    s1[] = "Sep-03-2015 16%3a01%3a12 UTC",
            s2[] = "abc%3D%3d",
            s3[] = "100%",
            s4[] = "%zz%4";

    CHECK(fesl_url_decode(s1) == strlen("Sep-03-2015 16:01:12 UTC"));
    CHECK_STR(s1, "Sep-03-2015 16:01:12 UTC");
    fesl_url_decode(s2);
    CHECK_STR(s2, "abc==");
    fesl_url_decode(s3);
    CHECK_STR(s3, "100%");
    fesl_url_decode(s4);
    CHECK_STR(s4, "%zz%4");
}

static size_t b64_encode(const uint8_t *in, size_t len, char *out) {
    static const char   tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t      i,
                o = 0;
    uint32_t    v;

    for(i = 0; i < len; i += 3) {
        v = in[i] << 16;
        if(i + 1 < len) v |= in[i + 1] << 8;
        if(i + 2 < len) v |= in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tab[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tab[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

// escapes '=' like EA does inside the base64 pieces
static void escape_b64(const char *in, size_t len, char *out) {
    size_t  i;

    for(i = 0; i < len; i++) {
        if(in[i] == '=') {
            memcpy(out, "%3d", 3);
            out += 3;
        } else {
            *out++ = in[i];
        }
    }
    *out = 0;
}

static void test_multipart(void) {
    char        *orig,
                *b64,
                piece[256],
                partdata[512],
                key[32];
    size_t      origlen = 0,
                b64len,
                off,
                n,
                i;
    int         ret = FESL_NEED_MORE,
                parts = 0;
    fesl_multipart  mp;
    fesl_packet part,
                pkt;

    // 2 bytes past a multiple of 3 so the base64 ends with '='
    orig = malloc(8192);
    origlen = sprintf(orig, "TXN=GetStats\n");
    for(i = 0; i < 300; i++) origlen += sprintf(orig + origlen, "stats.%u.value=%u\n", (unsigned)i, (unsigned)(i * 7));
    while((origlen % 3) != 2) origlen += sprintf(orig + origlen, "x\n");
    b64 = malloc(origlen * 2);
    b64len = b64_encode((uint8_t *)orig, origlen, b64);
    CHECK(b64[b64len - 1] == '=');

    fesl_multipart_init(&mp);
    for(off = 0; off < b64len; off += n) {
        n = b64len - off;
        if(n > 100) n = 100;
        escape_b64(b64 + off, n, piece);
        sprintf(partdata, "data=%s\nsize=%u\n", piece, (unsigned)b64len);
        CHECK(fesl_packet_init(&part, "rank", FESL_TYPE_MULTI_SERVER, 7, partdata, strlen(partdata)) == FESL_OK);
        CHECK(ret == FESL_NEED_MORE);   // nothing completed before the last piece
        ret = fesl_multipart_add(&mp, &part, &pkt);
        fesl_packet_free(&part);
        parts++;
    }
    CHECK(parts > 10);
    CHECK(ret == FESL_OK);
    if(ret == FESL_OK) {
        CHECK_STR(pkt.command, "rank");
        CHECK(pkt.type == FESL_TYPE_SINGLE_SERVER);
        CHECK(pkt.number == 7);
        CHECK_STR(fesl_txn(&pkt), "GetStats");
        CHECK_STR(fesl_get(&pkt, "stats.0.value"), "0");
        CHECK_STR(fesl_get(&pkt, "stats.299.value"), "2093");
        snprintf(key, sizeof(key), "stats.%u.value", 150);
        CHECK_STR(fesl_get(&pkt, key), "1050");
        fesl_packet_free(&pkt);
    }
    CHECK(!mp.active);

    // a piece of another transaction in the middle is rejected
    sprintf(partdata, "data=QUJD\nsize=8\n");
    CHECK(fesl_packet_init(&part, "rank", FESL_TYPE_MULTI_SERVER, 7, partdata, strlen(partdata)) == FESL_OK);
    CHECK(fesl_multipart_add(&mp, &part, &pkt) == FESL_NEED_MORE);
    fesl_packet_free(&part);
    CHECK(fesl_packet_init(&part, "rank", FESL_TYPE_MULTI_SERVER, 8, partdata, strlen(partdata)) == FESL_OK);
    CHECK(fesl_multipart_add(&mp, &part, &pkt) == FESL_ERR_FORMAT);
    fesl_packet_free(&part);

    // single packets aren't multipart pieces
    CHECK(fesl_packet_init(&part, "rank", FESL_TYPE_SINGLE_SERVER, 7, partdata, strlen(partdata)) == FESL_OK);
    CHECK(fesl_multipart_add(&mp, &part, &pkt) == FESL_ERR_FORMAT);
    fesl_packet_free(&part);

    fesl_multipart_free(&mp);
    free(b64);
    free(orig);
}

typedef struct {
    fesl_packet *pkt;
    size_t      count;
} packet_list;

// decodes a whole stream, every packet must re-encode to the same bytes
static int decode_stream(const pcap_buffer *s, packet_list *list) {
    size_t  off,
            consumed,
            cap = 0;
    uint8_t *enc;
    int     ret;

    memset(list, 0, sizeof(packet_list));
    enc = malloc(FESL_MAX_PACKET_SIZE);
    for(off = 0; off < s->len; off += consumed) {
        if(list->count == cap) {
            cap = cap ? (cap * 2) : 16;
            list->pkt = realloc(list->pkt, cap * sizeof(fesl_packet));
        }
        ret = fesl_decode(s->data + off, s->len - off, &list->pkt[list->count], &consumed);
        if(ret != FESL_OK) {
            fprintf(stderr, "decode error %d at stream offset %u\n", ret, (unsigned)off);
            free(enc);
            return -1;
        }
        CHECK(fesl_encode(&list->pkt[list->count], enc, FESL_MAX_PACKET_SIZE) == (int)consumed);
        CHECK(!memcmp(enc, s->data + off, consumed));
        CHECK(fesl_txn(&list->pkt[list->count]) != NULL);
        list->count++;
    }
    free(enc);
    return 0;
}

// feeds the stream one byte at a time like a slow socket
static size_t count_packets_bytewise(const pcap_buffer *s) {
    size_t      start = 0,
                avail,
                consumed,
                count = 0;
    fesl_packet pkt;
    int         ret;

    for(avail = 1; avail <= s->len; avail++) {
        ret = fesl_decode(s->data + start, avail - start, &pkt, &consumed);
        if(ret == FESL_NEED_MORE) continue;
        if(ret != FESL_OK) return 0;
        fesl_packet_free(&pkt);
        start += consumed;
        count++;
    }
    return (start == s->len) ? count : 0;
}

static void free_list(packet_list *list) {
    size_t  i;

    for(i = 0; i < list->count; i++) fesl_packet_free(&list->pkt[i]);
    free(list->pkt);
}

static const fesl_packet *find_txn(const packet_list *list, const char *txn) {
    size_t  i;

    for(i = 0; i < list->count; i++) {
        if(!strcmp(fesl_txn(&list->pkt[i]), txn)) return &list->pkt[i];
    }
    return NULL;
}

static void test_replay(const char *path, size_t expected_packets) {
    pcap_tcp_streams    s;
    packet_list client,
                server;
    const fesl_packet   *req,
                *ans;
    char        curtime[64];
    size_t      i,
                j;

    printf("replay %s\n", path);
    CHECK(pcap_read_tcp_streams(path, &s) == 0);
    if(!s.to_server.len) return;
    CHECK(s.server_port == 18300);

    CHECK(decode_stream(&s.to_server, &client) == 0);
    CHECK(decode_stream(&s.to_client, &server) == 0);
    CHECK(client.count == expected_packets);
    CHECK(server.count == expected_packets);
    CHECK(count_packets_bytewise(&s.to_server) == client.count);
    CHECK(count_packets_bytewise(&s.to_client) == server.count);

    // every numbered client request gets an answer with the same number, command and TXN
    for(i = 0; i < client.count; i++) {
        req = &client.pkt[i];
        if(!req->number) continue;
        for(j = 0; j < server.count; j++) {
            ans = &server.pkt[j];
            if((ans->number == req->number) && !strcmp(fesl_txn(ans), fesl_txn(req))) break;
        }
        CHECK(j < server.count);
        if(j < server.count) CHECK_STR(ans->command, req->command);
    }

    // the session starts with the client Hello
    req = &client.pkt[0];
    CHECK_STR(req->command, "fsys");
    CHECK(req->type == FESL_TYPE_SINGLE_SERVER);
    CHECK(req->number == 1);
    CHECK_STR(fesl_txn(req), "Hello");
    CHECK_STR(fesl_get(req, "clientString"), "bf2142-pc");
    CHECK_STR(fesl_get(req, "sku"), "125170");
    CHECK_STR(fesl_get(req, "protocolVersion"), "2.0");

    ans = find_txn(&server, "Hello");
    CHECK(ans != NULL);
    if(ans) {
        CHECK_STR(fesl_get(ans, "domainPartition.domain"), "eagames");
        CHECK_STR(fesl_get(ans, "domainPartition.subDomain"), "battlefield2142-2006");
        CHECK(fesl_get(ans, "curTime") != NULL);
        if(fesl_get(ans, "curTime")) {
            snprintf(curtime, sizeof(curtime), "%s", fesl_get(ans, "curTime"));
            fesl_url_decode(curtime);
            CHECK(!strncmp(curtime, "\"Sep-03-2015 ", 13) && strchr(curtime, ':') && !strchr(curtime, '%'));
        }
    }

    // the server MemCheck is sent unsolicited, the client answers with number 0
    ans = find_txn(&server, "MemCheck");
    req = find_txn(&client, "MemCheck");
    CHECK(ans && req && (req->number == 0));

    req = find_txn(&client, "Login");
    ans = find_txn(&server, "Login");
    CHECK(req && ans);
    if(req && ans) {
        CHECK_STR(req->command, "acct");
        CHECK_STR(fesl_get(req, "name"), "Tema567");
        CHECK_STR(fesl_get(ans, "displayName"), "Tema567");
        CHECK_STR(fesl_get(ans, "profileId"), "10");
        CHECK(fesl_get(ans, "lkey") != NULL);
    }

    CHECK(find_txn(&client, "GameSpyPreAuth") && find_txn(&server, "GameSpyPreAuth"));

    free_list(&client);
    free_list(&server);
    pcap_free_tcp_streams(&s);
}

int main(int argc, char *argv[]) {
    const char  *dir = (argc > 1) ? argv[1] : "data";
    char        path[1024];

    test_decode_basic();
    test_decode_errors();
    test_decode_lenient();
    test_url_decode();
    test_multipart();

    snprintf(path, sizeof(path), "%s/d1/dump2_ssl.pcap", dir);
    test_replay(path, 13);
    snprintf(path, sizeof(path), "%s/d2/dump2_ssl.pcap", dir);
    test_replay(path, 14);     // d2 also has a Ping

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
