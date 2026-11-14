/*
** File:        zipformat.c
** Description: The ZIP backend of arcedit.
**
**  A ZIP archive is a list of entries (local header + data, optionally a
**  data descriptor), then the central directory (one record per entry)
**  and the end of central directory record. Each entry is packed on its
**  own, so entries are copied as they are; a rename rebuilds the local
**  header, comments and protection bits live in the central directory.
**
**  Amiga attributes follow Info-ZIP: "version made by" host 1 (Amiga),
**  the protection bits in the upper 16 bits of the external attributes
**  with r w e d inverted (set = allowed, as unzip reads them as rwx),
**  the file note as the entry's file comment. Entries from other hosts
**  are taken as ----rwed (Unix: from the mode bits); setting their
**  protection makes them Amiga entries.
**
**  Not handled: ZIP64 (archives or entries over 4 GB, more than 65535
**  entries) and archives split over several disks.
**
** Change history:
**  2026-11-14 orgin, created
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcformat.h"
#include "deflate.h"
#include "lha.h"      /* lha_unix_to_dos, lha_pathcmp */

#define ZIP_OK         0
#define ZIP_ERR_IO     1
#define ZIP_ERR_FORMAT 2
#define ZIP_ERR_MEM    3
#define ZIP_ERR_ZIP64  4

#define SIG_LOCAL   0x04034b50
#define SIG_CENTRAL 0x02014b50
#define SIG_END     0x06054b50
#define SIG_DESC    0x08074b50

#define HOST_AMIGA 1
#define HOST_UNIX  3

struct zip_entry
{
 struct zip_entry *next;

 /* central directory fields */
 uint16_t madeby, needed, flags, method, time, date;
 uint32_t crc, csize, usize;
 uint16_t internal;
 uint32_t external;
 char *name;
 uint8_t *cextra;           /* central extra field */
 uint16_t cextralen;
 char *comment;

 /* the entry in the source archive: local header at lho, data from
    dataoff, rawlen bytes in all (local header + data + descriptor) */
 long lho, dataoff;
 uint32_t rawlen;
 uint8_t *lextra;           /* local extra field */
 uint16_t lextralen;
 uint32_t desclen;          /* data descriptor bytes after the data */
 int renamed;               /* local header must be rebuilt */

 uint8_t *data;             /* added entries: the packed data */
};

