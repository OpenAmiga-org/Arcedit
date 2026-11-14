/*
** File:        tarformat.c
** Description: The TAR backend of arcedit (ustar, with GNU long names and
**              pax extended headers).
**
**  A tar archive is a list of 512 byte header blocks, each followed by its
**  data padded to 512 bytes, ended by two zero blocks. An entry may be
**  preceded by a pax extended header ('x', records "len key=value\n") or
**  a GNU long name ('L'). Unchanged entries are copied with their
**  extension headers as they are. Changed entries get a new header:
**  names that fit ustar's name/prefix fields go there, longer ones into a
**  pax "path" record; the file note is a pax "comment" record.
**
**  Protection: tar has Unix modes, so PROTECT sets the owner's r w x from
**  the Amiga r w e (d follows w when read); h s p a are not stored.
**
**  The backend reads and writes files, so the gzip backend (TGZ) can use
**  it on an unpacked copy.
**
** Change history:
**  2026-11-14 orgin, created
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcformat.h"
#include "tar.h"
#include "lha.h"      /* lha_pathcmp */

#define BLOCK 512

#define TAR_OK         0
#define TAR_ERR_IO     1
#define TAR_ERR_FORMAT 2
#define TAR_ERR_MEM    3

struct tar_entry
{
 struct tar_entry *next;
 int isentry;              /* 0 for kept units that aren't entries (pax global headers) */

 uint8_t hdr[BLOCK];       /* the main header as read (or built) */
 char *path;               /* full path; drawers end with '/' */
 char *comment;
 char *paxother;           /* pax records other than path/comment, as text */
 uint32_t paxotherlen;
 uint32_t mode;
 uint64_t size;            /* data bytes (from the main header) */

 long rawoff;              /* first byte of the unit (extension headers) */
 uint64_t rawlen;          /* whole unit: extension headers, header, padded data */
 long dataoff;             /* data in the source */
 int modified;

 uint8_t *data;            /* added entries: the data */
};

struct tar_archive
{
 FILE *fh;
 struct tar_entry *first;
};

/*
** The last part of an archive path
*/
static const char *name_part(const char *path)
{
 const char *p = strrchr(path, '/');
 if(p && p[1] == 0)
 {
  /* "dir/": the drawer's own name */
  while(p > path && p[-1] != '/') p--;
  return p;
 }
 return p ? p + 1 : path;
}

static const char *tar_error_text(int err)
{
 switch(err)
 {
  case TAR_OK:         return "no error";
  case TAR_ERR_IO:     return "read/write error";
  case TAR_ERR_FORMAT: return "not a tar archive or damaged";
  case TAR_ERR_MEM:    return "out of memory";
 }
 return "unknown error";
}

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

