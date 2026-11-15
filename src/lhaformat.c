/*
** File:        lhaformat.c
** Description: The LHA (.lha, .lzh) backend of arcedit: the arc_format
**              calls mapped to the archive model in lha.c.
**
** Change history:
**  2026-11-14 orgin, created
*/

#include <stdlib.h>
#include <string.h>

#include "arcformat.h"
#include "lha.h"

static int lhaf_probe(const uint8_t *head, uint32_t len)
{
 uint32_t off;
 /* an empty archive: just the end byte */
 if(len == 1 && head[0] == 0) return 1;
 /* code in front of the first header only in self-extracting archives */
 if(len >= 22 && !lha_looks_like_header(head) &&
    !(head[0] == 0 && head[1] == 0 && head[2] == 3 && head[3] == 0xF3) && !(head[0] == 'M' && head[1] == 'Z')) return 0;
 /* a header at the start, or after the code of a self-extracting archive */
 for(off = 0; off + 22 <= len; off++)
 {
  const uint8_t *p = head + off;
  if(lha_looks_like_header(p)) return off == 0 ? 2 : 1;
 }
 return 0;
}

static int lhaf_probe_name(const char *path)
{
 size_t n = strlen(path);
 return n > 4 && (lha_pathcmp(path + n - 4, ".lha") == 0 || lha_pathcmp(path + n - 4, ".lzh") == 0);
}

static ARC *lhaf_open(const char *path, int create, int *err)
{
 return lha_open(path, create, err);
}

static void lhaf_close(ARC *arc)
{
 lha_close(arc);
}

static int lhaf_write(ARC *arc, const char *path)
{
 return lha_write(arc, path);
}

static uint32_t lhaf_caps(ARC *arc)
{
 (void)arc;
 return ARC_CAN_ALL;
}

static ENTRY *lhaf_first(ARC *arc)
{
 return ((struct lha_archive *)arc)->first;
}

static ENTRY *lhaf_next(ARC *arc, ENTRY *e)
{
 (void)arc;
 return ((struct lha_member *)e)->next;
}

static const char *lhaf_path(ENTRY *e)
{
 return ((struct lha_member *)e)->path;
}

static void lhaf_set_path(ENTRY *e, const char *path)
{
 lha_member_set_path(e, path);
}

static void lhaf_set_comment(ENTRY *e, const char *comment)
{
 lha_member_set_comment(e, comment);
}

/*
** The attribute byte holds Amiga protection bits only for entries made on
** the Amiga (OS 'A', or level 0 which has no OS field). Other systems put
** MS-DOS attributes there, which LhA ignores (it shows "----rwed").
*/
static int amiga_attr(const struct lha_member *m)
{
 return m->level == 0 || m->os == 'A';
}

static uint8_t lhaf_get_protection(ENTRY *e)
{
 struct lha_member *m = e;
 return amiga_attr(m) ? m->attr : 0;   /* 0: rwed allowed, nothing else set */
}

/*
** Remove the Unix permission header (0x50): readers such as xadmaster take
** the protection from it whenever it is there, so after a PROTECT it would
** hide the new Amiga bits. Owner, group and times (0x51, 0x54) stay.
*/
static int drop_unix_mode(struct lha_member *m)
{
 struct lha_ext **link = &m->extras;
 int dropped = 0;
 while(*link)
 {
  if((*link)->type == 0x50)
  {
   struct lha_ext *e = *link;
   *link = e->next;
   free(e->data);
   free(e);
   dropped = 1;
  }
  else
  {
   link = &(*link)->next;
  }
 }
 return dropped;
}

static void lhaf_set_protection(ENTRY *e, uint8_t prot)
{
 struct lha_member *m = e;
 if(drop_unix_mode(m))
 {
  m->modified = 1;
 }
 if(!amiga_attr(m))
 {
  /* an Amiga entry from now on, so LhA uses the new bits */
  m->os = 'A';
  m->attr = prot;
  m->modified = 1;
 }
 else if(m->attr != prot)
 {
  m->attr = prot;
  m->modified = 1;
 }
}

static void lhaf_remove(ARC *arc, ENTRY *e)
{
 lha_member_remove(arc, e);
}

static int lhaf_add(ARC *arc, const char *path, const uint8_t *buf, uint32_t len,
                    uint8_t prot, const char *comment, uint32_t seconds)
{
 return lha_add(arc, path, buf, len, prot, comment, seconds);
}

const struct arc_format LhaFormat =
{
 "LHA",
 lhaf_probe,
 lhaf_probe_name,
 lhaf_open,
 lhaf_close,
 lhaf_write,
 lha_error_text,
 lhaf_caps,
 lhaf_first,
 lhaf_next,
 lhaf_path,
 lhaf_set_path,
 lhaf_set_comment,
 lhaf_get_protection,
 lhaf_set_protection,
 lhaf_remove,
 lhaf_add,
 NULL,
 NULL,
 NULL
};
