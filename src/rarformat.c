/*
** File:        rarformat.c
** Description: The RAR backend of arcedit (RAR 5 archive format, header
**              editing only).
**
**  RAR 5 archives (signature "Rar!\x1A\x07\x01\x00") are a list of
**  blocks: CRC-32 of the header, header size (vint), header (type,
**  flags, optional extra area and data size, type fields), then the
**  data. arcedit works on the headers only: it renames and deletes
**  entries and adds files stored (method 0), copying all packed data as it
**  is. It doesn't pack: the RAR compression method is not public, and the
**  UnRAR source license forbids using it to write a RAR compressor.
**
**  Not allowed: deleting or adding in solid archives (the following
**  entries depend on the earlier ones), archives with encrypted headers,
**  multi-volume archives. RAR 5 has no per-file comments. Protection is
**  taken from the Unix mode (owner r w x) or the Windows read-only flag.
**  The quick open data and its locator are dropped on writing (their
**  offsets would be wrong); RAR rebuilds them when it changes the archive.
**  RAR 4 archives are not handled.
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

#define RAR_OK          0
#define RAR_ERR_IO      1
#define RAR_ERR_FORMAT  2
#define RAR_ERR_MEM     3
#define RAR_ERR_RAR4    4
#define RAR_ERR_ENCRYPT 5
#define RAR_ERR_VOLUME  6

#define HFL_EXTRA 0x0001
#define HFL_DATA  0x0002

#define FFL_DIR   0x0001
#define FFL_MTIME 0x0002
#define FFL_CRC   0x0004

#define HOST_WINDOWS 0
#define HOST_UNIX    1

static const uint8_t Signature[8] = { 'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00 };

struct rar_block
{
 struct rar_block *next;
 uint32_t type;
 int isentry;               /* a file header (type 2) */
 int keep;                  /* other blocks: 0 = dropped on writing */

 long rawoff;               /* the block in the source */
 uint64_t rawlen;           /* header + data */
 long dataoff;
 uint64_t datalen;

 /* file header fields */
 uint64_t hflags, fflags, usize, attr, compinfo, hostos;
 uint32_t mtime, crc;
 uint8_t *extra;            /* extra area */
 uint64_t extralen;
 char *rawname;             /* UTF-8 as stored */
 char *path;                /* Latin-1, drawers end with '/' */
 int modified;
 uint8_t *data;             /* added entries */
};

struct rar_archive
{
 FILE *fh;
 uint8_t *prefix;           /* self-extracting code before the signature */
 uint32_t prefixlen;
 uint64_t arcflags;
 struct rar_block *first;
};

static const char *rar_error_text(int err)
{
 switch(err)
 {
  case RAR_OK:          return "no error";
  case RAR_ERR_IO:      return "read/write error";
  case RAR_ERR_FORMAT:  return "not a RAR 5 archive or damaged";
  case RAR_ERR_MEM:     return "out of memory";
  case RAR_ERR_RAR4:    return "RAR 4 archives are not supported";
  case RAR_ERR_ENCRYPT: return "archives with encrypted headers can't be edited";
  case RAR_ERR_VOLUME:  return "multi-volume archives can't be edited";
 }
 return "unknown error";
}

/*
** Read a vint (7 bits per byte, low first, high bit = more). Returns the
** bytes used, 0 on bad data.
*/
static int vint(const uint8_t *p, uint64_t avail, uint64_t *v)
{
 int i;
 *v = 0;
 for(i = 0; i < 10 && (uint64_t)i < avail; i++)
 {
  *v |= (uint64_t)(p[i] & 0x7F) << (7 * i);
  if(!(p[i] & 0x80)) return i + 1;
 }
 return 0;
}