/*
** Octal number field (or GNU base-256 for large values)
*/
static uint64_t getnum(const uint8_t *p, int len)
{
 uint64_t v = 0;
 int i;
 if(p[0] & 0x80)
 {
  v = p[0] & 0x7F;
  for(i = 1; i < len; i++) v = (v << 8) | p[i];
  return v;
 }
 for(i = 0; i < len && (p[i] == ' ' || p[i] == 0); i++);
 for(; i < len && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (uint64_t)(p[i] - '0');
 return v;
}

static void putnum(uint8_t *p, int len, uint64_t v)
{
 int i;
 p[len - 1] = 0;
 for(i = len - 2; i >= 0; i--)
 {
  p[i] = (uint8_t)('0' + (v & 7));
  v >>= 3;
 }
}

static uint32_t checksum(const uint8_t *h)
{
 uint32_t sum = 0;
 int i;
 for(i = 0; i < BLOCK; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
 return sum;
}

static int zero_block(const uint8_t *h)
{
 int i;
 for(i = 0; i < BLOCK; i++) if(h[i]) return 0;
 return 1;
}

static uint64_t padded(uint64_t n)
{
 return (n + BLOCK - 1) / BLOCK * BLOCK;
}

static void entry_free(struct tar_entry *e)
{
 free(e->path);
 free(e->comment);
 free(e->paxother);
 free(e->data);
 free(e);
}

void tar_close_file(ARC *arc)
{
 struct tar_archive *t = arc;
 if(t == NULL) return;
 while(t->first)
 {
  struct tar_entry *e = t->first;
  t->first = e->next;
  entry_free(e);
 }
 if(t->fh) fclose(t->fh);
 free(t);
}

static int tar_probe(const uint8_t *head, uint32_t len)
{
 if(len < BLOCK) return 0;
 if(memcmp(head + 257, "ustar", 5) == 0 && getnum(head + 148, 8) == checksum(head)) return 2;
 /* old (v7) archives: only the checksum tells */
 if(head[0] && getnum(head + 148, 8) == checksum(head) && head[148] != 0) return 1;
 return 0;
}

static int tar_probe_name(const char *path)
{
 size_t n = strlen(path);
 return n > 4 && lha_pathcmp(path + n - 4, ".tar") == 0;
}

/*
** Parse pax records: path and comment are taken, the others kept as text
*/
static int parse_pax(struct tar_entry *e, const char *buf, uint64_t len, char **path)
{
 uint64_t pos = 0;
 while(pos < len)
 {
  uint64_t rl = 0, p = pos;
  const char *key, *eq, *val;
  while(p < len && buf[p] >= '0' && buf[p] <= '9') rl = rl * 10 + (uint64_t)(buf[p++] - '0');
  if(rl == 0 || pos + rl > len || p >= len || buf[p] != ' ') return TAR_ERR_FORMAT;
  key = buf + p + 1;
  eq = memchr(key, '=', (size_t)(buf + pos + rl - key));
  if(eq == NULL) return TAR_ERR_FORMAT;
  val = eq + 1;
  {
   size_t vl = (size_t)(buf + pos + rl - 1 - val);   /* without the "\n" */
   size_t kl = (size_t)(eq - key);
   if(kl == 4 && memcmp(key, "path", 4) == 0)
   {
    free(*path);
    *path = strndup2(val, vl);
   }
   else if(kl == 7 && memcmp(key, "comment", 7) == 0)
   {
    free(e->comment);
    e->comment = vl ? strndup2(val, vl) : NULL;
   }
   else
   {
    char *n = realloc(e->paxother, e->paxotherlen + (uint32_t)rl + 1);
    if(n == NULL) return TAR_ERR_MEM;
    memcpy(n + e->paxotherlen, buf + pos, (size_t)rl);
    e->paxotherlen += (uint32_t)rl;
    n[e->paxotherlen] = 0;
    e->paxother = n;
   }
  }
  pos += rl;
 }
 return TAR_OK;
}

ARC *tar_open_file(const char *path, int create, int *err)
{
 struct tar_archive *t = calloc(1, sizeof(*t));
 struct tar_entry **link;
 uint8_t h[BLOCK];
 long off = 0;

 *err = TAR_ERR_MEM;
 if(t == NULL) return NULL;
 t->fh = fopen(path, "rb");
 if(t->fh == NULL)
 {
  if(create)
  {
   *err = TAR_OK;
   return t;
  }
  *err = TAR_ERR_IO;
  free(t);
  return NULL;
 }
 link = &t->first;
 for(;;)
 {
  struct tar_entry *e;
  char *paxpath = NULL, *longname = NULL;
  long unitoff = off;
  int ok = 1;

  if(fseek(t->fh, off, SEEK_SET) != 0 || fread(h, 1, BLOCK, t->fh) != BLOCK || zero_block(h)) break;
  e = calloc(1, sizeof(*e));
  if(e == NULL) goto fail;
  e->rawoff = unitoff;

  /* extension headers before the entry */
  while(ok)
  {
   uint64_t size;
   char *buf;
   if(getnum(h + 148, 8) != checksum(h))
   {
    entry_free(e);
    *err = TAR_ERR_FORMAT;
    goto fail;
   }
   if(h[156] != 'x' && h[156] != 'L' && h[156] != 'K') break;
   size = getnum(h + 124, 12);
   buf = malloc((size_t)size + 1);
   if(buf == NULL || fread(buf, 1, (size_t)size, t->fh) != size)
   {
    free(buf);
    entry_free(e);
    *err = buf ? TAR_ERR_IO : TAR_ERR_MEM;
    goto fail;
   }
   buf[size] = 0;
   if(h[156] == 'x')
   {
    if(parse_pax(e, buf, size, &paxpath) != TAR_OK) ok = 0;
   }
   else if(h[156] == 'L')
   {
    free(longname);
    longname = strndup2(buf, strlen(buf));
   }
   free(buf);
   off += BLOCK + (long)padded(size);
   if(fseek(t->fh, off, SEEK_SET) != 0 || fread(h, 1, BLOCK, t->fh) != BLOCK) ok = 0;
  }
  if(!ok)
  {
   free(paxpath);
   free(longname);
   entry_free(e);
   *err = TAR_ERR_FORMAT;
   goto fail;
  }

  memcpy(e->hdr, h, BLOCK);
  e->isentry = h[156] != 'g';
  e->mode = (uint32_t)getnum(h + 100, 8);
  e->size = getnum(h + 124, 12);
  e->dataoff = off + BLOCK;
  /* link types and directories have no data */
  if(h[156] == '1' || h[156] == '2' || h[156] == '5') e->size = (h[156] == '5') ? 0 : e->size;
  off += BLOCK + (long)padded((h[156] == '1' || h[156] == '2') ? 0 : e->size);
  e->rawlen = (uint64_t)(off - unitoff);

  if(paxpath) e->path = paxpath;
  else if(longname) e->path = longname;
  else
  {
   char name[101], prefix[156], full[260];
   memcpy(name, h, 100);
   name[100] = 0;
   memcpy(prefix, h + 345, 155);
   prefix[155] = 0;
   if(memcmp(h + 257, "ustar\0", 6) == 0 && prefix[0]) snprintf(full, sizeof(full), "%s/%s", prefix, name);
   else snprintf(full, sizeof(full), "%s", name);
   e->path = strndup2(full, strlen(full));
  }
  if(paxpath && longname) free(longname);
  if(e->path == NULL)
  {
   entry_free(e);
   goto fail;
  }
  /* "./x" names: the "./" isn't part of the name for matching */
  while(e->path[0] == '.' && e->path[1] == '/') memmove(e->path, e->path + 2, strlen(e->path + 2) + 1);
  if(h[156] == '5')
  {
   size_t n = strlen(e->path);
   if(n == 0 || e->path[n - 1] != '/')
   {
    char *p = realloc(e->path, n + 2);
    if(p == NULL)
    {
     entry_free(e);
     goto fail;
    }
    p[n] = '/';
    p[n + 1] = 0;
    e->path = p;
   }
  }
  *link = e;
  link = &e->next;
 }
 *err = TAR_OK;
 return t;

fail:
 tar_close_file(t);
 return NULL;
}

/*
** Append one pax record "len key=value\n" to buf
*/
static uint32_t pax_record(char *buf, const char *key, const char *val)
{
 size_t kl = strlen(key), vl = strlen(val), base = kl + vl + 3, len = base + 1, digits;
 /* the length counts its own digits */
 for(;;)
 {
  char tmp[24];
  digits = (size_t)snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)len);
  if(base + digits == len) break;
  len = base + digits;
 }
 if(buf) sprintf(buf, "%lu %s=%s\n", (unsigned long)len, key, val);
 return (uint32_t)len;
}

