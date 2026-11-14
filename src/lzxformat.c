/*
** File:        lzxformat.c
** Description: The LZX (Amiga, Jonathan Forbes) backend of arcedit, header
**              editing only.
**
**  An LZX archive is a 10 byte info header ("LZX" + flags), then entries:
**  a 31 byte header, the name, the comment (up to 79 characters), then
**  the packed data. Small files are often "merged": they are packed
**  together, every member but the last has packed size 0 and the last
**  one's data holds the whole group.
**
**  Header layout (little endian sizes and CRCs, from unlzx.c 1.1):
**   0 protection (set = on: 1 r, 2 w, 4 d, 8 e, 16 a, 32 h, 64 s, 128 p)
**   2 unpacked size, 6 packed size, 10 machine type (10 = Amiga),
**   11 pack mode (0 store, 2 normal), 12 flags (1 = merged),
**   14 comment length, 15 version needed, 18 date (big endian bit field,
**   year from 1970), 22 data CRC-32, 26 header CRC-32 (over the header
**   with this field 0, the name and the comment), 30 name length.
**
**  arcedit renames, sets comments and protection (header only, data
**  untouched), deletes entries that aren't merged and whole merged groups,
**  and adds files stored. It doesn't pack (the LZX method is only known
**  from unpacking code) and doesn't create new LZX archives.
**
** Change history:
**  2026-11-14 orgin, created
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcformat.h"
#include "deflate.h"  /* crc32_update */
#include "lha.h"      /* lha_pathcmp */

#define LZX_OK         0
#define LZX_ERR_IO     1
#define LZX_ERR_FORMAT 2
#define LZX_ERR_MEM    3
#define LZX_ERR_MERGED 4

#define HDR 31

struct lzx_entry
{
 struct lzx_entry *next;
 uint8_t hdr[HDR];
 char *path;               /* name as stored, '/' separated */
 char *comment;
 uint32_t usize, psize;
 int merged;               /* member of a merged group */
 int group;                /* group number (entries packed together share it) */
 int deleted;              /* removed: only allowed for whole groups */

 long dataoff;             /* packed data in the source (psize bytes) */
 uint8_t *data;            /* added entries */
};

struct lzx_archive
{
 FILE *fh;
 uint8_t info[10];
 struct lzx_entry *first;
};

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static char *strndup2(const char *s, size_t n)
{
 char *r = malloc(n + 1);
 if(r)
 {
  memcpy(r, s, n);
  r[n] = 0;
 }
 return r;
}

static const char *lzx_error_text(int err)
{
 switch(err)
 {
  case LZX_OK:         return "no error";
  case LZX_ERR_IO:     return "read/write error";
  case LZX_ERR_FORMAT: return "not an LZX archive or damaged";
  case LZX_ERR_MEM:    return "out of memory";
  case LZX_ERR_MERGED: return "entries packed together (merged) can only be deleted all at once";
 }
 return "unknown error";
}

static void entry_free(struct lzx_entry *e)
{
 free(e->path);
 free(e->comment);
 free(e->data);
 free(e);
}

static void lzx_close(ARC *arc)
{
 struct lzx_archive *z = arc;
 if(z == NULL) return;
 while(z->first)
 {
  struct lzx_entry *e = z->first;
  z->first = e->next;
  entry_free(e);
 }
 if(z->fh) fclose(z->fh);
 free(z);
}

static int lzx_probe(const uint8_t *head, uint32_t len)
{
 return (len >= 10 && head[0] == 'L' && head[1] == 'Z' && head[2] == 'X') ? 2 : 0;
}

static int lzx_probe_name(const char *path)
{
 (void)path;
 return 0;   /* new LZX archives aren't created */
}

static uint32_t header_crc(struct lzx_entry *e)
{
 uint8_t h[HDR];
 uint32_t crc;
 memcpy(h, e->hdr, HDR);
 memset(h + 26, 0, 4);
 crc = crc32_update(0, h, HDR);
 crc = crc32_update(crc, (const uint8_t *)e->path, (uint32_t)strlen(e->path));
 if(e->comment) crc = crc32_update(crc, (const uint8_t *)e->comment, (uint32_t)strlen(e->comment));
 return crc;
}