static int putvint(uint8_t *p, uint64_t v)
{
 int n = 0;
 do
 {
  p[n] = (uint8_t)(v & 0x7F);
  v >>= 7;
  if(v) p[n] |= 0x80;
  n++;
 }
 while(v);
 return n;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/*
** UTF-8 to Latin-1 (other characters become '?') and back
*/
static char *utf8_to_latin1(const uint8_t *s, size_t n)
{
 char *r = malloc(n + 1), *o = r;
 size_t i = 0;
 if(r == NULL) return NULL;
 while(i < n)
 {
  uint8_t c = s[i];
  if(c < 0x80)
  {
   *o++ = (char)c;
   i++;
  }
  else if((c & 0xE0) == 0xC0 && i + 1 < n)
  {
   uint32_t u = ((uint32_t)(c & 0x1F) << 6) | (s[i + 1] & 0x3F);
   *o++ = (char)(u < 256 ? u : '?');
   i += 2;
  }
  else
  {
   *o++ = '?';
   i++;
   while(i < n && (s[i] & 0xC0) == 0x80) i++;
  }
 }
 *o = 0;
 return r;
}

static char *latin1_to_utf8(const char *s)
{
 size_t n = strlen(s), i;
 char *r = malloc(2 * n + 1), *o = r;
 if(r == NULL) return NULL;
 for(i = 0; i < n; i++)
 {
  uint8_t c = (uint8_t)s[i];
  if(c < 0x80) *o++ = (char)c;
  else
  {
   *o++ = (char)(0xC0 | (c >> 6));
   *o++ = (char)(0x80 | (c & 0x3F));
  }
 }
 *o = 0;
 return r;
}

static void block_free(struct rar_block *b)
{
 free(b->extra);
 free(b->rawname);
 free(b->path);
 free(b->data);
 free(b);
}

static void rar_close(ARC *arc)
{
 struct rar_archive *r = arc;
 if(r == NULL) return;
 while(r->first)
 {
  struct rar_block *b = r->first;
  r->first = b->next;
  block_free(b);
 }
 if(r->fh) fclose(r->fh);
 free(r->prefix);
 free(r);
}

static int rar_probe(const uint8_t *head, uint32_t len)
{
 uint32_t i;
 for(i = 0; i + 7 <= len; i++)
 {
  /* RAR 5, and RAR 4 ("Rar!\x1A\x07\x00") so that it's reported */
  if(memcmp(head + i, Signature, 6) == 0 && (head[i + 6] == 0x01 || head[i + 6] == 0x00)) return i == 0 ? 2 : 1;
 }
 return 0;
}

static int rar_probe_name(const char *path)
{
 (void)path;
 return 0;   /* arcedit can't create RAR archives */
}

/*
** Parse a file header (type 2) from the header bytes after type and flags
*/
static int parse_file(struct rar_block *b, const uint8_t *h, uint64_t len, uint64_t extralen)
{
 uint64_t pos = 0, namelen;
 int n;
#define VINT(v) do { if((n = vint(h + pos, len - pos, &(v))) == 0) return RAR_ERR_FORMAT; pos += (uint64_t)n; } while(0)
 VINT(b->fflags);
 VINT(b->usize);
 VINT(b->attr);
 if(b->fflags & FFL_MTIME)
 {
  if(pos + 4 > len) return RAR_ERR_FORMAT;
  b->mtime = rd32(h + pos);
  pos += 4;
 }
 if(b->fflags & FFL_CRC)
 {
  if(pos + 4 > len) return RAR_ERR_FORMAT;
  b->crc = rd32(h + pos);
  pos += 4;
 }
 VINT(b->compinfo);
 VINT(b->hostos);
 VINT(namelen);
#undef VINT
 if(pos + namelen + extralen > len) return RAR_ERR_FORMAT;
 b->rawname = malloc((size_t)namelen + 1);
 b->path = utf8_to_latin1(h + pos, (size_t)namelen);
 if(b->rawname == NULL || b->path == NULL) return RAR_ERR_MEM;
 memcpy(b->rawname, h + pos, (size_t)namelen);
 b->rawname[namelen] = 0;
 pos += namelen;
 b->extralen = extralen;
 b->extra = malloc(extralen ? (size_t)extralen : 1);
 if(b->extra == NULL) return RAR_ERR_MEM;
 memcpy(b->extra, h + len - extralen, (size_t)extralen);
 if(b->fflags & FFL_DIR)
 {
  size_t pl = strlen(b->path);
  char *p = realloc(b->path, pl + 2);
  if(p == NULL) return RAR_ERR_MEM;
  p[pl] = '/';
  p[pl + 1] = 0;
  b->path = p;
 }
 return RAR_OK;
}

static ARC *rar_open(const char *path, int create, int *err)
{
 struct rar_archive *r = calloc(1, sizeof(*r));
 struct rar_block **link;
 uint8_t *buf = NULL;
 long off;

 (void)create;
 *err = RAR_ERR_MEM;
 if(r == NULL) return NULL;
 r->fh = fopen(path, "rb");
 if(r->fh == NULL)
 {
  *err = RAR_ERR_IO;
  free(r);
  return NULL;
 }
 buf = malloc(1024 * 1024);
 if(buf == NULL) goto fail;

 /* the signature, after self-extracting code */
 {
  size_t got = fread(buf, 1, 1024 * 1024, r->fh), i;
  *err = RAR_ERR_FORMAT;
  for(i = 0; i + 8 <= got; i++)
  {
   if(memcmp(buf + i, Signature, 6) == 0 && (buf[i + 6] == 0x00 || buf[i + 6] == 0x01)) break;
  }
  if(i + 8 > got) goto fail;
  if(buf[i + 6] == 0x00)
  {
   *err = RAR_ERR_RAR4;
   goto fail;
  }
  if(buf[i + 7] != 0x00) goto fail;
  r->prefixlen = (uint32_t)i;
  if(i)
  {
   r->prefix = malloc(i);
   if(r->prefix == NULL)
   {
    *err = RAR_ERR_MEM;
    goto fail;
   }
   memcpy(r->prefix, buf, i);
  }
  off = (long)i + 8;
 }

 link = &r->first;
 for(;;)
 {
  struct rar_block *b;
  uint8_t pre[16];
  uint64_t hsize, type, hflags, extralen = 0, datalen = 0, pos;
  int n;
  size_t got;
  if(fseek(r->fh, off, SEEK_SET) != 0) break;
  got = fread(pre, 1, sizeof(pre), r->fh);
  if(got == 0) break;   /* no end header: end of file */
  *err = RAR_ERR_FORMAT;
  if(got < 7 || (n = vint(pre + 4, got - 4, &hsize)) == 0 || hsize == 0 || hsize > 2 * 1024 * 1024) goto fail;
  {
   uint64_t total = 4 + (uint64_t)n + hsize;
   uint8_t *h = total > 1024 * 1024 ? NULL : buf;
   if(h == NULL)
   {
    *err = RAR_ERR_MEM;
    goto fail;
   }
   if(fseek(r->fh, off, SEEK_SET) != 0 || fread(h, 1, (size_t)total, r->fh) != total)
   {
    *err = RAR_ERR_IO;
    goto fail;
   }
   if(crc32_update(0, h + 4, (uint32_t)(total - 4)) != rd32(h)) goto fail;
   pos = 4 + (uint64_t)n;
   if((n = vint(h + pos, total - pos, &type)) == 0) goto fail;
   pos += (uint64_t)n;
   if((n = vint(h + pos, total - pos, &hflags)) == 0) goto fail;
   pos += (uint64_t)n;
   if(hflags & HFL_EXTRA)
   {
    if((n = vint(h + pos, total - pos, &extralen)) == 0) goto fail;
    pos += (uint64_t)n;
   }
   if(hflags & HFL_DATA)
   {
    if((n = vint(h + pos, total - pos, &datalen)) == 0) goto fail;
    pos += (uint64_t)n;
   }
   b = calloc(1, sizeof(*b));
   if(b == NULL)
   {
    *err = RAR_ERR_MEM;
    goto fail;
   }
   *link = b;
   link = &b->next;
   b->type = (uint32_t)type;
   b->hflags = hflags;
   b->rawoff = off;
   b->dataoff = off + (long)total;
   b->datalen = datalen;
   b->rawlen = total + datalen;
   b->keep = 1;
   if(type == 1)
   {
    uint64_t af;
    if((n = vint(h + pos, total - pos, &af)) == 0) goto fail;
    r->arcflags = af;
    if(af & 0x0001)
    {
     *err = RAR_ERR_VOLUME;
     goto fail;
    }
    b->keep = 0;   /* rebuilt without the locator */
   }
   else if(type == 2)
   {
    int e;
    b->isentry = 1;
    if((e = parse_file(b, h + pos, total - pos, extralen)) != RAR_OK)
    {
     *err = e;
     goto fail;
    }
   }
   else if(type == 3)
   {
    /* service headers: same fields as a file header; quick open (QO) is dropped */
    int e;
    if((e = parse_file(b, h + pos, total - pos, extralen)) != RAR_OK)
    {
     *err = e;
     goto fail;
    }
    if(strcmp(b->rawname, "QO") == 0) b->keep = 0;
   }
   else if(type == 4)
   {
    *err = RAR_ERR_ENCRYPT;
    goto fail;
   }
   else if(type == 5)
   {
    b->keep = 0;   /* rebuilt */
    off += (long)b->rawlen;
    break;
   }
   off += (long)b->rawlen;
  }
 }
 free(buf);
 *err = RAR_OK;
 return r;

fail:
 free(buf);
 rar_close(r);
 return NULL;
}

/*
** Write one block: CRC, size, header (h, hl bytes from the type on)
*/
static int put_block(FILE *out, const uint8_t *h, uint32_t hl)
{
 uint8_t pre[16];
 int n = putvint(pre + 4, hl);
 uint32_t crc = crc32_update(0, pre + 4, (uint32_t)n);
 crc = crc32_update(crc, h, hl);
 wr32(pre, crc);
 if(fwrite(pre, 1, 4 + (size_t)n, out) != 4 + (size_t)n || fwrite(h, 1, hl, out) != hl) return RAR_ERR_IO;
 return RAR_OK;
}

/*
** Build a file header (type 2) from the fields
*/
static uint32_t build_file(struct rar_block *b, uint8_t *h)
{
 uint32_t p = 0;
 size_t nl = strlen(b->rawname);
 uint64_t hflags = (b->extralen ? HFL_EXTRA : 0) | (b->datalen || !(b->fflags & FFL_DIR) ? HFL_DATA : 0);
 hflags |= b->hflags & ~(uint64_t)(HFL_EXTRA | HFL_DATA);
 p += (uint32_t)putvint(h + p, 2);
 p += (uint32_t)putvint(h + p, hflags);
 if(hflags & HFL_EXTRA) p += (uint32_t)putvint(h + p, b->extralen);
 if(hflags & HFL_DATA) p += (uint32_t)putvint(h + p, b->datalen);
 p += (uint32_t)putvint(h + p, b->fflags);
 p += (uint32_t)putvint(h + p, b->usize);
 p += (uint32_t)putvint(h + p, b->attr);
 if(b->fflags & FFL_MTIME)
 {
  wr32(h + p, b->mtime);
  p += 4;
 }
 if(b->fflags & FFL_CRC)
 {
  wr32(h + p, b->crc);
  p += 4;
 }
 p += (uint32_t)putvint(h + p, b->compinfo);
 p += (uint32_t)putvint(h + p, b->hostos);
 p += (uint32_t)putvint(h + p, nl);
 memcpy(h + p, b->rawname, nl);
 p += (uint32_t)nl;
 memcpy(h + p, b->extra, (size_t)b->extralen);
 p += (uint32_t)b->extralen;
 return p;
}

static int rar_write(ARC *arc, const char *path)
{
 struct rar_archive *r = arc;
 struct rar_block *b;
 FILE *out = fopen(path, "wb");
 uint8_t *copy = malloc(64 * 1024), *h = malloc(64 * 1024 + 8192);
 int err = RAR_OK;

 if(out == NULL || copy == NULL || h == NULL)
 {
  err = out ? RAR_ERR_MEM : RAR_ERR_IO;
  goto done;
 }
 if(r->prefixlen && fwrite(r->prefix, 1, r->prefixlen, out) != r->prefixlen) err = RAR_ERR_IO;
 if(!err && fwrite(Signature, 1, 8, out) != 8) err = RAR_ERR_IO;
 /* main header: the archive flags, no locator */
 if(!err)
 {
  uint32_t p = 0;
  p += (uint32_t)putvint(h + p, 1);
  p += (uint32_t)putvint(h + p, 0);
  p += (uint32_t)putvint(h + p, r->arcflags & ~(uint64_t)0x0002);
  err = put_block(out, h, p);
 }
 for(b = r->first; b && !err; b = b->next)
 {
  uint64_t left;
  if(!b->isentry && !b->keep) continue;
  if(b->modified || b->data)
  {
   if(strlen(b->rawname) + b->extralen > 60 * 1024)
   {
    err = RAR_ERR_FORMAT;
    break;
   }
   err = put_block(out, h, build_file(b, h));
   if(err) break;
   if(b->data)
   {
    if(b->datalen && fwrite(b->data, 1, (size_t)b->datalen, out) != b->datalen) err = RAR_ERR_IO;
    continue;
   }
   left = b->datalen;
   if(fseek(r->fh, b->dataoff, SEEK_SET) != 0) err = RAR_ERR_IO;
  }
  else
  {
   left = b->rawlen;
   if(fseek(r->fh, b->rawoff, SEEK_SET) != 0) err = RAR_ERR_IO;
  }
  while(left && !err)
  {
   size_t n = left > 64 * 1024 ? 64 * 1024 : (size_t)left;
   if(fread(copy, 1, n, r->fh) != n || fwrite(copy, 1, n, out) != n) err = RAR_ERR_IO;
   left -= n;
  }
 }
 /* end of archive */
 if(!err)
 {
  uint32_t p = 0;
  p += (uint32_t)putvint(h + p, 5);
  p += (uint32_t)putvint(h + p, 0x0004);   /* skip if unknown, as RAR writes it */
  p += (uint32_t)putvint(h + p, 0);
  err = put_block(out, h, p);
 }
done:
 if(out && fclose(out) != 0 && !err) err = RAR_ERR_IO;
 free(copy);
 free(h);
 return err;
}

static uint32_t rar_caps(ARC *arc)
{
 struct rar_archive *r = arc;
 /* solid: the entries depend on the earlier ones */
 if(r->arcflags & 0x0004) return ARC_CAN_RENAME | ARC_CAN_PROTECT;
 return ARC_CAN_DELETE | ARC_CAN_RENAME | ARC_CAN_PROTECT | ARC_CAN_ADD;
}

static ENTRY *rar_first(ARC *arc)
{
 struct rar_block *b = ((struct rar_archive *)arc)->first;
 while(b && !b->isentry) b = b->next;
 return b;
}

static ENTRY *rar_next(ARC *arc, ENTRY *e)
{
 struct rar_block *b = ((struct rar_block *)e)->next;
 (void)arc;
 while(b && !b->isentry) b = b->next;
 return b;
}

static const char *rar_path(ENTRY *e)
{
 return ((struct rar_block *)e)->path;
}

static void rar_set_path(ENTRY *e, const char *path)
{
 struct rar_block *b = e;
 size_t n = strlen(path);
 char *p = malloc(n + 2), *raw;
 if(p == NULL) return;
 memcpy(p, path, n + 1);
 /* the stored name has no '/' at the end, the path of a drawer has one */
 while(n && p[n - 1] == '/') p[--n] = 0;
 raw = latin1_to_utf8(p);
 if(raw == NULL)
 {
  free(p);
  return;
 }
 if(b->fflags & FFL_DIR)
 {
  p[n] = '/';
  p[n + 1] = 0;
 }
 free(b->path);
 free(b->rawname);
 b->path = p;
 b->rawname = raw;
 b->modified = 1;
}

static uint8_t rar_get_protection(ENTRY *e)
{
 struct rar_block *b = e;
 uint8_t p = 0;
 if(b->hostos == HOST_UNIX)
 {
  if(!(b->attr & 0400)) p |= ARC_PROT_R;
  if(!(b->attr & 0200)) p |= ARC_PROT_W | ARC_PROT_D;
  if(!(b->attr & 0100) && !(b->fflags & FFL_DIR)) p |= ARC_PROT_E;
 }
 else if(b->attr & 0x01)   /* Windows read-only */
 {
  p |= ARC_PROT_W | ARC_PROT_D;
 }
 return p;
}

static void rar_set_protection(ENTRY *e, uint8_t prot)
{
 struct rar_block *b = e;
 uint64_t attr = b->attr;
 if(b->hostos == HOST_UNIX)
 {
  attr &= ~(uint64_t)0700;
  if(!(prot & ARC_PROT_R)) attr |= 0400;
  if(!(prot & ARC_PROT_W)) attr |= 0200;
  if(!(prot & ARC_PROT_E) || (b->fflags & FFL_DIR)) attr |= 0100;
 }
 else
 {
  attr = (prot & ARC_PROT_W) ? (attr | 0x01) : (attr & ~(uint64_t)0x01);
 }
 if(attr != b->attr)
 {
  b->attr = attr;
  b->modified = 1;
 }
}

static void rar_remove(ARC *arc, ENTRY *e)
{
 struct rar_archive *r = arc;
 struct rar_block **link = &r->first;
 while(*link && *link != e) link = &(*link)->next;
 if(*link)
 {
  *link = ((struct rar_block *)e)->next;
  block_free(e);
 }
}

/*
** Add a file stored (RAR packing isn't available), Unix attributes
*/
static int rar_add(ARC *arc, const char *path, const uint8_t *buf, uint32_t len,
                   uint8_t prot, const char *comment, uint32_t seconds)
{
 struct rar_archive *r = arc;
 struct rar_block *b = calloc(1, sizeof(*b)), **link;
 int isdir = len == 0 && path[0] && path[strlen(path) - 1] == '/';

 (void)comment;
 if(b == NULL) return RAR_ERR_MEM;
 b->type = 2;
 b->isentry = 1;
 b->keep = 1;
 b->fflags = FFL_MTIME | (isdir ? FFL_DIR : FFL_CRC);
 b->usize = len;
 b->attr = isdir ? 040755 : 0100644;
 b->hostos = HOST_UNIX;
 b->mtime = seconds;
 b->crc = isdir ? 0 : crc32_update(0, buf, len);
 b->compinfo = 0;          /* version 0, stored */
 b->datalen = isdir ? 0 : len;
 b->extra = malloc(1);
 b->rawname = malloc(1);
 b->path = malloc(1);
 b->data = malloc(len ? len : 1);
 if(b->extra == NULL || b->rawname == NULL || b->path == NULL || b->data == NULL)
 {
  block_free(b);
  return RAR_ERR_MEM;
 }
 b->rawname[0] = b->path[0] = 0;
 if(len) memcpy(b->data, buf, len);
 rar_set_path(b, path);
 if(b->path[0] == 0)
 {
  block_free(b);
  return RAR_ERR_MEM;
 }
 rar_set_protection(b, prot);
 b->modified = 1;

 for(link = &r->first; *link; link = &(*link)->next)
 {
  if((*link)->isentry && lha_pathcmp((*link)->path, b->path) == 0)
  {
   struct rar_block *old = *link;
   b->next = old->next;
   *link = b;
   block_free(old);
   return RAR_OK;
  }
 }
 /* after the last block (before the end header, which is rebuilt) */
 *link = b;
 return RAR_OK;
}

const struct arc_format RarFormat =
{
 "RAR",
 rar_probe,
 rar_probe_name,
 rar_open,
 rar_close,
 rar_write,
 rar_error_text,
 rar_caps,
 rar_first,
 rar_next,
 rar_path,
 rar_set_path,
 NULL,                /* no per-file comments */
 rar_get_protection,
 rar_set_protection,
 rar_remove,
 rar_add,
 NULL
};