/*
** Build the headers of a changed or added entry into out (a pax header
** first when needed). Returns the bytes written, 0 when out of memory.
*/
static uint64_t build_headers(struct tar_entry *e, uint8_t **out)
{
 uint8_t *h = e->hdr;
 const char *path = e->path;
 size_t n = strlen(path);
 int fits = 0;
 char *pax = NULL;
 uint32_t paxlen = 0;
 uint64_t total;

 /* name, or prefix + "/" + name */
 memset(h, 0, 100);
 memset(h + 345, 0, 155);
 if(n <= 100)
 {
  memcpy(h, path, n);
  fits = 1;
 }
 else
 {
  size_t i;
  for(i = n; i > 0; i--)
  {
   if(path[i - 1] == '/' && i - 1 <= 155 && n - i <= 100 && n - i > 0)
   {
    memcpy(h + 345, path, i - 1);
    memcpy(h, path + i, n - i);
    fits = 1;
    break;
   }
  }
  if(!fits) memcpy(h, path, 100);   /* cut; the pax path has the real one */
 }
 memcpy(h + 257, "ustar\0" "00", 8);
 putnum(h + 100, 8, e->mode & 07777);

 /* pax header: long path, comment and records that were there */
 paxlen = e->paxotherlen;
 if(!fits) paxlen += pax_record(NULL, "path", path);
 if(e->comment) paxlen += pax_record(NULL, "comment", e->comment);
 if(paxlen)
 {
  pax = malloc(paxlen + 64);
  if(pax == NULL) return 0;
  paxlen = 0;
  if(e->paxotherlen)
  {
   memcpy(pax, e->paxother, e->paxotherlen);
   paxlen = e->paxotherlen;
  }
  if(!fits) paxlen += pax_record(pax + paxlen, "path", path);
  if(e->comment) paxlen += pax_record(pax + paxlen, "comment", e->comment);
 }

 memset(h + 148, ' ', 8);
 putnum(h + 148, 7, checksum(h));
 h[155] = ' ';

 total = (paxlen ? BLOCK + padded(paxlen) : 0) + BLOCK;
 *out = calloc(1, (size_t)total);
 if(*out == NULL)
 {
  free(pax);
  return 0;
 }
 if(paxlen)
 {
  uint8_t *x = *out;
  char name[100];
  snprintf(name, sizeof(name), "PaxHeaders/%.80s", name_part(path));
  memcpy(x, name, strlen(name));
  putnum(x + 100, 8, 0644);
  putnum(x + 108, 8, 0);
  putnum(x + 116, 8, 0);
  putnum(x + 124, 12, paxlen);
  memcpy(x + 136, h + 136, 12);
  x[156] = 'x';
  memcpy(x + 257, "ustar\0" "00", 8);
  memset(x + 148, ' ', 8);
  putnum(x + 148, 7, checksum(x));
  x[155] = ' ';
  memcpy(x + BLOCK, pax, paxlen);
 }
 memcpy(*out + total - BLOCK, h, BLOCK);
 free(pax);
 return total;
}

