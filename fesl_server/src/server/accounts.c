/*
    Accounts file, see accounts.h

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "accounts.h"

// copies the next space separated word, returns NULL if missing or too long
static const char *next_word(const char *p, const char *eol, char *out, size_t outsz) {
    size_t  n;

    while((p < eol) && isspace((unsigned char)*p)) p++;
    for(n = 0; (p + n < eol) && !isspace((unsigned char)p[n]); n++);
    if(!n || (n >= outsz)) return NULL;
    memcpy(out, p, n);
    out[n] = 0;
    return p + n;
}

static int has_more(const char *p, const char *eol) {
    while((p < eol) && isspace((unsigned char)*p)) p++;
    return p < eol;
}

int fesl_accounts_parse(fesl_accounts *a, const char *text) {
    const char      *p,
                    *line_end,
                    *end;
    char            id[16],
                    status[16],
                    *idend;
    fesl_account    acc,
                    *tmp;
    unsigned long   n;
    int             line;

    memset(a, 0, sizeof(fesl_accounts));
    for(p = text, line = 1; *p; line++) {
        line_end = strchr(p, '\n');
        if(!line_end) line_end = p + strlen(p);
        end = memchr(p, '#', line_end - p);
        if(!end) end = line_end;

        while((p < end) && isspace((unsigned char)*p)) p++;
        if(p < end) {
            memset(&acc, 0, sizeof(acc));
            p = next_word(p, end, acc.name, sizeof(acc.name));
            if(p) p = next_word(p, end, acc.password, sizeof(acc.password));
            if(p) p = next_word(p, end, id, sizeof(id));
            if(!p) goto invalid;
            if(has_more(p, end)) {
                p = next_word(p, end, status, sizeof(status));
                if(!p || has_more(p, end)) goto invalid;
                if(!strcasecmp(status, "active"))           acc.status = ACCOUNT_ACTIVE;
                else if(!strcasecmp(status, "disabled"))    acc.status = ACCOUNT_DISABLED;
                else if(!strcasecmp(status, "banned"))      acc.status = ACCOUNT_BANNED;
                else if(!strcasecmp(status, "pending"))     acc.status = ACCOUNT_PENDING;
                else goto invalid;
            }
            n = strtoul(id, &idend, 10);
            if(*idend || !n || (n > 0xffffffffUL) || fesl_accounts_find(a, acc.name)) goto invalid;
            acc.user_id = n;

            tmp = realloc(a->list, (a->count + 1) * sizeof(fesl_account));
            if(!tmp) {
                fesl_accounts_free(a);
                return -1;
            }
            a->list = tmp;
            a->list[a->count++] = acc;
        }
        p = *line_end ? line_end + 1 : line_end;
    }
    return 0;

invalid:
    fesl_accounts_free(a);
    return line;
}

int fesl_accounts_load(fesl_accounts *a, const char *path) {
    FILE    *fd;
    char    *text;
    long    size;
    int     ret;

    memset(a, 0, sizeof(fesl_accounts));
    fd = fopen(path, "rb");
    if(!fd) return -1;
    if(fseek(fd, 0, SEEK_END) || ((size = ftell(fd)) < 0) || fseek(fd, 0, SEEK_SET)) {
        fclose(fd);
        return -1;
    }
    text = malloc(size + 1);
    if(!text || (fread(text, 1, size, fd) != (size_t)size)) {
        free(text);
        fclose(fd);
        return -1;
    }
    fclose(fd);
    text[size] = 0;
    ret = fesl_accounts_parse(a, text);
    free(text);
    return ret;
}

const fesl_account *fesl_accounts_find(const fesl_accounts *a, const char *name) {
    size_t  i;

    for(i = 0; i < a->count; i++) {
        if(!strcasecmp(a->list[i].name, name)) return &a->list[i];
    }
    return NULL;
}

void fesl_accounts_free(fesl_accounts *a) {
    free(a->list);
    memset(a, 0, sizeof(fesl_accounts));
}
