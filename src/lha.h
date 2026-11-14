/*
** File:        lha.h
** Description: LHA archive model: read the member list, edit it, write the
**              archive back. Members that aren't changed are copied byte for
**              byte (header and data), only changed headers are rebuilt and
**              only added files are compressed.
**
** Change history:
**  2026-11-14 orgin, created
*/

#ifndef LHA_H
#define LHA_H

#include <stdio.h>
#include <stdint.h>

/* Amiga protection bits as stored in the header attribute byte (the low
   byte of fib_Protection: bits 0-3 d e w r are set when NOT allowed, bits
   4-7 a p s h are set when the flag is on) */
#define LHA_PROT_D 0x01
#define LHA_PROT_E 0x02
#define LHA_PROT_W 0x04
#define LHA_PROT_R 0x08
#define LHA_PROT_A 0x10
#define LHA_PROT_P 0x20
#define LHA_PROT_S 0x40
#define LHA_PROT_H 0x80

/* An extended header kept as it is (types this tool doesn't rebuild) */
struct lha_ext
{
 struct lha_ext *next;
 uint8_t type;
 uint32_t len;          /* data bytes, without type and next-size fields */
 uint8_t *data;
};

struct lha_member
{
 struct lha_member *next;

 int level;             /* header level 0, 1 or 2 */
 char method[6];        /* "-lh5-" etc, NUL terminated */
 uint32_t compsize;     /* bytes of compressed data */
 uint32_t origsize;
 uint32_t stamp;        /* level 0/1: MS-DOS date/time, level 2: seconds since 1970 (local time) */
 uint8_t attr;          /* Amiga protection, see LHA_PROT_* */
 uint8_t os;            /* 'A' for Amiga, 0 when the header has none (level 0) */
 uint16_t crc;          /* CRC-16 of the original data */
 char *path;            /* full path, '/' separated; drawer entries end with '/' */
 char *comment;         /* file note or NULL */
 struct lha_ext *extras;

 /* the original header, written again as it is unless modified is set */
 uint8_t *rawhdr;
 uint32_t rawhdrlen;
 int modified;

 /* compressed data: in the source archive at dataoff, or in memory */
 long dataoff;
 uint8_t *data;
};

struct lha_archive
{
 FILE *fh;              /* the source archive, NULL for a new one */
 uint8_t *prefix;       /* bytes before the first header (self-extracting archives) */
 uint32_t prefixlen;
 struct lha_member *first;
 int deflevel;          /* header level for added members */
};

/* Results */
#define LHA_OK        0
#define LHA_ERR_IO    1
#define LHA_ERR_FORMAT 2
#define LHA_ERR_MEM   3

const char *lha_error_text(int err);

uint16_t lha_crc16(uint16_t crc, const uint8_t *buf, uint32_t len);

/* Open and read an archive. A missing file gives an empty archive when
   create is set. *err gets an LHA_ERR_* code on failure. */
struct lha_archive *lha_open(const char *name, int create, int *err);
void lha_close(struct lha_archive *arc);

/* Write the archive to name (which must not be the source file). */
int lha_write(struct lha_archive *arc, const char *name);

/* Member helpers */
void lha_member_set_path(struct lha_member *m, const char *path);
void lha_member_set_comment(struct lha_member *m, const char *comment);
void lha_member_remove(struct lha_archive *arc, struct lha_member *m);
void lha_member_free(struct lha_member *m);
int lha_member_is_dir(const struct lha_member *m);

/* Add a member from data in memory: compressed with -lh5- when that is
   smaller, else stored. stamp is a DateStamp-like local time given as
   seconds since 1970. Replaces a member with the same path. */
int lha_add(struct lha_archive *arc, const char *path, const uint8_t *buf, uint32_t len,
            uint8_t attr, const char *comment, uint32_t seconds);

/* Time conversion between the two header formats (local time) */
uint32_t lha_unix_to_dos(uint32_t t);
uint32_t lha_dos_to_unix(uint32_t d);

/* -lh5- compressor (lh5enc.c): returns the compressed size, or 0 when the
   result would not be smaller than len. out must hold len bytes. */
uint32_t lh5_compress(const uint8_t *in, uint32_t len, uint8_t *out);

/* Case-insensitive compare of archive paths (Latin-1) */
int lha_pathcmp(const char *a, const char *b);
int lha_pathncmp(const char *a, const char *b, size_t n);

#endif