int tar_write_file(ARC *arc, const char *path)
{
 struct tar_archive *t = arc;
 struct tar_entry *e;
 FILE *out = fopen(path, "wb");
 uint8_t *copy = malloc(64 * 1024);
 uint64_t pos = 0;
 int err = TAR_OK;

 if(out == NULL || copy == NULL)
 {
  err = out ? TAR_ERR_MEM : TAR_ERR_IO;
  goto done;
 }
 for(e = t->first; e && err == TAR_OK; e = e->next)
 {
  uint64_t left, datalen = padded((e->hdr[156] == '1' || e->hdr[156] == '2') ? 0 : e->size);
  if(e->modified || e->data)
  {
   uint8_t *hb;
   uint64_t hl = build_headers(e, &hb);
   if(hl == 0)
   {
    err = TAR_ERR_MEM;
    break;
   }
   if(fwrite(hb, 1, (size_t)hl, out) != hl) err = TAR_ERR_IO;
   free(hb);
   pos += hl;
   if(e->data)
   {
    static const uint8_t zeros[BLOCK];
    if(e->size && fwrite(e->data, 1, (size_t)e->size, out) != e->size) err = TAR_ERR_IO;
    if(datalen > e->size && fwrite(zeros, 1, (size_t)(datalen - e->size), out) != datalen - e->size) err = TAR_ERR_IO;
    pos += datalen;
    continue;
   }
   left = datalen;
   if(fseek(t->fh, e->dataoff, SEEK_SET) != 0) err = TAR_ERR_IO;
  }
  else
  {
   left = e->rawlen;
   if(fseek(t->fh, e->rawoff, SEEK_SET) != 0) err = TAR_ERR_IO;
  }
  while(left && err == TAR_OK)
  {
   size_t n = left > 64 * 1024 ? 64 * 1024 : (size_t)left;
   if(fread(copy, 1, n, t->fh) != n || fwrite(copy, 1, n, out) != n) err = TAR_ERR_IO;
   left -= n;
   pos += n;
  }
 }
 /* two zero blocks, then up to a full 10 KB record as tar does */
 if(err == TAR_OK)
 {
  uint64_t end = pos + 2 * BLOCK;
  end = (end + 10239) / 10240 * 10240;
  memset(copy, 0, 64 * 1024);
  while(pos < end && err == TAR_OK)
  {
   size_t n = end - pos > 64 * 1024 ? 64 * 1024 : (size_t)(end - pos);
   if(fwrite(copy, 1, n, out) != n) err = TAR_ERR_IO;
   pos += n;
  }
 }
done:
 if(out && fclose(out) != 0 && err == TAR_OK) err = TAR_ERR_IO;
 free(copy);
 return err;
}

static uint32_t tar_caps(ARC *arc)
{
 (void)arc;
 return ARC_CAN_ALL;
}