static ARC *lzx_open(const char *path, int create, int *err)
{
 struct lzx_archive *z = calloc(1, sizeof(*z));
 struct lzx_entry **link, *groupstart = NULL;
 long off = 10;
 int group = 0;

 (void)create;
 *err = LZX_ERR_MEM;
 if(z == NULL) return NULL;
 z->fh = fopen(path, "rb");
 if(z->fh == NULL)
 {
  *err = LZX_ERR_IO;
  free(z);
  return NULL;
 }
 *err = LZX_ERR_FORMAT;
 if(fread(z->info, 1, 10, z->fh) != 10 || memcmp(z->info, "LZX", 3) != 0) goto fail;

 link = &z->first;
 for(;;)
 {
  struct lzx_entry *e;
  char name[256], comment[256];
  uint8_t h[HDR];
  size_t got;
  if(fseek(z->fh, off, SEEK_SET) != 0) break;
  got = fread(h, 1, HDR, z->fh);
  if(got == 0) break;
  if(got != HDR) goto fail;
  if(fread(name, 1, h[30], z->fh) != h[30] || fread(comment, 1, h[14], z->fh) != h[14]) goto fail;
  e = calloc(1, sizeof(*e));
  if(e == NULL)
  {
   *err = LZX_ERR_MEM;
   goto fail;
  }
  *link = e;
  link = &e->next;
  memcpy(e->hdr, h, HDR);
  e->path = strndup2(name, h[30]);
  e->comment = h[14] ? strndup2(comment, h[14]) : NULL;
  if(e->path == NULL || (h[14] && e->comment == NULL))
  {
   *err = LZX_ERR_MEM;
   goto fail;
  }
  if(header_crc(e) != rd32le(h + 26)) goto fail;
  e->usize = rd32le(h + 2);
  e->psize = rd32le(h + 6);
  e->merged = (h[12] & 1) != 0;
  e->dataoff = off + HDR + h[30] + h[14];
  /* group: merged entries up to and including the one carrying the
     group's data; an entry that isn't merged is a group of its own */
  if(groupstart == NULL || !e->merged)
  {
   groupstart = e;
   group++;
  }
  e->group = group;
  if(e->psize || !e->merged) groupstart = NULL;
  off = e->dataoff + (long)e->psize;
 }
 *err = LZX_OK;
 return z;

fail:
 lzx_close(z);
 return NULL;
}

/*
** The protection byte in LZX's layout from Amiga bits and back
*/
static uint8_t lzx_from_amiga(uint8_t prot)
{
 uint8_t p = 0;
 if(!(prot & ARC_PROT_R)) p |= 1;
 if(!(prot & ARC_PROT_W)) p |= 2;
 if(!(prot & ARC_PROT_D)) p |= 4;
 if(!(prot & ARC_PROT_E)) p |= 8;
 if(prot & ARC_PROT_A) p |= 16;
 if(prot & ARC_PROT_H) p |= 32;
 if(prot & ARC_PROT_S) p |= 64;
 if(prot & ARC_PROT_P) p |= 128;
 return p;
}

static uint8_t lzx_to_amiga(uint8_t p)
{
 uint8_t prot = 0;
 if(!(p & 1)) prot |= ARC_PROT_R;
 if(!(p & 2)) prot |= ARC_PROT_W;
 if(!(p & 4)) prot |= ARC_PROT_D;
 if(!(p & 8)) prot |= ARC_PROT_E;
 if(p & 16) prot |= ARC_PROT_A;
 if(p & 32) prot |= ARC_PROT_H;
 if(p & 64) prot |= ARC_PROT_S;
 if(p & 128) prot |= ARC_PROT_P;
 return prot;
}

static int lzx_write(ARC *arc, const char *path)
{
 struct lzx_archive *z = arc;
 struct lzx_entry *e;
 FILE *out;
 uint8_t *copy;
 int err = LZX_OK;

 /* merged groups must be kept or deleted as a whole */
 for(e = z->first; e; e = e->next)
 {
  struct lzx_entry *o;
  for(o = z->first; o; o = o->next)
  {
   if(o->group == e->group && o->deleted != e->deleted) return LZX_ERR_MERGED;
  }
 }
 out = fopen(path, "wb");
 copy = malloc(64 * 1024);
 if(out == NULL || copy == NULL)
 {
  if(out) fclose(out);
  free(copy);
  return out ? LZX_ERR_MEM : LZX_ERR_IO;
 }
 if(fwrite(z->info, 1, 10, out) != 10) err = LZX_ERR_IO;
 for(e = z->first; e && !err; e = e->next)
 {
  size_t nl = strlen(e->path), cl = e->comment ? strlen(e->comment) : 0;
  uint32_t left = e->psize;
  if(e->deleted) continue;
  if(nl > 255) nl = 255;
  if(cl > 79) cl = 79;
  e->hdr[30] = (uint8_t)nl;
  e->hdr[14] = (uint8_t)cl;
  wr32le(e->hdr + 26, header_crc(e));
  if(fwrite(e->hdr, 1, HDR, out) != HDR || fwrite(e->path, 1, nl, out) != nl ||
     (cl && fwrite(e->comment, 1, cl, out) != cl))
  {
   err = LZX_ERR_IO;
   break;
  }
  if(e->data)
  {
   if(left && fwrite(e->data, 1, left, out) != left) err = LZX_ERR_IO;
   continue;
  }
  if(left && fseek(z->fh, e->dataoff, SEEK_SET) != 0) err = LZX_ERR_IO;
  while(left && !err)
  {
   uint32_t n = left > 64 * 1024 ? 64 * 1024 : left;
   if(fread(copy, 1, n, z->fh) != n || fwrite(copy, 1, n, out) != n) err = LZX_ERR_IO;
   left -= n;
  }
 }
 if(fclose(out) != 0 && !err) err = LZX_ERR_IO;
 free(copy);
 return err;
}

