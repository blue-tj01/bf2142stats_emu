/*
    Minimal pcap reader for tests: rebuilds the two directions of the first
    TCP connection found in a capture (Ethernet or raw IPv4, no IP fragments).

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef PCAP_STREAM_H
#define PCAP_STREAM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t     *data;
    size_t      len;
} pcap_buffer;

typedef struct {
    uint16_t    server_port;
    pcap_buffer to_server;      // client -> server bytes
    pcap_buffer to_client;      // server -> client bytes
} pcap_tcp_streams;

// Returns 0 on success, otherwise prints the reason and returns -1.
int pcap_read_tcp_streams(const char *path, pcap_tcp_streams *s);

void pcap_free_tcp_streams(pcap_tcp_streams *s);

#endif