/* Kept units that aren't entries (pax global headers) are skipped */
static ENTRY *tar_first(ARC *arc)
{
 struct tar_entry *e = ((struct tar_archive *)arc)->first;
 while(e && !e->isentry) e = e->next;
 return e;
}

static ENTRY *tar_next(ARC *arc, ENTRY *en)
{
 struct tar_entry *e = ((struct tar_entry *)en)->next;
 (void)arc;
 while(e && !e->isentry) e = e->next;
 return e;
}

static const char *tar_path(ENTRY *e)
{
 return ((struct tar_entry *)e)->path;
}

static void tar_set_path(ENTRY *en, const char *path)
{
 struct tar_entry *e = en;
 char *p = strndup2(path, strlen(path));
 if(p)
 {
  free(e->path);
  e->path = p;
  e->modified = 1;
 }
}

static void tar_set_comment(ENTRY *en, const char *comment)
{
 struct tar_entry *e = en;
 free(e->comment);
 e->comment = (comment && comment[0]) ? strndup2(comment, strlen(comment)) : NULL;
 e->modified = 1;
}

/*
** Amiga protection from the owner's mode bits (d follows w)
*/
static uint8_t tar_get_protection(ENTRY *en)
{
 struct tar_entry *e = en;
 uint8_t p = 0;
 if(!(e->mode & 0400)) p |= ARC_PROT_R;
 if(!(e->mode & 0200)) p |= ARC_PROT_W | ARC_PROT_D;
 if(!(e->mode & 0100) && e->hdr[156] != '5') p |= ARC_PROT_E;
 return p;
}

static void tar_set_protection(ENTRY *en, uint8_t prot)
{
 struct tar_entry *e = en;
 uint32_t mode = e->mode & ~0700u;
 if(!(prot & ARC_PROT_R)) mode |= 0400;
 if(!(prot & ARC_PROT_W)) mode |= 0200;
 if(!(prot & ARC_PROT_E) || e->hdr[156] == '5') mode |= 0100;
 if(mode != e->mode)
 {
  e->mode = mode;
  e->modified = 1;
 }
}

static void tar_remove(ARC *arc, ENTRY *en)
{
 struct tar_archive *t = arc;
 struct tar_entry **link = &t->first;
 while(*link && *link != en) link = &(*link)->next;
 if(*link)
 {
  *link = ((struct tar_entry *)en)->next;
  entry_free(en);
 }
}

static int tar_add(ARC *arc, const char *path, const uint8_t *buf, uint32_t len,
            uint8_t prot, const char *comment, uint32_t seconds)
{
 struct tar_archive *t = arc;
 struct tar_entry *e = calloc(1, sizeof(*e)), **link;
 int isdir = len == 0 && path[0] && path[strlen(path) - 1] == '/';

 if(e == NULL) return TAR_ERR_MEM;
 e->isentry = 1;
 e->path = strndup2(path, strlen(path));
 e->comment = (comment && comment[0]) ? strndup2(comment, strlen(comment)) : NULL;
 e->data = malloc(len ? len : 1);
 if(e->path == NULL || e->data == NULL)
 {
  entry_free(e);
  return TAR_ERR_MEM;
 }
 if(len) memcpy(e->data, buf, len);
 e->size = len;
 e->mode = isdir ? 0755 : 0644;
 tar_set_protection(e, prot);
 e->modified = 1;
 memset(e->hdr, 0, BLOCK);
 putnum(e->hdr + 108, 8, 0);
 putnum(e->hdr + 116, 8, 0);
 putnum(e->hdr + 124, 12, len);
 putnum(e->hdr + 136, 12, seconds);
 e->hdr[156] = isdir ? '5' : '0';

 for(link = &t->first; *link; link = &(*link)->next)
 {
  if((*link)->isentry && lha_pathcmp((*link)->path, path) == 0)
  {
   struct tar_entry *old = *link;
   e->next = old->next;
   *link = e;
   entry_free(old);
   return TAR_OK;
  }
 }
 *link = e;
 return TAR_OK;
}

const struct arc_format TarFormat =
{
 "TAR",
 tar_probe,
 tar_probe_name,
 tar_open_file,
 tar_close_file,
 tar_write_file,
 tar_error_text,
 tar_caps,
 tar_first,
 tar_next,
 tar_path,
 tar_set_path,
 tar_set_comment,
 tar_get_protection,
 tar_set_protection,
 tar_remove,
 tar_add
};
