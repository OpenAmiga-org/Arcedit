/*
** File:        lha.c
** Description: LHA archive model: reading, editing and writing headers.
**
**  An LHA archive is a list of members, each a header followed by that
**  file's own compressed data, ended by a zero byte. Every member is
**  compressed on its own, so members can be removed, renamed or have their
**  header fields changed by copying the other members' bytes unchanged.
**
**  Header levels (as written by LhA 2.15 on the Amiga):
**   level 0: name field holds "path\name\0comment", path separator '\'
**   level 1: name field holds "name\0comment", path in extended header 0x02
**            (0xFF separated), header CRC in extended header 0x00 when
**            there are extended headers, OS id 'A'
**   level 2: name in ext 0x01, path in 0x02, comment in 0x71, protection in
**            0x40, header CRC in 0x00, time in seconds since 1970 (local)
**  The attribute byte holds the low byte of the Amiga protection bits.
**
** Change history:
**  2026-11-14 orgin, created
*/

#include <stdlib.h>
#include <string.h>

#include "lha.h"
#include "platform.h"

#define HDR_MAX 65536   /* largest header handled */

static uint16_t rd16(const uint8_t *p)
{
 return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
 return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint32_t v)
{
 p[0] = (uint8_t)v;
 p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
 p[0] = (uint8_t)v;
 p[1] = (uint8_t)(v >> 8);
 p[2] = (uint8_t)(v >> 16);
 p[3] = (uint8_t)(v >> 24);
}

/*
** Error text for an LHA_ERR_* code
*/
const char *lha_error_text(int err)
{
 switch(err)
 {
  case LHA_OK:         return "no error";
  case LHA_ERR_IO:     return "read/write error";
  case LHA_ERR_FORMAT: return "not an LHA archive or unsupported header";
  case LHA_ERR_MEM:    return "out of memory";
 }
 return "unknown error";
}

/*
** CRC-16 (polynomial 0xA001, as used by LHA for data and headers)
*/
uint16_t lha_crc16(uint16_t crc, const uint8_t *buf, uint32_t len)
{
 static uint16_t table[256];
 static int ready = 0;
 uint32_t i;

 if(!ready)
 {
  for(i = 0; i < 256; i++)
  {
   uint16_t c = (uint16_t)i;
   int k;
   for(k = 0; k < 8; k++)
   {
    c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
   }
   table[i] = c;
  }
  ready = 1;
 }
 for(i = 0; i < len; i++)
 {
  crc = (uint16_t)((crc >> 8) ^ table[(crc ^ buf[i]) & 0xFF]);
 }
 return crc;
}

