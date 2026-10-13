/*
    FESL server for Battlefield 2142

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Handles Hello, Login and GetObjectInventory, everything else gets a
    "Not implemented" error. The game uses SSLv3, so the server must be built
    with "make ssl3" to talk to it; "-X tls" accepts any TLS version for
    testing, and "-X none" speaks plain TCP to clients patched to skip SSL.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include "fesl.h"
#include "fesl_session.h"
#include "accounts.h"

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    #error "OpenSSL 1.1.0 or newer is needed"
#endif

#define DEFAULT_PORT            18300
#define DEFAULT_PING_INTERVAL   30      // seconds, retail sends a Ping this often even to busy clients
#define MAX_SILENT_INTERVALS    3       // close after this many ping intervals without client data
#define HANDSHAKE_TIMEOUT       30      // seconds

typedef struct {
    SSL_CTX     *ctx;           // NULL for plain TCP
    fesl_accounts accounts;
    int         ping_interval;
} server;

typedef struct {
    const server *srv;
    int         sd;
    char        peer[64];
} connection;

static uint32_t ssl_random(void *ctx) {
    uint32_t    n = 0;

    (void)ctx;
    if(RAND_bytes((unsigned char *)&n, sizeof(n)) != 1) {
        fprintf(stderr, "Error: RAND_bytes failed\n");
        abort();
    }
    return n;
}

static void print_ssl_errors(const char *peer) {
    unsigned long   e;
    char            buf[256];

    while((e = ERR_get_error())) {
        ERR_error_string_n(e, buf, sizeof(buf));
        printf("%s: %s\n", peer, buf);
    }
}

// the client socket, with or without SSL
typedef struct {
    int         sd;
    SSL         *ssl;
} channel;

// returns > 0 bytes, 0 on timeout, < 0 on close or error
static int channel_read(channel *ch, uint8_t *buf, size_t len) {
    int     n,
            err;

    if(ch->ssl) {
        n = SSL_read(ch->ssl, buf, len);
        if(n > 0) return n;
        err = SSL_get_error(ch->ssl, n);
        if((err == SSL_ERROR_WANT_READ) ||
          ((err == SSL_ERROR_SYSCALL) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))) return 0;
        return -1;
    }
    n = recv(ch->sd, buf, len, 0);
    if(n > 0) return n;
    if((n < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))) return 0;
    return -1;
}

static int channel_write_all(channel *ch, const fesl_output *out) {
    size_t  off;
    int     n;

    for(off = 0; off < out->len; off += n) {
        if(ch->ssl) n = SSL_write(ch->ssl, out->data + off, out->len - off);
        else        n = send(ch->sd, out->data + off, out->len - off, 0);
        if(n <= 0) return -1;
    }
    return 0;
}

static double now_seconds(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// logs the packets without their values, Login carries the password
static void log_packets(const char *peer, const char *dir, const uint8_t *data, size_t len) {
    fesl_packet pkt;
    size_t      consumed;

    while(fesl_decode(data, len, &pkt, &consumed) == FESL_OK) {
        printf("%s %s %s %s #%u%s%s\n", peer, dir, pkt.command, fesl_txn(&pkt) ? fesl_txn(&pkt) : "?", pkt.number,
            fesl_get(&pkt, "errorCode") ? " error " : "",
            fesl_get(&pkt, "errorCode") ? fesl_get(&pkt, "errorCode") : "");
        fesl_packet_free(&pkt);
        data += consumed;
        len  -= consumed;
    }
}

static void *client_thread(void *arg) {
    connection  *c = arg;
    channel     ch = { .sd = c->sd, .ssl = NULL };
    uint8_t     *buf = NULL;
    size_t      len = 0,
                consumed;
    fesl_session session;
    fesl_output out = { 0 };
    fesl_packet pkt;
    struct timeval tv;
    double      now,
                next_ping,
                last_rx;
    int         n,
                ret,
                logged_in,
                done = 0;

    printf("%s: connected\n", c->peer);
    buf = malloc(FESL_MAX_PACKET_SIZE);
    if(!buf) goto quit;

    tv.tv_sec  = HANDSHAKE_TIMEOUT;
    tv.tv_usec = 0;
    setsockopt(c->sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if(c->srv->ctx) {
        ch.ssl = SSL_new(c->srv->ctx);
        if(!ch.ssl) goto quit;
        SSL_set_fd(ch.ssl, c->sd);
        if(SSL_accept(ch.ssl) != 1) {
            printf("%s: SSL handshake failed\n", c->peer);
            print_ssl_errors(c->peer);
            goto quit;
        }
        printf("%s: %s %s\n", c->peer, SSL_get_version(ch.ssl), SSL_get_cipher_name(ch.ssl));
    }
    fesl_session_init(&session, &c->srv->accounts, ssl_random, NULL);

    // short receive timeout so pings go out on time even while idle
    tv.tv_sec  = 1;
    setsockopt(c->sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    last_rx   = now_seconds();
    next_ping = last_rx + c->srv->ping_interval;
    while(!done) {
        n = channel_read(&ch, buf + len, FESL_MAX_PACKET_SIZE - len);
        if(n < 0) break;    // closed or error
        now = now_seconds();

        if(n > 0) {
            last_rx = now;
            len += n;
            fesl_output_reset(&out);
            for(;;) {
                ret = fesl_decode(buf, len, &pkt, &consumed);
                if(ret == FESL_NEED_MORE) break;
                if(ret != FESL_OK) {
                    printf("%s: invalid packet\n", c->peer);
                    done = 1;
                    break;
                }
                log_packets(c->peer, ">", buf, consumed);
                memmove(buf, buf + consumed, len - consumed);
                len -= consumed;

                logged_in = !strcmp(pkt.command, "acct") && fesl_txn(&pkt) && !strcmp(fesl_txn(&pkt), "Login");
                ret = fesl_session_handle(&session, &pkt, time(NULL), &out);
                if(logged_in && session.account) printf("%s: logged in as %s\n", c->peer, session.account->name);
                fesl_packet_free(&pkt);
                if(ret != FESL_SESSION_CONTINUE) {
                    done = 1;
                    break;
                }
            }
            if(out.len) {
                log_packets(c->peer, "<", out.data, out.len);
                if(channel_write_all(&ch, &out) < 0) break;
            }
        } else if(now - last_rx >= c->srv->ping_interval * MAX_SILENT_INTERVALS) {
            printf("%s: timeout\n", c->peer);
            break;
        }

        if(!done && (now >= next_ping)) {
            fesl_output_reset(&out);
            if((fesl_session_ping(&out) != FESL_SESSION_CONTINUE) || (channel_write_all(&ch, &out) < 0)) break;
            log_packets(c->peer, "<", out.data, out.len);
            next_ping = now + c->srv->ping_interval;
        }
    }

quit:
    printf("%s: disconnected\n", c->peer);
    if(ch.ssl) {
        SSL_shutdown(ch.ssl);
        SSL_free(ch.ssl);
    }
    close(c->sd);
    fesl_output_free(&out);
    free(buf);
    free(c);
    return NULL;
}

static SSL_CTX *create_ssl_ctx(const char *cert_file, const char *method) {
    SSL_CTX     *ctx;

    ctx = SSL_CTX_new(TLS_server_method());
    if(!ctx) return NULL;

    // security level 0 allows the old protocols, ciphers and keys of the game
    SSL_CTX_set_cipher_list(ctx, "ALL:@SECLEVEL=0");
    SSL_CTX_set_options(ctx, SSL_OP_ALL);
    if(!strcmp(method, "ssl3")) {
#ifdef OPENSSL_NO_SSL3
        fprintf(stderr, "Error: this OpenSSL has no SSLv3 support, build with \"make ssl3\" or use -X tls\n");
        SSL_CTX_free(ctx);
        return NULL;
#else
        SSL_CTX_set_min_proto_version(ctx, SSL3_VERSION);
        SSL_CTX_set_max_proto_version(ctx, SSL3_VERSION);
#endif
    } else if(strcmp(method, "tls")) {
        fprintf(stderr, "Error: unknown SSL method %s\n", method);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if((SSL_CTX_use_certificate_chain_file(ctx, cert_file) != 1) ||
       (SSL_CTX_use_PrivateKey_file(ctx, cert_file, SSL_FILETYPE_PEM) != 1)) {
        fprintf(stderr, "Error: can't load the certificate and key from %s\n", cert_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] -c CERT -a ACCOUNTS\n"
        "\n"
        "-c CERT      PEM file with the certificate and its passwordless private key\n"
        "             (not needed with -X none)\n"
        "-a ACCOUNTS  accounts file, one \"name password user_id\" per line\n"
        "-p PORT      port to listen on (default %d)\n"
        "-b IP        address to listen on (default all)\n"
        "-X METHOD    ssl3 (default, what the game uses), tls (any TLS version) or\n"
        "             none (plain TCP, for clients patched to skip SSL)\n"
        "-i SECONDS   interval between the server Pings (default %d)\n",
        prog, DEFAULT_PORT, DEFAULT_PING_INTERVAL);
    exit(1);
}

int main(int argc, char *argv[]) {
    server      srv;
    const char  *cert_file = NULL,
                *accounts_file = NULL,
                *method = "ssl3",
                *bind_ip = NULL;
    struct sockaddr_in addr;
    socklen_t   addrlen;
    connection  *c;
    pthread_t   tid;
    int         opt,
                port = DEFAULT_PORT,
                sd,
                csd,
                on = 1,
                ret;

    memset(&srv, 0, sizeof(srv));
    srv.ping_interval = DEFAULT_PING_INTERVAL;
    while((opt = getopt(argc, argv, "c:a:p:b:X:i:h")) != -1) {
        switch(opt) {
            case 'c': cert_file = optarg; break;
            case 'a': accounts_file = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'b': bind_ip = optarg; break;
            case 'X': method = optarg; break;
            case 'i': srv.ping_interval = atoi(optarg); break;
            default:  usage(argv[0]);
        }
    }
    if((!cert_file && strcmp(method, "none")) || !accounts_file || (optind != argc) ||
       (port <= 0) || (port > 65535) || (srv.ping_interval <= 0)) usage(argv[0]);

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    ret = fesl_accounts_load(&srv.accounts, accounts_file);
    if(ret) {
        if(ret < 0) fprintf(stderr, "Error: can't read %s\n", accounts_file);
        else        fprintf(stderr, "Error: %s line %d is invalid, expected \"name password user_id\"\n", accounts_file, ret);
        return 1;
    }

    if(strcmp(method, "none")) {
        srv.ctx = create_ssl_ctx(cert_file, method);
        if(!srv.ctx) return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind_ip && (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1)) {
        fprintf(stderr, "Error: invalid address %s\n", bind_ip);
        return 1;
    }
    sd = socket(AF_INET, SOCK_STREAM, 0);
    if(sd < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if((bind(sd, (struct sockaddr *)&addr, sizeof(addr)) < 0) || (listen(sd, SOMAXCONN) < 0)) {
        perror("bind");
        return 1;
    }
    printf("FESL server listening on %s:%d (%s), %u accounts\n",
        bind_ip ? bind_ip : "0.0.0.0", port, method, (unsigned)srv.accounts.count);

    for(;;) {
        addrlen = sizeof(addr);
        csd = accept(sd, (struct sockaddr *)&addr, &addrlen);
        if(csd < 0) {
            if(errno == EINTR) continue;
            perror("accept");
            continue;
        }
        c = calloc(1, sizeof(connection));
        if(!c) {
            close(csd);
            continue;
        }
        c->srv = &srv;
        c->sd  = csd;
        snprintf(c->peer, sizeof(c->peer), "%s:%u", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        if(pthread_create(&tid, NULL, client_thread, c)) {
            close(csd);
            free(c);
            continue;
        }
        pthread_detach(tid);
    }
    return 0;
}