struct zip_archive
{
 FILE *fh;
 uint8_t *prefix;           /* bytes before the first entry (self-extracting) */
 uint32_t prefixlen;
 struct zip_entry *first;
 char *comment;             /* archive comment */
 uint16_t commentlen;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

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

static const char *zip_error_text(int err)
{
 switch(err)
 {
  case ZIP_OK:         return "no error";
  case ZIP_ERR_IO:     return "read/write error";
  case ZIP_ERR_FORMAT: return "not a ZIP archive or damaged";
  case ZIP_ERR_MEM:    return "out of memory";
  case ZIP_ERR_ZIP64:  return "ZIP64 archives are not supported";
 }
 return "unknown error";
}

static void entry_free(struct zip_entry *e)
{
 free(e->name);
 free(e->cextra);
 free(e->lextra);
 free(e->comment);
 free(e->data);
 free(e);
}

static void zip_close(ARC *arc)
{
 struct zip_archive *z = arc;
 if(z == NULL) return;
 while(z->first)
 {
  struct zip_entry *e = z->first;
  z->first = e->next;
  entry_free(e);
 }
 if(z->fh) fclose(z->fh);
 free(z->prefix);
 free(z->comment);
 free(z);
}

static int zip_probe(const uint8_t *head, uint32_t len)
{
 uint32_t i;
 /* a local header at the start, or (self-extracting / empty) an end record */
 if(len >= 4 && rd32(head) == SIG_LOCAL) return 2;
 if(len >= 22 && rd32(head) == SIG_END) return 2;
 for(i = 0; i + 30 <= len; i++)
 {
  if(rd32(head + i) == SIG_LOCAL && (rd16(head + i + 4) & 0xFF) <= 63 && rd16(head + i + 8) <= 99) return 1;
 }
 return 0;
}

static int zip_probe_name(const char *path)
{
 size_t n = strlen(path);
 return n > 4 && lha_pathcmp(path + n - 4, ".zip") == 0;
}

/*
** Read the archive: end record, central directory, local headers
*/
static ARC *zip_open(const char *path, int create, int *err)
{
 struct zip_archive *z = calloc(1, sizeof(*z));
 struct zip_entry **link;
 uint8_t *tail = NULL, *cd = NULL, buf[46];
 long size, tailoff, endoff = -1;
 uint32_t tl, i, cdsize, cdoff, count, pos;

 *err = ZIP_ERR_MEM;
 if(z == NULL) return NULL;
 z->fh = fopen(path, "rb");
 if(z->fh == NULL)
 {
  if(create)
  {
   *err = ZIP_OK;
   return z;
  }
  *err = ZIP_ERR_IO;
  free(z);
  return NULL;
 }
 *err = ZIP_ERR_FORMAT;
 if(fseek(z->fh, 0, SEEK_END) != 0 || (size = ftell(z->fh)) < 22) goto fail;

 /* the end record is in the last 22 + 65535 bytes */
 tailoff = size > 22 + 65535 ? size - (22 + 65535) : 0;
 tl = (uint32_t)(size - tailoff);
 tail = malloc(tl);
 if(tail == NULL)
 {
  *err = ZIP_ERR_MEM;
  goto fail;
 }
 if(fseek(z->fh, tailoff, SEEK_SET) != 0 || fread(tail, 1, tl, z->fh) != tl)
 {
  *err = ZIP_ERR_IO;
  goto fail;
 }
 for(i = tl - 22 + 1; i-- > 0;)
 {
  if(rd32(tail + i) == SIG_END && i + 22 + rd16(tail + i + 20) <= tl)
  {
   endoff = tailoff + (long)i;
   break;
  }
 }
 if(endoff < 0) goto fail;
 {
  uint8_t *e = tail + (endoff - tailoff);
  if(rd16(e + 4) != 0 || rd16(e + 6) != 0) goto fail;   /* split archives */
  count = rd16(e + 10);
  cdsize = rd32(e + 12);
  cdoff = rd32(e + 16);
  if(count == 0xFFFF || cdsize == 0xFFFFFFFF || cdoff == 0xFFFFFFFF)
  {
   *err = ZIP_ERR_ZIP64;
   goto fail;
  }
  z->commentlen = rd16(e + 20);
  if(z->commentlen)
  {
   z->comment = strndup2((const char *)e + 22, z->commentlen);
   if(z->comment == NULL)
   {
    *err = ZIP_ERR_MEM;
    goto fail;
   }
  }
 }
 /* self-extracting archives: offsets are often relative to the archive
    start, which is where the central directory really is minus cdoff */
 if((long)cdoff + (long)cdsize > endoff) goto fail;
 z->prefixlen = (uint32_t)(endoff - (long)cdsize - (long)cdoff);

 cd = malloc(cdsize ? cdsize : 1);
 if(cd == NULL)
 {
  *err = ZIP_ERR_MEM;
  goto fail;
 }
 if(fseek(z->fh, (long)z->prefixlen + (long)cdoff, SEEK_SET) != 0 || fread(cd, 1, cdsize, z->fh) != cdsize)
 {
  *err = ZIP_ERR_IO;
  goto fail;
 }

 link = &z->first;
 for(pos = 0, i = 0; i < count; i++)
 {
  struct zip_entry *e;
  uint16_t nl, xl, cl;
  if(pos + 46 > cdsize || rd32(cd + pos) != SIG_CENTRAL) goto fail;
  e = calloc(1, sizeof(*e));
  if(e == NULL)
  {
   *err = ZIP_ERR_MEM;
   goto fail;
  }
  *link = e;
  link = &e->next;
  e->madeby = rd16(cd + pos + 4);
  e->needed = rd16(cd + pos + 6);
  e->flags = rd16(cd + pos + 8);
  e->method = rd16(cd + pos + 10);
  e->time = rd16(cd + pos + 12);
  e->date = rd16(cd + pos + 14);
  e->crc = rd32(cd + pos + 16);
  e->csize = rd32(cd + pos + 20);
  e->usize = rd32(cd + pos + 24);
  nl = rd16(cd + pos + 28);
  xl = rd16(cd + pos + 30);
  cl = rd16(cd + pos + 32);
  e->internal = rd16(cd + pos + 36);
  e->external = rd32(cd + pos + 38);
  e->lho = (long)rd32(cd + pos + 42) + (long)z->prefixlen;
  if(e->csize == 0xFFFFFFFF || e->usize == 0xFFFFFFFF || rd32(cd + pos + 42) == 0xFFFFFFFF)
  {
   *err = ZIP_ERR_ZIP64;
   goto fail;
  }
  if(pos + 46 + nl + xl + cl > cdsize) goto fail;
  e->name = strndup2((const char *)cd + pos + 46, nl);
  e->cextralen = xl;
  e->cextra = malloc(xl ? xl : 1);
  e->comment = cl ? strndup2((const char *)cd + pos + 46 + nl + xl, cl) : NULL;
  if(e->name == NULL || e->cextra == NULL || (cl && e->comment == NULL))
  {
   *err = ZIP_ERR_MEM;
   goto fail;
  }
  memcpy(e->cextra, cd + pos + 46 + nl, xl);
  pos += 46u + nl + xl + cl;

  /* the local header: where the data starts, its extra field */
  if(fseek(z->fh, e->lho, SEEK_SET) != 0 || fread(buf, 1, 30, z->fh) != 30 || rd32(buf) != SIG_LOCAL) goto fail;
  e->lextralen = rd16(buf + 28);
  e->lextra = malloc(e->lextralen ? e->lextralen : 1);
  if(e->lextra == NULL)
  {
   *err = ZIP_ERR_MEM;
   goto fail;
  }
  if(fseek(z->fh, e->lho + 30 + rd16(buf + 26), SEEK_SET) != 0 ||
     fread(e->lextra, 1, e->lextralen, z->fh) != e->lextralen) goto fail;
  e->dataoff = e->lho + 30 + rd16(buf + 26) + e->lextralen;
  e->desclen = 0;
  if(e->flags & 0x08)
  {
   uint8_t d[4];
   if(fseek(z->fh, e->dataoff + (long)e->csize, SEEK_SET) != 0 || fread(d, 1, 4, z->fh) != 4) goto fail;
   e->desclen = rd32(d) == SIG_DESC ? 16 : 12;
  }
  e->rawlen = (uint32_t)(e->dataoff - e->lho) + e->csize + e->desclen;
 }
 free(tail);
 free(cd);
 if(z->prefixlen)
 {
  z->prefix = malloc(z->prefixlen);
  if(z->prefix == NULL || fseek(z->fh, 0, SEEK_SET) != 0 || fread(z->prefix, 1, z->prefixlen, z->fh) != z->prefixlen)
  {
   *err = ZIP_ERR_IO;
   zip_close(z);
   return NULL;
  }
 }
 *err = ZIP_OK;
 return z;

fail:
 free(tail);
 free(cd);
 zip_close(z);
 return NULL;
}

static int put_bytes(FILE *out, const void *buf, uint32_t len, uint32_t *pos)
{
 if(len && fwrite(buf, 1, len, out) != len) return ZIP_ERR_IO;
 *pos += len;
 return ZIP_OK;
}

/*
** Write the archive: entries, central directory, end record
*/
static int zip_write(ARC *arc, const char *path)
{
 struct zip_archive *z = arc;
 struct zip_entry *e;
 FILE *out = fopen(path, "wb");
 uint8_t h[46], *copy = malloc(64 * 1024);
 uint32_t pos = 0, cdstart, count = 0;
 uint32_t *offsets = NULL;
 int err = ZIP_OK, i;

 if(out == NULL || copy == NULL)
 {
  err = out ? ZIP_ERR_MEM : ZIP_ERR_IO;
  goto done;
 }
 for(e = z->first; e; e = e->next) count++;
 if(count > 65534)
 {
  err = ZIP_ERR_ZIP64;
  goto done;
 }
 offsets = malloc((count + 1) * sizeof(uint32_t));
 if(offsets == NULL)
 {
  err = ZIP_ERR_MEM;
  goto done;
 }
 if(z->prefixlen) err = put_bytes(out, z->prefix, z->prefixlen, &pos);

 /* entries; offsets are written relative to the archive start (after the prefix) */
 for(i = 0, e = z->first; e && err == ZIP_OK; e = e->next, i++)
 {
  offsets[i] = pos - z->prefixlen;
  if(e->data || e->renamed)
  {
   uint16_t nl = (uint16_t)strlen(e->name);
   wr32(h, SIG_LOCAL);
   wr16(h + 4, e->needed);
   wr16(h + 6, e->data ? e->flags : e->flags);
   wr16(h + 8, e->method);
   wr16(h + 10, e->time);
   wr16(h + 12, e->date);
   /* with a data descriptor the local header may hold zeros; keep the real values only without one */
   wr32(h + 14, (e->flags & 0x08) ? 0 : e->crc);
   wr32(h + 18, (e->flags & 0x08) ? 0 : e->csize);
   wr32(h + 22, (e->flags & 0x08) ? 0 : e->usize);
   wr16(h + 26, nl);
   wr16(h + 28, e->lextralen);
   err = put_bytes(out, h, 30, &pos);
   if(!err) err = put_bytes(out, e->name, nl, &pos);
   if(!err) err = put_bytes(out, e->lextra, e->lextralen, &pos);
   if(err) break;
   if(e->data)
   {
    err = put_bytes(out, e->data, e->csize, &pos);
   }
   else
   {
    /* data and descriptor from the source */
    uint32_t left = e->csize + e->desclen;
    if(fseek(z->fh, e->dataoff, SEEK_SET) != 0) err = ZIP_ERR_IO;
    while(left && !err)
    {
     uint32_t n = left > 64 * 1024 ? 64 * 1024 : left;
     if(fread(copy, 1, n, z->fh) != n) err = ZIP_ERR_IO;
     else err = put_bytes(out, copy, n, &pos);
     left -= n;
    }
   }
  }
  else
  {
   /* unchanged: local header, data and descriptor as they are */
   uint32_t left = e->rawlen;
   if(fseek(z->fh, e->lho, SEEK_SET) != 0) err = ZIP_ERR_IO;
   while(left && !err)
   {
    uint32_t n = left > 64 * 1024 ? 64 * 1024 : left;
    if(fread(copy, 1, n, z->fh) != n) err = ZIP_ERR_IO;
    else err = put_bytes(out, copy, n, &pos);
    left -= n;
   }
  }
 }

 /* central directory */
 cdstart = pos;
 for(i = 0, e = z->first; e && err == ZIP_OK; e = e->next, i++)
 {
  uint16_t nl = (uint16_t)strlen(e->name), cl = (uint16_t)(e->comment ? strlen(e->comment) : 0);
  wr32(h, SIG_CENTRAL);
  wr16(h + 4, e->madeby);
  wr16(h + 6, e->needed);
  wr16(h + 8, e->flags);
  wr16(h + 10, e->method);
  wr16(h + 12, e->time);
  wr16(h + 14, e->date);
  wr32(h + 16, e->crc);
  wr32(h + 20, e->csize);
  wr32(h + 24, e->usize);
  wr16(h + 28, nl);
  wr16(h + 30, e->cextralen);
  wr16(h + 32, cl);
  wr16(h + 34, 0);
  wr16(h + 36, e->internal);
  wr32(h + 38, e->external);
  wr32(h + 42, offsets[i]);
  err = put_bytes(out, h, 46, &pos);
  if(!err) err = put_bytes(out, e->name, nl, &pos);
  if(!err) err = put_bytes(out, e->cextra, e->cextralen, &pos);
  if(!err) err = put_bytes(out, e->comment, cl, &pos);
 }

 /* end record */
 if(err == ZIP_OK)
 {
  uint8_t end[22];
  wr32(end, SIG_END);
  wr16(end + 4, 0);
  wr16(end + 6, 0);
  wr16(end + 8, count);
  wr16(end + 10, count);
  wr32(end + 12, pos - cdstart);
  wr32(end + 16, cdstart - z->prefixlen);
  wr16(end + 20, z->commentlen);
  err = put_bytes(out, end, 22, &pos);
  if(!err) err = put_bytes(out, z->comment, z->commentlen, &pos);
 }

done:
 if(out && fclose(out) != 0 && err == ZIP_OK) err = ZIP_ERR_IO;
 free(copy);
 free(offsets);
 return err;
}

static uint32_t zip_caps(ARC *arc)
{
 (void)arc;
 return ARC_CAN_ALL;
}

static ENTRY *zip_first(ARC *arc)
{
 return ((struct zip_archive *)arc)->first;
}

static ENTRY *zip_next(ARC *arc, ENTRY *e)
{
 (void)arc;
 return ((struct zip_entry *)e)->next;
}

static const char *zip_path(ENTRY *e)
{
 return ((struct zip_entry *)e)->name;
}

/*
** Remove extra field blocks of a type (id) from an extra field
*/
static void strip_extra(uint8_t *extra, uint16_t *len, uint16_t id)
{
 uint32_t pos = 0;
 while(pos + 4 <= *len)
 {
  uint32_t blen = 4u + rd16(extra + pos + 2);
  if(pos + blen > *len) break;
  if(rd16(extra + pos) == id)
  {
   memmove(extra + pos, extra + pos + blen, *len - pos - blen);
   *len = (uint16_t)(*len - blen);
  }
  else
  {
   pos += blen;
  }
 }
}

static void zip_set_path(ENTRY *e, const char *path)
{
 struct zip_entry *z = e;
 size_t n = strlen(path), i, extra = 0;
 char *p;
 /* names marked UTF-8 (flag bit 11) get the Latin-1 name converted */
 if(z->flags & 0x0800)
 {
  for(i = 0; i < n; i++) if((uint8_t)path[i] >= 0x80) extra++;
 }
 p = malloc(n + extra + 1);
 if(p == NULL) return;
 if(extra)
 {
  char *o = p;
  for(i = 0; i < n; i++)
  {
   uint8_t c = (uint8_t)path[i];
   if(c >= 0x80)
   {
    *o++ = (char)(0xC0 | (c >> 6));
    *o++ = (char)(0x80 | (c & 0x3F));
   }
   else *o++ = (char)c;
  }
  *o = 0;
 }
 else
 {
  memcpy(p, path, n + 1);
 }
 free(z->name);
 z->name = p;
 z->renamed = 1;
 /* the Info-ZIP Unicode path field holds the old name */
 strip_extra(z->lextra, &z->lextralen, 0x7075);
 strip_extra(z->cextra, &z->cextralen, 0x7075);
}

static void zip_set_comment(ENTRY *e, const char *comment)
{
 struct zip_entry *z = e;
 free(z->comment);
 z->comment = (comment && comment[0]) ? strndup2(comment, strlen(comment)) : NULL;
}

/*
** Amiga protection of an entry: Amiga entries carry it, Unix entries are
** mapped from the owner's rwx, others count as ----rwed
*/
static uint8_t zip_get_protection(ENTRY *e)
{
 struct zip_entry *z = e;
 uint8_t host = (uint8_t)(z->madeby >> 8);
 if(host == HOST_AMIGA) return (uint8_t)((z->external >> 16) ^ 0x0F);
 if(host == HOST_UNIX)
 {
  uint32_t mode = z->external >> 16;
  uint8_t p = 0;
  if(!(mode & 0400)) p |= ARC_PROT_R;
  if(!(mode & 0200)) p |= ARC_PROT_W | ARC_PROT_D;
  if(!(mode & 0100) && !((mode & 0170000) == 0040000)) p |= ARC_PROT_E;
  return p;
 }
 return 0;
}

static void zip_set_protection(ENTRY *e, uint8_t prot)
{
 struct zip_entry *z = e;
 /* an Amiga entry from now on; keep the MS-DOS attributes (directory bit) */
 z->madeby = (uint16_t)((HOST_AMIGA << 8) | (z->madeby & 0xFF));
 z->external = ((uint32_t)(prot ^ 0x0F) << 16) | (z->external & 0xFF);
}

static void zip_remove(ARC *arc, ENTRY *e)
{
 struct zip_archive *z = arc;
 struct zip_entry **link = &z->first;
 while(*link && *link != e) link = &(*link)->next;
 if(*link)
 {
  *link = ((struct zip_entry *)e)->next;
  entry_free(e);
 }
}

/*
** Add an entry from memory: deflated, or stored when that is smaller
*/
static int zip_add(ARC *arc, const char *path, const uint8_t *buf, uint32_t len,
                   uint8_t prot, const char *comment, uint32_t seconds)
{
 struct zip_archive *z = arc;
 struct zip_entry *e = calloc(1, sizeof(*e)), **link;
 uint32_t dt = lha_unix_to_dos(seconds), max = deflate_bound(len), clen;
 int isdir = len == 0 && path[0] && path[strlen(path) - 1] == '/';

 if(e == NULL) return ZIP_ERR_MEM;
 e->name = strndup2(path, strlen(path));
 e->comment = (comment && comment[0]) ? strndup2(comment, strlen(comment)) : NULL;
 e->cextra = malloc(1);
 e->lextra = malloc(1);
 e->data = malloc(max);
 if(e->name == NULL || e->cextra == NULL || e->lextra == NULL || e->data == NULL)
 {
  entry_free(e);
  return ZIP_ERR_MEM;
 }
 e->madeby = (HOST_AMIGA << 8) | 20;
 e->needed = 20;
 e->time = (uint16_t)dt;
 e->date = (uint16_t)(dt >> 16);
 e->crc = len ? crc32_update(0, buf, len) : 0;
 e->usize = len;
 e->external = ((uint32_t)(prot ^ 0x0F) << 16) | (isdir ? 0x10 : 0);
 clen = len ? deflate_compress(buf, len, e->data, max) : 0;
 if(len && clen && clen < len)
 {
  e->method = 8;
  e->csize = clen;
 }
 else
 {
  e->method = 0;
  e->needed = isdir ? 20 : 10;
  if(len) memcpy(e->data, buf, len);
  e->csize = len;
 }

 /* replace an entry with the same name, else append */
 for(link = &z->first; *link; link = &(*link)->next)
 {
  if(lha_pathcmp((*link)->name, path) == 0)
  {
   struct zip_entry *old = *link;
   e->next = old->next;
   *link = e;
   entry_free(old);
   return ZIP_OK;
  }
 }
 *link = e;
 return ZIP_OK;
}

const struct arc_format ZipFormat =
{
 "ZIP",
 zip_probe,
 zip_probe_name,
 zip_open,
 zip_close,
 zip_write,
 zip_error_text,
 zip_caps,
 zip_first,
 zip_next,
 zip_path,
 zip_set_path,
 zip_set_comment,
 zip_get_protection,
 zip_set_protection,
 zip_remove,
 zip_add,
 NULL,
 NULL,
 NULL
};