static uint32_t lzx_caps(ARC *arc)
{
 (void)arc;
 return ARC_CAN_ALL;
}

static ENTRY *lzx_first(ARC *arc)
{
 struct lzx_entry *e = ((struct lzx_archive *)arc)->first;
 while(e && e->deleted) e = e->next;
 return e;
}

static ENTRY *lzx_next(ARC *arc, ENTRY *en)
{
 struct lzx_entry *e = ((struct lzx_entry *)en)->next;
 (void)arc;
 while(e && e->deleted) e = e->next;
 return e;
}

static const char *lzx_path(ENTRY *e)
{
 return ((struct lzx_entry *)e)->path;
}

static void lzx_set_path(ENTRY *en, const char *path)
{
 struct lzx_entry *e = en;
 char *p = strndup2(path, strlen(path) > 255 ? 255 : strlen(path));
 if(p)
 {
  free(e->path);
  e->path = p;
 }
}

static void lzx_set_comment(ENTRY *en, const char *comment)
{
 struct lzx_entry *e = en;
 free(e->comment);
 e->comment = (comment && comment[0]) ? strndup2(comment, strlen(comment) > 79 ? 79 : strlen(comment)) : NULL;
}

static uint8_t lzx_get_protection(ENTRY *e)
{
 return lzx_to_amiga(((struct lzx_entry *)e)->hdr[0]);
}

static void lzx_set_protection(ENTRY *e, uint8_t prot)
{
 ((struct lzx_entry *)e)->hdr[0] = lzx_from_amiga(prot);
}

/* Deleting only marks: the write checks merged groups */
static void lzx_remove(ARC *arc, ENTRY *e)
{
 (void)arc;
 ((struct lzx_entry *)e)->deleted = 1;
}

/*
** Packed date: bit 0 (MSB) .. day 5 bits, month 4 (0 = January),
** year 6 (from 1970), hour 5, minute 6, second 6
*/
static uint32_t lzx_date(uint32_t seconds)
{
 uint32_t dos = lha_unix_to_dos(seconds);
 uint32_t year = (dos >> 25) + 10, month = ((dos >> 21) & 15) - 1, day = (dos >> 16) & 31;
 uint32_t hour = (dos >> 11) & 31, minute = (dos >> 5) & 63, second = (dos & 31) * 2;
 if(year > 63) year = 63;
 return (day << 27) | (month << 23) | (year << 17) | (hour << 12) | (minute << 6) | second;
}

static int lzx_add(ARC *arc, const char *path, const uint8_t *buf, uint32_t len,
                   uint8_t prot, const char *comment, uint32_t seconds)
{
 struct lzx_archive *z = arc;
 struct lzx_entry *e = calloc(1, sizeof(*e)), **link;
 uint32_t date = lzx_date(seconds);
 int maxgroup = 0;

 if(e == NULL) return LZX_ERR_MEM;
 e->path = strndup2(path, strlen(path) > 255 ? 255 : strlen(path));
 e->comment = (comment && comment[0]) ? strndup2(comment, strlen(comment) > 79 ? 79 : strlen(comment)) : NULL;
 e->data = malloc(len ? len : 1);
 if(e->path == NULL || e->data == NULL)
 {
  entry_free(e);
  return LZX_ERR_MEM;
 }
 if(len) memcpy(e->data, buf, len);
 e->usize = e->psize = len;
 e->hdr[0] = lzx_from_amiga(prot);
 wr32le(e->hdr + 2, len);
 wr32le(e->hdr + 6, len);
 e->hdr[10] = 10;            /* Amiga */
 e->hdr[11] = 0;             /* stored */
 e->hdr[12] = 0;             /* not merged */
 e->hdr[15] = 10;            /* version needed */
 e->hdr[18] = (uint8_t)(date >> 24);
 e->hdr[19] = (uint8_t)(date >> 16);
 e->hdr[20] = (uint8_t)(date >> 8);
 e->hdr[21] = (uint8_t)date;
 wr32le(e->hdr + 22, len ? crc32_update(0, buf, len) : 0);

 /* replace an entry with the same name that isn't merged, else append */
 for(link = &z->first; *link; link = &(*link)->next)
 {
  if((*link)->group > maxgroup) maxgroup = (*link)->group;
  if(!(*link)->deleted && !(*link)->merged && lha_pathcmp((*link)->path, path) == 0)
  {
   struct lzx_entry *old = *link;
   e->group = old->group;
   e->next = old->next;
   *link = e;
   entry_free(old);
   return LZX_OK;
  }
 }
 e->group = maxgroup + 1;
 *link = e;
 return LZX_OK;
}

const struct arc_format LzxFormat =
{
 "LZX",
 lzx_probe,
 lzx_probe_name,
 lzx_open,
 lzx_close,
 lzx_write,
 lzx_error_text,
 lzx_caps,
 lzx_first,
 lzx_next,
 lzx_path,
 lzx_set_path,
 lzx_set_comment,
 lzx_get_protection,
 lzx_set_protection,
 lzx_remove,
 lzx_add,
 NULL,
 NULL,
 NULL
};
