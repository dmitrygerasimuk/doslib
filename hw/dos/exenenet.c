
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>

#include <hw/dos/exehdr.h>
#include <hw/dos/exenehdr.h>
#include <hw/dos/exenepar.h>

void exe_ne_header_name_entry_table_init(struct exe_ne_header_name_entry_table * const t) {
    memset(t,0,sizeof(*t));
}

void exe_ne_header_name_entry_table_free_table(struct exe_ne_header_name_entry_table * const t) {
    if (t->table) free(t->table);
    t->table = NULL;
    t->length = 0;
}

void exe_ne_header_name_entry_table_free_raw(struct exe_ne_header_name_entry_table * const t) {
    if (t->raw && t->raw_ownership) free(t->raw);
    t->raw_length = 0;
    t->raw = 0;
}

unsigned char *exe_ne_header_name_entry_table_alloc_raw(struct exe_ne_header_name_entry_table * const t,const size_t length) {
    if (t->raw != NULL) {
        if (length == t->raw_length)
            return t->raw;

        exe_ne_header_name_entry_table_free_raw(t);
    }

    assert(t->raw == NULL);
    if (length == 0)
        return NULL;

    t->raw = malloc(length);
    if (t->raw == NULL)
        return NULL;

    t->raw_length = length;
    t->raw_ownership = 1;
    return t->raw;
}

void exe_ne_header_name_entry_table_free(struct exe_ne_header_name_entry_table * const t) {
    exe_ne_header_name_entry_table_free_table(t);
    exe_ne_header_name_entry_table_free_raw(t);
}

uint16_t ne_name_entry_get_ordinal(const struct exe_ne_header_name_entry_table * const t,const struct exe_ne_header_name_entry * const ent) {
    assert(((size_t)ent->offset+(size_t)ent->length+(size_t)2) <= t->raw_length);
    return *((uint16_t*)(t->raw + ent->offset + ent->length));
}

/* WARNING: Points at name string, which is NOT terminated by NUL. You will need to copy the string to make it a C-string. */
unsigned char *ne_name_entry_get_name_base(const struct exe_ne_header_name_entry_table * const t,const struct exe_ne_header_name_entry * const ent) {
    assert(((size_t)ent->offset+(size_t)ent->length) <= t->raw_length);
    return t->raw + ent->offset;
}

void ne_name_entry_get_name(char *dst,size_t dstmax,const struct exe_ne_header_name_entry_table * const t,const struct exe_ne_header_name_entry * const ent) {
    size_t cpy;

    if (dstmax == 0) return;

    cpy = ent->length;
    if (cpy > (dstmax - 1)) cpy = dstmax - 1;

    dst[cpy] = 0;
    if (cpy != 0) {
        unsigned char *rstr = ne_name_entry_get_name_base(t,ent);
        memcpy(dst,rstr,cpy);
    }
}

