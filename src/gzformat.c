/*
** File:        gzformat.c
** Description: The gzip backend of arcedit: .gz files and .tgz/.tar.gz.
**
**  A gzip file (RFC 1952) holds one packed file: a header with the
**  original name (FNAME) and an optional comment (FCOMMENT), the Deflate
**  data, then CRC-32 and size. arcedit can rename it (FNAME), set the
**  comment and replace the content (ADD).
**
**  When the content is a tar archive (.tgz, .tar.gz) the archive is
**  unpacked into a temporary file next to it and the TAR backend works on
**  that; writing packs the edited tar again. This is the only format that
**  needs unpacking: gzip packs the whole tar as one stream.
**
** Change history:
**  2026-11-14 orgin, created
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcformat.h"
#include "deflate.h"
#include "tar.h"
#include "lha.h"      /* lha_pathcmp */
#include "platform.h"

#define GZ_OK         0
#define GZ_ERR_IO     1
#define GZ_ERR_FORMAT 2
#define GZ_ERR_MEM    3
#define GZ_ERR_ONE    4
#define GZ_ERR_SPACE  5
#define GZ_ERR_TAR    100   /* + a TAR backend error */

#define FTEXT    0x01
#define FHCRC    0x02
#define FEXTRA   0x04
#define FNAME    0x08
#define FCOMMENT 0x10

struct gz_archive
{
 char *archive;           /* the archive's own name */
 uint8_t flags, xfl, os;
 uint32_t mtime;
 uint8_t *extra;          /* FEXTRA field */
 uint16_t extralen;
 char *name;              /* FNAME, or the archive name without ".gz" */
 char *comment;

 /* plain .gz: the packed data in the source, or new data */
 FILE *fh;
 long dataoff, datalen;   /* data + CRC + size of all members */
 uint8_t *newdata;        /* ADD: the new packed member (deflate + trailer) */
 uint32_t newlen;
 int added;               /* ADD was used once already */

