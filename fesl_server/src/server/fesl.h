/*
    FESL (fesl.ea.com) packet decoder/encoder

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Packet layout (see stuff/info/fesl.ea.com.protocol.aluigi.mht):

        4 bytes   command, ASCII ("fsys", "acct", "subs", "dobj", ...)
        4 bytes   big endian, (type << 24) | packet number
        4 bytes   big endian, total packet size including this header
        ...       "key=value\n" lines terminated by a NUL byte

    Packets bigger than the fragment size are sent as multipart packets:
    each part has a multi type and the same command and number, and its
    data is "data=<piece of base64'ed original data>\nsize=<total base64
    size>\n". Use fesl_multipart_add() to put them back together.
*/

#ifndef FESL_H
#define FESL_H

#include <stddef.h>
#include <stdint.h>

#define FESL_HEADER_SIZE        12
#define FESL_MAX_PACKET_SIZE    0x10000     // sanity limit, real packets are a few KB at most

enum {
    FESL_TYPE_SINGLE_SERVER = 0x80,     // one packet answer/request
    FESL_TYPE_MULTI_SERVER  = 0xb0,     // part of a multipart answer/request
    FESL_TYPE_SINGLE_CLIENT = 0xc0,     // one packet request
    FESL_TYPE_MULTI_CLIENT  = 0xf0      // part of a multipart request
};

enum {
    FESL_OK         = 0,
    FESL_NEED_MORE  = 1,    // buffer doesn't contain a complete packet yet
    FESL_ERR_SIZE   = -1,   // invalid size field, the stream can't be resynchronized
    FESL_ERR_MEMORY = -2,
    FESL_ERR_FORMAT = -3
};

typedef struct {
    char        *key;
    char        *value;
} fesl_pair;

typedef struct {
    char        command[5];     // NUL terminated
    uint8_t     type;           // FESL_TYPE_*
    uint32_t    number;         // 24 bits, answers use the number of their request
    fesl_pair   *pairs;         // in packet order, point into data
    size_t      count;
    char        *data;          // owned storage for pairs
} fesl_packet;

typedef struct {
    char        command[5];
    uint8_t     type;
    uint32_t    number;
    size_t      total;          // base64 size announced by the "size" field
    char        *b64;
    size_t      len;
    size_t      cap;
    int         active;
} fesl_multipart;

// Decodes one packet from the start of buf. On FESL_OK fills pkt (free it
// with fesl_packet_free) and sets *consumed to the packet size; on
// FESL_NEED_MORE nothing is allocated and more data must be appended.
int fesl_decode(const uint8_t *buf, size_t len, fesl_packet *pkt, size_t *consumed);

// Builds pkt from a key/value data block (without header), used for
// multipart packets and for building answers.
int fesl_packet_init(fesl_packet *pkt, const char *command, uint8_t type, uint32_t number, const char *data, size_t len);

void fesl_packet_free(fesl_packet *pkt);

// Value of the first pair with this key, or NULL.
const char *fesl_get(const fesl_packet *pkt, const char *key);

#define fesl_txn(PKT)   fesl_get((PKT), "TXN")

// Size of the encoded packet, header included.
size_t fesl_encoded_size(const fesl_packet *pkt);

// Encodes pkt into out, returns the number of bytes written or FESL_ERR_SIZE
// if outsz is too small or the packet exceeds FESL_MAX_PACKET_SIZE.
int fesl_encode(const fesl_packet *pkt, uint8_t *out, size_t outsz);

// Decodes %XX escapes in place, returns the new length.
size_t fesl_url_decode(char *s);

void fesl_multipart_init(fesl_multipart *mp);
void fesl_multipart_free(fesl_multipart *mp);

// Adds a multipart piece. Returns FESL_NEED_MORE until the last piece
// arrives, then FESL_OK with the reassembled packet in out (its type is the
// matching single type). Pieces of another command/number are an error.
int fesl_multipart_add(fesl_multipart *mp, const fesl_packet *part, fesl_packet *out);

#endif