/*
** Lower case of a Latin-1 character
*/
static int lower(int c)
{
 c &= 0xFF;
 if((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 32;
 return c;
}

/*
** Case-insensitive compare of two archive paths
*/
int lha_pathcmp(const char *a, const char *b)
{
 while(*a && lower(*a) == lower(*b))
 {
  a++;
  b++;
 }
 return lower(*a) - lower(*b);
}

/*
** Case-insensitive compare of the first n characters of two paths
*/
int lha_pathncmp(const char *a, const char *b, size_t n)
{
 while(n > 0 && *a && lower(*a) == lower(*b))
 {
  a++;
  b++;
  n--;
 }
 return n == 0 ? 0 : lower(*a) - lower(*b);
}

/*
** Seconds since 1970 (local time) to an MS-DOS date/time stamp
*/
uint32_t lha_unix_to_dos(uint32_t t)
{
 uint32_t days = t / 86400, secs = t % 86400;
 /* civil from days (Howard Hinnant's algorithm) */
 int32_t z = (int32_t)days + 719468;
 int32_t era = z / 146097;
 uint32_t doe = (uint32_t)(z - era * 146097);
 uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
 int32_t y = (int32_t)yoe + era * 400;
 uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
 uint32_t mp = (5 * doy + 2) / 153;
 uint32_t d = doy - (153 * mp + 2) / 5 + 1;
 uint32_t m = mp < 10 ? mp + 3 : mp - 9;
 if(m <= 2) y++;
 if(y < 1980) return (1 << 21) | (1 << 16);   /* 1980-01-01 */
 return ((uint32_t)(y - 1980) << 25) | (m << 21) | (d << 16) |
        ((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2);
}

/*
** MS-DOS date/time stamp to seconds since 1970 (local time)
*/
uint32_t lha_dos_to_unix(uint32_t dt)
{
 int32_t y = 1980 + (int32_t)(dt >> 25);
 uint32_t m = (dt >> 21) & 15, d = (dt >> 16) & 31;
 uint32_t h = (dt >> 11) & 31, mi = (dt >> 5) & 63, s = (dt & 31) * 2;
 int32_t era;
 uint32_t yoe, doy, doe;

 if(m < 1) m = 1;
 if(d < 1) d = 1;
 /* days from civil (Howard Hinnant's algorithm) */
 if(m <= 2) y--;
 era = y / 400;
 yoe = (uint32_t)(y - era * 400);
 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
 return (uint32_t)((era * 146097 + (int32_t)doe - 719468) * 86400) + h * 3600 + mi * 60 + s;
}

static char *strdup_n(const char *s, size_t n)
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
** TRUE when the member is a drawer entry (path ends with '/')
*/
int lha_member_is_dir(const struct lha_member *m)
{
 size_t n = strlen(m->path);
 return n > 0 && m->path[n - 1] == '/';
}

void lha_member_set_path(struct lha_member *m, const char *path)
{
 char *p = strdup_n(path, strlen(path));
 if(p)
 {
  free(m->path);
  m->path = p;
  m->modified = 1;
 }
}

void lha_member_set_comment(struct lha_member *m, const char *comment)
{
 free(m->comment);
 m->comment = (comment && comment[0]) ? strdup_n(comment, strlen(comment)) : NULL;
 m->modified = 1;
}

void lha_member_free(struct lha_member *m)
{
 while(m->extras)
 {
  struct lha_ext *e = m->extras;
  m->extras = e->next;
  free(e->data);
  free(e);
 }
 free(m->path);
 free(m->comment);
 free(m->rawhdr);
 free(m->data);
 free(m);
}

/*
** Unlink a member from the archive and free it
*/
void lha_member_remove(struct lha_archive *arc, struct lha_member *m)
{
 struct lha_member **link = &arc->first;
 while(*link && *link != m) link = &(*link)->next;
 if(*link)
 {
  *link = m->next;
  lha_member_free(m);
 }
}

/*
** TRUE if p (22 bytes) looks like the start of a header: "-lh?-" or
** "-lz?-" method at offset 2 and level 0..2 at offset 20. The format probe
** uses the same test.
*/
int lha_looks_like_header(const uint8_t *p)
{
 return p[2] == '-' && p[3] == 'l' && (p[4] == 'h' || p[4] == 'z') && p[6] == '-' && p[20] <= 2;
}

/*
** Handle one extended header (level 1/2) of member m.
** dir/name receive the path parts when present.
*/
static int take_ext(struct lha_member *m, uint8_t type, const uint8_t *data, uint32_t len,
                    char **dir, char **name)
{
 uint32_t i;
 switch(type)
 {
  case 0x00:  /* header CRC, rebuilt when writing */
   return LHA_OK;
  case 0x01:  /* file name */
   free(*name);
   *name = strdup_n((const char *)data, len);
   return *name ? LHA_OK : LHA_ERR_MEM;
  case 0x02:  /* directory, 0xFF separated */
   free(*dir);
   *dir = malloc(len + 2);
   if(*dir == NULL) return LHA_ERR_MEM;
   for(i = 0; i < len; i++) (*dir)[i] = (data[i] == 0xFF || data[i] == '\\') ? '/' : (char)data[i];
   if(len > 0 && (*dir)[len - 1] != '/') (*dir)[len++] = '/';
   (*dir)[len] = 0;
   return LHA_OK;
  case 0x3F:  /* comment */
  case 0x71:
   free(m->comment);
   m->comment = len ? strdup_n((const char *)data, len) : NULL;
   return LHA_OK;
  case 0x40:  /* attributes */
   if(len >= 1 && m->level == 2) m->attr = data[0];
   return LHA_OK;
  default:    /* kept as it is */
   {
    struct lha_ext *e = calloc(1, sizeof(*e)), **link = &m->extras;
    if(e == NULL) return LHA_ERR_MEM;
    e->type = type;
    e->len = len;
    e->data = malloc(len ? len : 1);
    if(e->data == NULL)
    {
     free(e);
     return LHA_ERR_MEM;
    }
    memcpy(e->data, data, len);
    while(*link) link = &(*link)->next;
    *link = e;
   }
   return LHA_OK;
 }
}

/*
** Read the member header at the current position of fh.
** Returns LHA_OK with *out NULL at the end of the archive.
*/
static int read_member(FILE *fh, long off, struct lha_member **out)
{
 uint8_t *h = malloc(HDR_MAX);
 struct lha_member *m = NULL;
 char *dir = NULL, *name = NULL;
 uint32_t hdrlen, pos, next, extsize = 0;
 int err = LHA_ERR_FORMAT;
 size_t got;

 *out = NULL;
 if(h == NULL) return LHA_ERR_MEM;
 if(fseek(fh, off, SEEK_SET) != 0)
 {
  free(h);
  return LHA_ERR_IO;
 }
 got = fread(h, 1, 22, fh);
 if(got == 0 || h[0] == 0)   /* end marker or end of file */
 {
  free(h);
  return LHA_OK;
 }
 if(got < 22 || !lha_looks_like_header(h)) goto fail;

 m = calloc(1, sizeof(*m));
 if(m == NULL)
 {
  err = LHA_ERR_MEM;
  goto fail;
 }
 m->level = h[20];
 memcpy(m->method, h + 2, 5);
 m->origsize = rd32(h + 11);
 m->stamp = rd32(h + 15);
 m->attr = h[19];

 if(m->level == 0 || m->level == 1)
 {
  uint32_t nl, i, sum = 0;
  hdrlen = (uint32_t)h[0] + 2;
  if(hdrlen < 24 || fread(h + 22, 1, hdrlen - 22, fh) != hdrlen - 22) goto fail;
  for(i = 2; i < hdrlen; i++) sum += h[i];
  if((sum & 0xFF) != h[1]) goto fail;
  nl = h[21];
  if(22 + nl + 2 > hdrlen) goto fail;
  /* "path\name" (level 0) or "name", then optionally "\0comment" */
  for(i = 0; i < nl && h[22 + i] != 0; i++);
  name = strdup_n((const char *)h + 22, i);
  if(i + 1 < nl) m->comment = strdup_n((const char *)h + 22 + i + 1, nl - i - 1);
  if(name == NULL) goto fail;
  for(i = 0; name[i]; i++) if(name[i] == '\\') name[i] = '/';
  m->crc = rd16(h + 22 + nl);
  if(m->level == 0)
  {
   m->os = 0;
   m->compsize = rd32(h + 7);
   pos = hdrlen;
  }
  else
  {
   if(22 + nl + 5 > hdrlen) goto fail;
   m->os = h[24 + nl];
   next = rd16(h + 25 + nl);
   pos = hdrlen;
   while(next)   /* extended headers follow the base header */
   {
    if(next < 3 || pos + next > HDR_MAX || fread(h + pos, 1, next, fh) != next) goto fail;
    if((err = take_ext(m, h[pos], h + pos + 1, next - 3, &dir, &name)) != LHA_OK) goto fail;
    err = LHA_ERR_FORMAT;
    extsize += next;
    pos += next;
    next = rd16(h + pos - 2);
   }
   if(rd32(h + 7) < extsize) goto fail;
   m->compsize = rd32(h + 7) - extsize;
  }
 }
 else if(m->level == 2)
 {
  hdrlen = rd16(h);
  if(hdrlen < 26 || fread(h + 22, 1, hdrlen - 22, fh) != hdrlen - 22) goto fail;
  m->compsize = rd32(h + 7);
  m->crc = rd16(h + 21);
  m->os = h[23];
  next = rd16(h + 24);
  pos = 26;
  while(next)
  {
   if(next < 3 || pos + next > hdrlen) goto fail;
   if((err = take_ext(m, h[pos], h + pos + 1, next - 3, &dir, &name)) != LHA_OK) goto fail;
   err = LHA_ERR_FORMAT;
   pos += next;
   next = rd16(h + pos - 2);
  }
  pos = hdrlen;
 }
 else
 {
  goto fail;   /* level 3 and others are not handled */
 }

 /* full path */
 {
  size_t dl = dir ? strlen(dir) : 0, nl2 = name ? strlen(name) : 0;
  m->path = malloc(dl + nl2 + 1);
  if(m->path == NULL)
  {
   err = LHA_ERR_MEM;
   goto fail;
  }
  if(dl) memcpy(m->path, dir, dl);
  if(nl2) memcpy(m->path + dl, name, nl2);
  m->path[dl + nl2] = 0;
 }
 m->method[5] = 0;
 m->rawhdr = malloc(pos);
 if(m->rawhdr == NULL)
 {
  err = LHA_ERR_MEM;
  goto fail;
 }
 memcpy(m->rawhdr, h, pos);
 m->rawhdrlen = pos;
 m->dataoff = off + (long)pos;
 free(dir);
 free(name);
 free(h);
 *out = m;
 return LHA_OK;

fail:
 free(dir);
 free(name);
 free(h);
 if(m) lha_member_free(m);
 return err;
}

/*
** Open and read an archive (see lha.h)
*/
struct lha_archive *lha_open(const char *name, int create, int *err)
{
 struct lha_archive *arc = calloc(1, sizeof(*arc));
 struct lha_member **link;
 uint8_t *head;          /* not on the stack: AmigaOS commands have small stacks */
 long off = 0;
 size_t got;

 *err = LHA_ERR_MEM;
 if(arc == NULL) return NULL;
 arc->deflevel = 1;   /* LhA's default */
 arc->fh = fopen(name, "rb");
 if(arc->fh == NULL)
 {
  if(create)
  {
   *err = LHA_OK;
   return arc;
  }
  *err = LHA_ERR_IO;
  free(arc);
  return NULL;
 }

 /* the first header, after the code of a self-extracting archive */
 head = malloc(64 * 1024);
 if(head == NULL)
 {
  lha_close(arc);
  return NULL;
 }
 got = fread(head, 1, 64 * 1024, arc->fh);
 if(got >= 22 && !lha_looks_like_header(head))
 {
  /* code in front of the first header: only a self-extracting archive
     (an Amiga or MS-DOS executable), else the first header is damaged */
  if(!(head[0] == 0 && head[1] == 0 && head[2] == 3 && head[3] == 0xF3) && !(head[0] == 'M' && head[1] == 'Z'))
  {
   *err = LHA_ERR_FORMAT;
   free(head);
   lha_close(arc);
   return NULL;
  }
  for(off = 1; off + 22 <= (long)got; off++)
  {
   if(lha_looks_like_header(head + off)) break;
  }
  if(off + 22 > (long)got)
  {
   *err = LHA_ERR_FORMAT;
   free(head);
   lha_close(arc);
   return NULL;
  }
  arc->prefix = malloc((size_t)off);
  if(arc->prefix == NULL)
  {
   free(head);
   lha_close(arc);
   return NULL;
  }
  memcpy(arc->prefix, head, (size_t)off);
  arc->prefixlen = (uint32_t)off;
 }
 free(head);

 link = &arc->first;
 for(;;)
 {
  struct lha_member *m;
  int e = read_member(arc->fh, off, &m);
  if(e != LHA_OK)
  {
   *err = e;
   lha_close(arc);
   return NULL;
  }
  if(m == NULL) break;
  if(arc->first == NULL) arc->deflevel = m->level ? m->level : 1;
  *link = m;
  link = &m->next;
  off = m->dataoff + (long)m->compsize;
 }
 *err = LHA_OK;
 return arc;
}

void lha_close(struct lha_archive *arc)
{
 if(arc == NULL) return;
 while(arc->first)
 {
  struct lha_member *m = arc->first;
  arc->first = m->next;
  lha_member_free(m);
 }
 if(arc->fh) fclose(arc->fh);
 free(arc->prefix);
 free(arc);
}

/*
** Append one extended header (type + data) to a header being built at
** buf+*pos; *prevnext points to the "next size" field to fill in.
*/
static void put_ext(uint8_t *buf, uint32_t *pos, uint8_t **prevnext, uint8_t type,
                    const uint8_t *data, uint32_t len)
{
 wr16(*prevnext, len + 3);
 buf[(*pos)++] = type;
 if(len) memcpy(buf + *pos, data, len);
 *pos += len;
 *prevnext = buf + *pos;
 wr16(*prevnext, 0);
 *pos += 2;
}

/*
** Split a member path into its directory part (with the trailing '/') and
** its name: "sub/b.txt" gives "sub/" and "b.txt", "empty/" gives "empty/"
** and "".
*/
static void split_path(const char *path, size_t *dirlen, const char **name)
{
 size_t n = strlen(path), i;
 if(n > 0 && path[n - 1] == '/')
 {
  *dirlen = n;
  *name = path + n;
  return;
 }
 for(i = n; i > 0 && path[i - 1] != '/'; i--);
 *dirlen = i;
 *name = path + i;
}

/*
** Build the header of a modified member into buf (HDR_MAX bytes).
** The level may be raised to 2 when the names don't fit.
** Returns the header length.
*/
static uint32_t build_header(struct lha_member *m, uint8_t *buf)
{
 size_t dirlen, namelen, commentlen = m->comment ? strlen(m->comment) : 0;
 const char *name;
 uint32_t pos, i;
 uint8_t *prevnext = NULL, *crcfield = NULL;
 struct lha_ext *e;

 split_path(m->path, &dirlen, &name);
 namelen = strlen(name);

 /* names that don't fit a level 0/1 name field go to level 2, whose time
    stamp is in seconds instead of MS-DOS format */
 {
  int oldlevel = m->level;
  if(m->level == 0 && dirlen + namelen + (commentlen ? commentlen + 1 : 0) > 230) m->level = 2;
  if(m->level == 1 && namelen + (commentlen ? commentlen + 1 : 0) > 230) m->level = 2;
  if(oldlevel != 2 && m->level == 2) m->stamp = lha_dos_to_unix(m->stamp);
 }
 if(m->os == 0 && m->level != 0) m->os = 'A';

 memset(buf, 0, 26);
 memcpy(buf + 2, m->method, 5);
 wr32(buf + 11, m->origsize);
 wr32(buf + 15, m->stamp);
 buf[19] = m->attr;
 buf[20] = (uint8_t)m->level;

 if(m->level == 0 || m->level == 1)
 {
  uint32_t nl = 0, sum = 0, base;
  uint8_t *field = buf + 22;
  if(m->level == 0)
  {
   for(i = 0; i < dirlen + namelen; i++)
   {
    char c = m->path[i];
    field[nl++] = (uint8_t)(c == '/' ? '\\' : c);
   }
  }
  else
  {
   memcpy(field, name, namelen);
   nl = (uint32_t)namelen;
  }
  if(commentlen)
  {
   field[nl++] = 0;
   memcpy(field + nl, m->comment, commentlen);
   nl += (uint32_t)commentlen;
  }
  buf[21] = (uint8_t)nl;
  pos = 22 + nl;
  wr16(buf + pos, m->crc);
  pos += 2;
  if(m->level == 1)
  {
   buf[pos++] = m->os;
   prevnext = buf + pos;
   wr16(prevnext, 0);
   pos += 2;
  }
  base = pos;
  buf[0] = (uint8_t)(base - 2);

  if(m->level == 1)
  {
   if(dirlen > 0)
   {
    uint8_t d[1024];
    uint32_t dl = (uint32_t)(dirlen > sizeof(d) ? sizeof(d) : dirlen);
    for(i = 0; i < dl; i++) d[i] = (uint8_t)(m->path[i] == '/' ? 0xFF : m->path[i]);
    put_ext(buf, &pos, &prevnext, 0x02, d, dl);
   }
   for(e = m->extras; e; e = e->next) put_ext(buf, &pos, &prevnext, e->type, e->data, e->len);
   if(pos > base)   /* LhA adds the header CRC when there are extended headers */
   {
    uint8_t zero[2] = { 0, 0 };
    put_ext(buf, &pos, &prevnext, 0x00, zero, 2);
    crcfield = buf + pos - 4;
   }
   wr32(buf + 7, m->compsize + (pos - base));
  }
  else
  {
   wr32(buf + 7, m->compsize);
  }

  for(i = 2; i < base; i++) sum += buf[i];
  buf[1] = (uint8_t)sum;
  if(crcfield) wr16(crcfield, lha_crc16(0, buf, pos));
  return pos;
 }

 /* level 2 */
 wr32(buf + 7, m->compsize);
 wr16(buf + 21, m->crc);
 buf[23] = m->os;
 prevnext = buf + 24;
 wr16(prevnext, 0);
 pos = 26;
 put_ext(buf, &pos, &prevnext, 0x01, (const uint8_t *)name, (uint32_t)namelen);
 if(dirlen > 0)
 {
  uint8_t d[1024];
  uint32_t dl = (uint32_t)(dirlen > sizeof(d) ? sizeof(d) : dirlen);
  for(i = 0; i < dl; i++) d[i] = (uint8_t)(m->path[i] == '/' ? 0xFF : m->path[i]);
  put_ext(buf, &pos, &prevnext, 0x02, d, dl);
 }
 if(commentlen) put_ext(buf, &pos, &prevnext, 0x71, (const uint8_t *)m->comment, (uint32_t)commentlen);
 {
  uint8_t a[2];
  a[0] = m->attr;
  a[1] = 0;
  put_ext(buf, &pos, &prevnext, 0x40, a, 2);
 }
 for(e = m->extras; e; e = e->next) put_ext(buf, &pos, &prevnext, e->type, e->data, e->len);
 {
  uint8_t zero[2] = { 0, 0 };
  put_ext(buf, &pos, &prevnext, 0x00, zero, 2);
  crcfield = buf + pos - 4;
 }
 if((pos & 0xFF) == 0) buf[pos++] = 0;   /* the size's low byte must not be 0 (end marker) */
 wr16(buf, pos);
 wr16(crcfield, lha_crc16(0, buf, pos));
 return pos;
}

/*
** Write the archive to name (see lha.h)
*/
int lha_write(struct lha_archive *arc, const char *name)
{
 FILE *out = fopen(name, "wb");
 uint8_t *buf = malloc(HDR_MAX);
 uint8_t *copybuf = malloc(64 * 1024);
 struct lha_member *m;
 int err = LHA_OK;

 if(out == NULL || buf == NULL || copybuf == NULL)
 {
  err = out ? LHA_ERR_MEM : LHA_ERR_IO;
  goto done;
 }
 if(arc->prefixlen && fwrite(arc->prefix, 1, arc->prefixlen, out) != arc->prefixlen) err = LHA_ERR_IO;

 for(m = arc->first; m && err == LHA_OK; m = m->next)
 {
  if(m->modified || m->rawhdr == NULL)
  {
   uint32_t len = build_header(m, buf);
   if(fwrite(buf, 1, len, out) != len) err = LHA_ERR_IO;
  }
  else if(fwrite(m->rawhdr, 1, m->rawhdrlen, out) != m->rawhdrlen)
  {
   err = LHA_ERR_IO;
  }
  if(err != LHA_OK) break;

  if(m->data)
  {
   if(m->compsize && fwrite(m->data, 1, m->compsize, out) != m->compsize) err = LHA_ERR_IO;
  }
  else if(m->compsize)
  {
   uint32_t left = m->compsize;
   if(arc->fh == NULL || fseek(arc->fh, m->dataoff, SEEK_SET) != 0) err = LHA_ERR_IO;
   while(left && err == LHA_OK)
   {
    size_t chunk = left > 64 * 1024 ? 64 * 1024 : left;
    if(plat_break() || fread(copybuf, 1, chunk, arc->fh) != chunk || fwrite(copybuf, 1, chunk, out) != chunk) err = LHA_ERR_IO;
    left -= (uint32_t)chunk;
   }
  }
 }
 if(err == LHA_OK && fputc(0, out) == EOF) err = LHA_ERR_IO;

done:
 if(out && fclose(out) != 0 && err == LHA_OK) err = LHA_ERR_IO;
 free(buf);
 free(copybuf);
 return err;
}

/*
** Add a member from memory (see lha.h)
*/
int lha_add(struct lha_archive *arc, const char *path, const uint8_t *buf, uint32_t len,
            uint8_t attr, const char *comment, uint32_t seconds)
{
 struct lha_member *m = calloc(1, sizeof(*m)), **link;
 uint32_t clen = 0;

 if(m == NULL) return LHA_ERR_MEM;
 m->level = arc->deflevel;
 m->os = 'A';
 m->attr = attr;
 m->origsize = len;
 m->crc = len ? lha_crc16(0, buf, len) : 0;
 m->stamp = m->level == 2 ? seconds : lha_unix_to_dos(seconds);
 m->path = strdup_n(path, strlen(path));
 m->comment = (comment && comment[0]) ? strdup_n(comment, strlen(comment)) : NULL;
 m->modified = 1;
 m->data = malloc(len ? len : 1);
 if(m->path == NULL || m->data == NULL)
 {
  lha_member_free(m);
  return LHA_ERR_MEM;
 }
 if(len) clen = lh5_compress(buf, len, m->data);
 if(clen)
 {
  memcpy(m->method, "-lh5-", 6);
  m->compsize = clen;
 }
 else
 {
  memcpy(m->method, "-lh0-", 6);
  if(len) memcpy(m->data, buf, len);
  m->compsize = len;
 }

 /* replace a member with the same path, else append */
 for(link = &arc->first; *link; link = &(*link)->next)
 {
  if(lha_pathcmp((*link)->path, path) == 0)
  {
   struct lha_member *old = *link;
   m->next = old->next;
   *link = m;
   lha_member_free(old);
   return LHA_OK;
  }
 }
 *link = m;
 return LHA_OK;
}