 /* .tgz: the tar backend on an unpacked copy */
 ARC *tar;
 char tarname[1024];
 int tarerr;
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
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

static const char *gz_error_text(int err)
{
 if(err >= GZ_ERR_TAR) return TarFormat.error_text(err - GZ_ERR_TAR);
 switch(err)
 {
  case GZ_OK:         return "no error";
  case GZ_ERR_IO:     return "read/write error";
  case GZ_ERR_FORMAT: return "not a gzip file or damaged";
  case GZ_ERR_MEM:    return "out of memory";
  case GZ_ERR_ONE:    return "a gzip file holds one file (use .tgz for more)";
  case GZ_ERR_SPACE:  return "not enough free space to unpack the tar";
 }
 return "unknown error";
}

static int gz_probe(const uint8_t *head, uint32_t len)
{
 return (len >= 18 && head[0] == 0x1F && head[1] == 0x8B && head[2] == 8) ? 2 : 0;
}

static int ends_with(const char *s, const char *tail)
{
 size_t n = strlen(s), t = strlen(tail);
 return n > t && lha_pathcmp(s + n - t, tail) == 0;
}

static int gz_probe_name(const char *path)
{
 return ends_with(path, ".gz") || ends_with(path, ".tgz");
}

static int is_tar_name(const char *path)
{
 return ends_with(path, ".tgz") || ends_with(path, ".tar.gz");
}

static void gz_close(ARC *arc)
{
 struct gz_archive *g = arc;
 if(g == NULL) return;
 if(g->tar)
 {
  tar_close_file(g->tar);
  plat_delete(g->tarname);
 }
 if(g->fh) fclose(g->fh);
 free(g->archive);
 free(g->extra);
 free(g->name);
 free(g->comment);
 free(g->newdata);
 free(g);
}

/*
** The name of the packed file when the header has none: the archive's
** name without ".gz" (".tgz" gives ".tar")
*/
static char *default_name(const char *archive)
{
 const char *base = plat_filepart(archive);
 size_t n = strlen(base);
 char *r = malloc(n + 2);
 if(r == NULL) return NULL;
 strcpy(r, base);
 if(ends_with(r, ".tgz")) strcpy(r + n - 4, ".tar");
 else if(ends_with(r, ".gz")) r[n - 3] = 0;
 return r;
}

static int read_zstring(FILE *fh, char **out)
{
 size_t max = 256, n = 0;
 char *s = malloc(max);
 int c;
 if(s == NULL) return GZ_ERR_MEM;
 while((c = fgetc(fh)) != EOF && c != 0)
 {
  if(n + 1 == max)
  {
   char *p = realloc(s, max *= 2);
   if(p == NULL)
   {
    free(s);
    return GZ_ERR_MEM;
   }
   s = p;
  }
  s[n++] = (char)c;
 }
 s[n] = 0;
 *out = s;
 return c == EOF ? GZ_ERR_FORMAT : GZ_OK;
}

static ARC *gz_open(const char *path, int create, int *err)
{
 struct gz_archive *g = calloc(1, sizeof(*g));
 uint8_t h[10];
 long end;

 *err = GZ_ERR_MEM;
 if(g == NULL) return NULL;
 g->archive = strndup2(path, strlen(path));
 if(g->archive == NULL) goto fail;
 g->fh = fopen(path, "rb");
 if(g->fh == NULL)
 {
  if(!create)
  {
   *err = GZ_ERR_IO;
   goto fail;
  }
  /* a new archive */
  g->os = 1;   /* Amiga */
  g->name = default_name(path);
  if(is_tar_name(path))
  {
   int e;
   snprintf(g->tarname, sizeof(g->tarname), "%s.arcedit-tar", path);
   g->tar = tar_open_file(g->tarname, 1, &e);
   if(g->tar == NULL)
   {
    *err = GZ_ERR_TAR + e;
    goto fail;
   }
  }
  *err = GZ_OK;
  return g;
 }

 *err = GZ_ERR_FORMAT;
 if(fread(h, 1, 10, g->fh) != 10 || h[0] != 0x1F || h[1] != 0x8B || h[2] != 8) goto fail;
 g->flags = h[3];
 g->mtime = rd32(h + 4);
 g->xfl = h[8];
 g->os = h[9];
 if(g->flags & FEXTRA)
 {
  uint8_t x[2];
  if(fread(x, 1, 2, g->fh) != 2) goto fail;
  g->extralen = (uint16_t)(x[0] | (x[1] << 8));
  g->extra = malloc(g->extralen ? g->extralen : 1);
  if(g->extra == NULL || fread(g->extra, 1, g->extralen, g->fh) != g->extralen) goto fail;
 }
 if(g->flags & FNAME)
 {
  if((*err = read_zstring(g->fh, &g->name)) != GZ_OK) goto fail;
 }
 if(g->flags & FCOMMENT)
 {
  if((*err = read_zstring(g->fh, &g->comment)) != GZ_OK) goto fail;
  if(g->comment[0] == 0)
  {
   free(g->comment);
   g->comment = NULL;
  }
 }
 if(g->flags & FHCRC) fseek(g->fh, 2, SEEK_CUR);
 *err = GZ_ERR_FORMAT;
 if(g->name == NULL && (g->name = default_name(path)) == NULL)
 {
  *err = GZ_ERR_MEM;
  goto fail;
 }
 g->dataoff = ftell(g->fh);
 if(fseek(g->fh, 0, SEEK_END) != 0 || (end = ftell(g->fh)) < g->dataoff + 8) goto fail;
 g->datalen = end - g->dataoff;

 /* a tar inside? unpack it for the TAR backend */
 {
  uint8_t *packed = malloc((size_t)g->datalen), *plain = NULL;
  uint32_t plainlen = 0, used = 0;
  int r;
  if(packed == NULL)
  {
   *err = GZ_ERR_MEM;
   goto fail;
  }
  if(fseek(g->fh, g->dataoff, SEEK_SET) != 0 || fread(packed, 1, (size_t)g->datalen, g->fh) != (size_t)g->datalen)
  {
   free(packed);
   *err = GZ_ERR_IO;
   goto fail;
  }
  r = inflate_raw(packed, (uint32_t)g->datalen, &plain, &plainlen, &used);
  /* more members after the first: unpack them too */
  while(r == 0 && used + 8 <= (uint32_t)g->datalen)
  {
   if(used + 8 == (uint32_t)g->datalen) break;
   {
    /* next member: skip its header */
    uint32_t p = used + 8, fl;
    uint8_t *more = NULL;
    uint32_t morelen = 0, mused = 0;
    if(p + 10 > (uint32_t)g->datalen || packed[p] != 0x1F || packed[p + 1] != 0x8B) break;
    fl = packed[p + 3];
    p += 10;
    if(fl & FEXTRA) p += 2u + (packed[p] | (packed[p + 1] << 8));
    if(fl & FNAME) while(p < (uint32_t)g->datalen && packed[p++]);
    if(fl & FCOMMENT) while(p < (uint32_t)g->datalen && packed[p++]);
    if(fl & FHCRC) p += 2;
    if(p >= (uint32_t)g->datalen || inflate_raw(packed + p, (uint32_t)g->datalen - p, &more, &morelen, &mused) != 0)
    {
     r = -1;
     break;
    }
    {
     uint8_t *n = realloc(plain, plainlen + morelen + 1);
     if(n == NULL)
     {
      free(more);
      r = -2;
      break;
     }
     memcpy(n + plainlen, more, morelen);
     plain = n;
     plainlen += morelen;
     free(more);
    }
    used = p + mused;
   }
  }
  free(packed);
  if(r != 0)
  {
   *err = r == -2 ? GZ_ERR_MEM : GZ_ERR_FORMAT;
   goto fail;
  }
  if(plainlen >= 512 && TarFormat.probe(plain, plainlen) == 2)
  {
   FILE *t;
   int e;
   uint64_t freebytes;
   /* room for the unpacked tar next to the archive */
   if(plat_free_space(path, &freebytes) == 0 && freebytes < (uint64_t)plainlen + 65536)
   {
    free(plain);
    *err = GZ_ERR_SPACE;
    goto fail;
   }
   snprintf(g->tarname, sizeof(g->tarname), "%s.arcedit-tar", path);
   t = fopen(g->tarname, "wb");
   if(t == NULL || fwrite(plain, 1, plainlen, t) != plainlen)
   {
    if(t) fclose(t);
    free(plain);
    *err = GZ_ERR_IO;
    goto fail;
   }
   fclose(t);
   g->tar = tar_open_file(g->tarname, 0, &e);
   if(g->tar == NULL)
   {
    free(plain);
    plat_delete(g->tarname);
    *err = GZ_ERR_TAR + e;
    goto fail;
   }
  }
  free(plain);
 }
 *err = GZ_OK;
 return g;

fail:
 gz_close(g);
 return NULL;
}

/*
** A gzip member: header (name, comment, extra as they are) + Deflate of
** buf + CRC and size. The packed part is newdata when set.
*/
static int write_member(struct gz_archive *g, FILE *out, const uint8_t *packed, uint32_t packedlen)
{
 uint8_t h[10];
 uint8_t flags = (uint8_t)(g->flags & FTEXT);
 if(g->extra) flags |= FEXTRA;
 if(g->name && g->name[0]) flags |= FNAME;
 if(g->comment) flags |= FCOMMENT;
 h[0] = 0x1F;
 h[1] = 0x8B;
 h[2] = 8;
 h[3] = flags;
 wr32(h + 4, g->mtime);
 h[8] = g->xfl;
 h[9] = g->os;
 if(fwrite(h, 1, 10, out) != 10) return GZ_ERR_IO;
 if(flags & FEXTRA)
 {
  uint8_t x[2];
  x[0] = (uint8_t)g->extralen;
  x[1] = (uint8_t)(g->extralen >> 8);
  if(fwrite(x, 1, 2, out) != 2 || fwrite(g->extra, 1, g->extralen, out) != g->extralen) return GZ_ERR_IO;
 }
 if((flags & FNAME) && fwrite(g->name, 1, strlen(g->name) + 1, out) != strlen(g->name) + 1) return GZ_ERR_IO;
 if((flags & FCOMMENT) && fwrite(g->comment, 1, strlen(g->comment) + 1, out) != strlen(g->comment) + 1) return GZ_ERR_IO;
 if(packedlen && fwrite(packed, 1, packedlen, out) != packedlen) return GZ_ERR_IO;
 return GZ_OK;
}

/*
** Pack buf as Deflate + CRC + size into a new buffer
*/
static int pack(const uint8_t *buf, uint32_t len, uint8_t **out, uint32_t *outlen)
{
 uint32_t max = deflate_bound(len) + 8, n;
 *out = malloc(max);
 if(*out == NULL) return GZ_ERR_MEM;
 n = deflate_compress(buf, len, *out, max - 8);
 if(n == 0)
 {
  free(*out);
  return GZ_ERR_MEM;
 }
 wr32(*out + n, crc32_update(0, buf, len));
 wr32(*out + n + 4, len);
 *outlen = n + 8;
 return GZ_OK;
}

static int gz_write(ARC *arc, const char *path)
{
 struct gz_archive *g = arc;
 FILE *out;
 int err = GZ_OK;

 if(g->tar)
 {
  /* write the edited tar, pack it */
  char tmp[1100];
  uint8_t *buf, *packed;
  uint32_t len, plen;
  snprintf(tmp, sizeof(tmp), "%s.arcedit-tar2", g->archive);
  err = tar_write_file(g->tar, tmp);
  if(err)
  {
   plat_delete(tmp);
   return GZ_ERR_TAR + err;
  }
  if(plat_read(tmp, &buf, &len) != 0)
  {
   plat_delete(tmp);
   return GZ_ERR_IO;
  }
  plat_delete(tmp);
  err = pack(buf, len, &packed, &plen);
  free(buf);
  if(err) return err;
  out = fopen(path, "wb");
  if(out == NULL)
  {
   free(packed);
   return GZ_ERR_IO;
  }
  err = write_member(g, out, packed, plen);
  free(packed);
 }
 else
 {
  out = fopen(path, "wb");
  if(out == NULL) return GZ_ERR_IO;
  if(g->newdata)
  {
   err = write_member(g, out, g->newdata, g->newlen);
  }
  else
  {
   /* the packed data (all members) as it is */
   uint8_t *copy = malloc(64 * 1024);
   long left = g->datalen;
   err = write_member(g, out, NULL, 0);
   if(copy == NULL) err = GZ_ERR_MEM;
   if(!err && fseek(g->fh, g->dataoff, SEEK_SET) != 0) err = GZ_ERR_IO;
   while(left > 0 && !err)
   {
    size_t n = left > 64 * 1024 ? 64 * 1024 : (size_t)left;
    if(fread(copy, 1, n, g->fh) != n || fwrite(copy, 1, n, out) != n) err = GZ_ERR_IO;
    left -= (long)n;
   }
   free(copy);
  }
 }
 if(fclose(out) != 0 && !err) err = GZ_ERR_IO;
 return err;
}

static uint32_t gz_caps(ARC *arc)
{
 struct gz_archive *g = arc;
 return g->tar ? TarFormat.caps(g->tar) : (ARC_CAN_RENAME | ARC_CAN_COMMENT | ARC_CAN_ADD);
}

static const char *gz_type_name(ARC *arc)
{
 return ((struct gz_archive *)arc)->tar ? "TGZ" : "GZIP";
}

/* plain .gz: the archive itself is the one entry */
static ENTRY *gz_first(ARC *arc)
{
 struct gz_archive *g = arc;
 if(g->tar) return TarFormat.first(g->tar);
 return (g->fh || g->newdata) ? g : NULL;
}

static ENTRY *gz_next(ARC *arc, ENTRY *e)
{
 struct gz_archive *g = arc;
 return g->tar ? TarFormat.next(g->tar, e) : NULL;
}

/* For a .tgz the entries are the tar's; tell them apart from the archive */
static struct gz_archive *Current;

static const char *gz_path(ENTRY *e)
{
 if(Current && Current->tar) return TarFormat.path(e);
 return ((struct gz_archive *)e)->name;
}

static void gz_set_path(ENTRY *e, const char *path)
{
 if(Current && Current->tar)
 {
  TarFormat.set_path(e, path);
  return;
 }
 {
  struct gz_archive *g = e;
  char *p = strndup2(path, strlen(path));
  if(p)
  {
   free(g->name);
   g->name = p;
  }
 }
}

static void gz_set_comment(ENTRY *e, const char *comment)
{
 if(Current && Current->tar)
 {
  TarFormat.set_comment(e, comment);
  return;
 }
 {
  struct gz_archive *g = e;
  free(g->comment);
  g->comment = (comment && comment[0]) ? strndup2(comment, strlen(comment)) : NULL;
 }
}

static uint8_t gz_get_protection(ENTRY *e)
{
 return (Current && Current->tar) ? TarFormat.get_protection(e) : 0;
}

static void gz_set_protection(ENTRY *e, uint8_t prot)
{
 if(Current && Current->tar) TarFormat.set_protection(e, prot);
}

static void gz_remove(ARC *arc, ENTRY *e)
{
 struct gz_archive *g = arc;
 if(g->tar) TarFormat.remove(g->tar, e);
}

static int gz_add(ARC *arc, const char *path, const uint8_t *buf, uint32_t len,
                  uint8_t prot, const char *comment, uint32_t seconds)
{
 struct gz_archive *g = arc;
 const char *base;
 int err;

 if(g->tar)
 {
  err = TarFormat.add(g->tar, path, buf, len, prot, comment, seconds);
  return err ? GZ_ERR_TAR + err : GZ_OK;
 }
 /* plain .gz: one file, replacing the content */
 if(g->added || (len == 0 && path[0] && path[strlen(path) - 1] == '/')) return GZ_ERR_ONE;
 free(g->newdata);
 g->newdata = NULL;
 if((err = pack(buf, len, &g->newdata, &g->newlen)) != GZ_OK) return err;
 base = strrchr(path, '/');
 base = base ? base + 1 : path;
 free(g->name);
 g->name = strndup2(base, strlen(base));
 if(comment && comment[0])
 {
  free(g->comment);
  g->comment = strndup2(comment, strlen(comment));
 }
 g->mtime = seconds;
 g->xfl = 0;
 g->added = 1;
 return GZ_OK;
}

static ARC *gz_open_current(const char *path, int create, int *err)
{
 Current = gz_open(path, create, err);
 return Current;
}

static void gz_close_current(ARC *arc)
{
 if(arc == Current) Current = NULL;
 gz_close(arc);
}

/*
** INFO: is it a tar inside? Unpack only the start of the stream.
*/
static uint32_t gz_quick_caps(const char *path, const uint8_t *head, uint32_t len, const char **type)
{
 uint32_t p = 10;
 uint8_t *plain = NULL;
 uint32_t plainlen = 0;
 int istar = 0;
 (void)path;
 if(len < 18) return 0;
 if(head[3] & FEXTRA) p += 2u + (head[10] | (head[11] << 8));
 if(head[3] & FNAME) while(p < len && head[p++]);
 if(head[3] & FCOMMENT) while(p < len && head[p++]);
 if(head[3] & FHCRC) p += 2;
 if(p < len && inflate_head(head + p, len - p, &plain, &plainlen, 512) == 0)
 {
  istar = plainlen >= 512 && TarFormat.probe(plain, plainlen) == 2;
  free(plain);
 }
 *type = istar ? "TGZ" : "GZIP";
 return istar ? ARC_CAN_ALL : (ARC_CAN_RENAME | ARC_CAN_COMMENT | ARC_CAN_ADD);
}

/*
** Writing a TGZ also needs room for the edited tar
*/
static uint64_t gz_extra_space(ARC *arc)
{
 struct gz_archive *g = arc;
 return g->tar ? plat_size(g->tarname) : 0;
}

const struct arc_format GzFormat =
{
 "GZIP",
 gz_probe,
 gz_probe_name,
 gz_open_current,
 gz_close_current,
 gz_write,
 gz_error_text,
 gz_caps,
 gz_first,
 gz_next,
 gz_path,
 gz_set_path,
 gz_set_comment,
 gz_get_protection,
 gz_set_protection,
 gz_remove,
 gz_add,
 gz_type_name,
 gz_quick_caps,
 gz_extra_space
};
