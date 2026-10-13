/*
    Minimal pcap reader for tests, see pcap_stream.h

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcap_stream.h"

#define LINKTYPE_ETHERNET   1
#define LINKTYPE_RAW        101
#define TCP_SYN             0x02
#define TCP_ACK             0x10

typedef struct {
    uint32_t    src_ip,
                dst_ip;
    uint16_t    src_port,
                dst_port;
    uint32_t    seq;
    uint8_t     flags;
    const uint8_t *payload;
    size_t      len;
} tcp_segment;

typedef struct {
    pcap_buffer *buf;
    uint32_t    next_seq;
    int         started;
} direction;

static uint32_t get32(const uint8_t *p, int swap) {
    return swap
        ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]
        : ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static uint32_t be32(const uint8_t *p) { return get32(p, 1); }
static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

static int parse_segment(const uint8_t *pkt, size_t len, uint32_t linktype, tcp_segment *seg) {
    size_t  ihl,
            iplen,
            doff;

    if(linktype == LINKTYPE_ETHERNET) {
        if((len < 14) || (be16(pkt + 12) != 0x0800)) return -1;
        pkt += 14;
        len -= 14;
    }
    if((len < 20) || ((pkt[0] >> 4) != 4) || (pkt[9] != 6)) return -1;    // IPv4 TCP only
    ihl   = (pkt[0] & 15) * 4;
    iplen = be16(pkt + 2);      // excludes the Ethernet padding of short frames
    if((ihl < 20) || (iplen < ihl) || (iplen > len)) return -1;
    if(be16(pkt + 6) & 0x3fff) return -1;   // fragmented

    seg->src_ip = be32(pkt + 12);
    seg->dst_ip = be32(pkt + 16);
    pkt   += ihl;
    iplen -= ihl;
    if(iplen < 20) return -1;
    doff = (pkt[12] >> 4) * 4;
    if((doff < 20) || (doff > iplen)) return -1;

    seg->src_port = be16(pkt);
    seg->dst_port = be16(pkt + 2);
    seg->seq      = be32(pkt + 4);
    seg->flags    = pkt[13];
    seg->payload  = pkt + doff;
    seg->len      = iplen - doff;
    return 0;
}

static int append(direction *d, const tcp_segment *seg) {
    uint32_t    skip;
    size_t      len;
    uint8_t     *tmp;

    if(seg->flags & TCP_SYN) {
        d->next_seq = seg->seq + 1;
        d->started  = 1;
        return 0;
    }
    if(!seg->len) return 0;
    if(!d->started) {   // capture started after the handshake
        d->next_seq = seg->seq;
        d->started  = 1;
    }

    skip = d->next_seq - seg->seq;  // > 0 for retransmitted data
    if((int32_t)skip < 0) {
        fprintf(stderr, "pcap: missing TCP data before seq %u\n", seg->seq);
        return -1;
    }
    if(skip >= seg->len) return 0;
    len = seg->len - skip;

    tmp = realloc(d->buf->data, d->buf->len + len);
    if(!tmp) return -1;
    d->buf->data = tmp;
    memcpy(d->buf->data + d->buf->len, seg->payload + skip, len);
    d->buf->len += len;
    d->next_seq += len;
    return 0;
}

int pcap_read_tcp_streams(const char *path, pcap_tcp_streams *s) {
    FILE        *fd;
    uint8_t     *file = NULL;
    long        size;
    size_t      off;
    uint32_t    magic,
                linktype,
                incl;
    int         swap,
                ret = -1,
                have_conn = 0;
    tcp_segment seg,
                conn;
    direction   up,
                down;

    memset(s, 0, sizeof(pcap_tcp_streams));
    memset(&up, 0, sizeof(up));
    memset(&down, 0, sizeof(down));
    up.buf   = &s->to_server;
    down.buf = &s->to_client;

    fd = fopen(path, "rb");
    if(!fd) {
        perror(path);
        return -1;
    }
    if(fseek(fd, 0, SEEK_END) || ((size = ftell(fd)) < 24) || fseek(fd, 0, SEEK_SET)) goto bad_file;
    file = malloc(size);
    if(!file || (fread(file, 1, size, fd) != (size_t)size)) goto bad_file;

    magic = get32(file, 0);
    if(magic == 0xa1b2c3d4)      swap = 0;
    else if(magic == 0xd4c3b2a1) swap = 1;
    else goto bad_file;
    linktype = get32(file + 20, swap);
    if((linktype != LINKTYPE_ETHERNET) && (linktype != LINKTYPE_RAW)) {
        fprintf(stderr, "%s: unsupported link type %u\n", path, linktype);
        goto quit;
    }

    for(off = 24; off + 16 <= (size_t)size; off += 16 + incl) {
        incl = get32(file + off + 8, swap);
        if(incl > (size_t)size - off - 16) goto bad_file;
        if(parse_segment(file + off + 16, incl, linktype, &seg) < 0) continue;

        if(!have_conn) {
            // the first SYN, or else the first data, goes to the server
            if(!(seg.flags & TCP_SYN) && !seg.len) continue;
            if((seg.flags & TCP_SYN) && (seg.flags & TCP_ACK)) {
                conn.src_ip   = seg.dst_ip;
                conn.src_port = seg.dst_port;
                conn.dst_ip   = seg.src_ip;
                conn.dst_port = seg.src_port;
            } else {
                conn = seg;
            }
            s->server_port = conn.dst_port;
            have_conn = 1;
        }

        if((seg.src_ip == conn.src_ip) && (seg.src_port == conn.src_port) &&
           (seg.dst_ip == conn.dst_ip) && (seg.dst_port == conn.dst_port)) {
            if(append(&up, &seg) < 0) goto quit;
        } else if((seg.src_ip == conn.dst_ip) && (seg.src_port == conn.dst_port) &&
                  (seg.dst_ip == conn.src_ip) && (seg.dst_port == conn.src_port)) {
            if(append(&down, &seg) < 0) goto quit;
        }
    }
    if(!have_conn) {
        fprintf(stderr, "%s: no TCP connection found\n", path);
        goto quit;
    }
    ret = 0;
    goto quit;

bad_file:
    fprintf(stderr, "%s: not a valid pcap file\n", path);
quit:
    if(fd) fclose(fd);
    free(file);
    if(ret < 0) pcap_free_tcp_streams(s);
    return ret;
}

void pcap_free_tcp_streams(pcap_tcp_streams *s) {
    free(s->to_server.data);
    free(s->to_client.data);
    memset(s, 0, sizeof(pcap_tcp_streams));
}
