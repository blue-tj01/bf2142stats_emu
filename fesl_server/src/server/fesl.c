/*
    FESL (fesl.ea.com) packet decoder/encoder

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "fesl.h"

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}

// splits data in place into "key=value" lines, data must be NUL terminated
static int parse_pairs(fesl_packet *pkt) {
    char    *p,
            *eol,
            *eq;
    size_t  max = 1;

    for(p = pkt->data; *p; p++) {
        if(*p == '\n') max++;
    }
    pkt->pairs = calloc(max, sizeof(fesl_pair));
    if(!pkt->pairs) return FESL_ERR_MEMORY;

    for(p = pkt->data; *p; p = eol) {
        eol = strchr(p, '\n');
        if(eol) *eol++ = 0;
        else    eol = p + strlen(p);
        if(!*p) continue;   // empty line

        eq = strchr(p, '=');
        if(eq) *eq++ = 0;
        else   eq = p + strlen(p);  // key without value, treated as empty
        pkt->pairs[pkt->count].key   = p;
        pkt->pairs[pkt->count].value = eq;
        pkt->count++;
    }
    return FESL_OK;
}

int fesl_packet_init(fesl_packet *pkt, const char *command, uint8_t type, uint32_t number, const char *data, size_t len) {
    int     ret;

    memset(pkt, 0, sizeof(fesl_packet));
    memcpy(pkt->command, command, 4);
    pkt->type   = type;
    pkt->number = number & 0xffffff;

    pkt->data = malloc(len + 1);
    if(!pkt->data) return FESL_ERR_MEMORY;
    memcpy(pkt->data, data, len);
    pkt->data[len] = 0;     // data stops at the first NUL anyway

    ret = parse_pairs(pkt);
    if(ret != FESL_OK) fesl_packet_free(pkt);
    return ret;
}

int fesl_decode(const uint8_t *buf, size_t len, fesl_packet *pkt, size_t *consumed) {
    uint32_t    word,
                size;
    int         ret;

    if(len < FESL_HEADER_SIZE) return FESL_NEED_MORE;
    size = get_be32(buf + 8);
    if((size < FESL_HEADER_SIZE) || (size > FESL_MAX_PACKET_SIZE)) return FESL_ERR_SIZE;
    if(len < size) return FESL_NEED_MORE;

    word = get_be32(buf + 4);
    ret = fesl_packet_init(pkt, (const char *)buf, word >> 24, word, (const char *)buf + FESL_HEADER_SIZE, size - FESL_HEADER_SIZE);
    if(ret != FESL_OK) return ret;
    if(consumed) *consumed = size;
    return FESL_OK;
}

void fesl_packet_free(fesl_packet *pkt) {
    free(pkt->pairs);
    free(pkt->data);
    memset(pkt, 0, sizeof(fesl_packet));
}

const char *fesl_get(const fesl_packet *pkt, const char *key) {
    size_t  i;

    for(i = 0; i < pkt->count; i++) {
        if(!strcmp(pkt->pairs[i].key, key)) return pkt->pairs[i].value;
    }
    return NULL;
}

size_t fesl_encoded_size(const fesl_packet *pkt) {
    size_t  i,
            size = FESL_HEADER_SIZE + 1;    // final NUL

    for(i = 0; i < pkt->count; i++) {
        size += strlen(pkt->pairs[i].key) + 1 + strlen(pkt->pairs[i].value) + 1;
    }
    return size;
}

int fesl_encode(const fesl_packet *pkt, uint8_t *out, size_t outsz) {
    size_t  i,
            n,
            size = fesl_encoded_size(pkt);
    uint8_t *p;

    if((size > outsz) || (size > FESL_MAX_PACKET_SIZE)) return FESL_ERR_SIZE;

    memcpy(out, pkt->command, 4);
    put_be32(out + 4, ((uint32_t)pkt->type << 24) | (pkt->number & 0xffffff));
    put_be32(out + 8, size);
    p = out + FESL_HEADER_SIZE;
    for(i = 0; i < pkt->count; i++) {
        n = strlen(pkt->pairs[i].key);
        memcpy(p, pkt->pairs[i].key, n);
        p += n;
        *p++ = '=';
        n = strlen(pkt->pairs[i].value);
        memcpy(p, pkt->pairs[i].value, n);
        p += n;
        *p++ = '\n';
    }
    *p++ = 0;
    return (int)(p - out);
}

static int hex_value(int c) {
    if((c >= '0') && (c <= '9')) return c - '0';
    c = tolower(c);
    if((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
    return -1;
}

size_t fesl_url_decode(char *s) {
    char    *in,
            *out;
    int     hi,
            lo;

    for(in = out = s; *in; in++) {
        if((in[0] == '%') && ((hi = hex_value(in[1])) >= 0) && ((lo = hex_value(in[2])) >= 0)) {
            *out++ = (hi << 4) | lo;
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = 0;
    return out - s;
}

static int b64_value(int c) {
    if((c >= 'A') && (c <= 'Z')) return c - 'A';
    if((c >= 'a') && (c <= 'z')) return c - 'a' + 26;
    if((c >= '0') && (c <= '9')) return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}

// decodes in place, characters outside the base64 alphabet are skipped
static size_t b64_decode(char *s, size_t len) {
    uint32_t    bits = 0;
    size_t      i,
                n = 0,
                out = 0;
    int         v;

    for(i = 0; i < len; i++) {
        if(s[i] == '=') break;
        v = b64_value((uint8_t)s[i]);
        if(v < 0) continue;
        bits = (bits << 6) | v;
        if(++n == 4) {
            s[out++] = bits >> 16;
            s[out++] = bits >> 8;
            s[out++] = bits;
            n = bits = 0;
        }
    }
    if(n == 2) {
        s[out++] = bits >> 4;
    } else if(n == 3) {
        s[out++] = bits >> 10;
        s[out++] = bits >> 2;
    }
    return out;
}

void fesl_multipart_init(fesl_multipart *mp) {
    memset(mp, 0, sizeof(fesl_multipart));
}

void fesl_multipart_free(fesl_multipart *mp) {
    free(mp->b64);
    fesl_multipart_init(mp);
}

int fesl_multipart_add(fesl_multipart *mp, const fesl_packet *part, fesl_packet *out) {
    const char  *data,
                *size;
    char        *piece,
                *tmp;
    size_t      len;
    int         ret;

    if((part->type != FESL_TYPE_MULTI_SERVER) && (part->type != FESL_TYPE_MULTI_CLIENT)) return FESL_ERR_FORMAT;
    data = fesl_get(part, "data");
    if(!data) return FESL_ERR_FORMAT;

    if(!mp->active) {
        size = fesl_get(part, "size");
        if(!size) return FESL_ERR_FORMAT;
        mp->total = strtoul(size, NULL, 10);
        if(!mp->total || (mp->total > (FESL_MAX_PACKET_SIZE * 2))) return FESL_ERR_SIZE;
        memcpy(mp->command, part->command, sizeof(mp->command));
        mp->type   = part->type;
        mp->number = part->number;
        mp->len    = 0;
        mp->active = 1;
    } else if(memcmp(mp->command, part->command, 4) || (mp->type != part->type) || (mp->number != part->number)) {
        return FESL_ERR_FORMAT;
    }

    // EA escapes some characters ("%3d" for '=') inside the base64 pieces
    piece = strdup(data);
    if(!piece) return FESL_ERR_MEMORY;
    len = fesl_url_decode(piece);
    if((mp->len + len) > mp->total) {
        free(piece);
        fesl_multipart_free(mp);
        return FESL_ERR_SIZE;
    }
    if((mp->len + len) > mp->cap) {
        tmp = realloc(mp->b64, mp->total);
        if(!tmp) {
            free(piece);
            return FESL_ERR_MEMORY;
        }
        mp->b64 = tmp;
        mp->cap = mp->total;
    }
    memcpy(mp->b64 + mp->len, piece, len);
    mp->len += len;
    free(piece);

    if(mp->len < mp->total) return FESL_NEED_MORE;

    len = b64_decode(mp->b64, mp->len);
    ret = fesl_packet_init(out, mp->command,
        (mp->type == FESL_TYPE_MULTI_SERVER) ? FESL_TYPE_SINGLE_SERVER : FESL_TYPE_SINGLE_CLIENT,
        mp->number, mp->b64, len);
    fesl_multipart_free(mp);
    return ret;
}
