/*
** File:        main.c
** Description: arcedit: edit archives in place without unpacking them.
**
**  Template:
**   ARCHIVE/A,ENTRIES/M,DELETE/S,RENAME/K,AS/K,COMMENT/K,PROTECT/K,ADD/S,
**   TO/K,FROM=LIST/K,QUIET/S
**
**  arcedit Work:a.lha docs readme.txt DELETE     delete entries (a drawer
**                                                 with all it holds)
**  arcedit Work:a.lha DELETE FROM RAM:list       entries from a file, one
**                                                 per line
**  arcedit Work:a.lha RENAME docs/a AS docs/b    rename a file or drawer
**  arcedit Work:a.lha docs/a COMMENT "text"      set the file note ("" clears)
**  arcedit Work:a.lha docs/a PROTECT +s-d        set protection bits
**                                                 (hsparwed, +/- to change)
**  arcedit Work:a.lha ADD RAM:x RAM:dir TO docs  add files and drawers
**                                                 (replaces existing entries)
**
**  Unchanged entries are copied as they are, only added files are packed.
**  The archive is written to a temporary file first and replaces the
**  original only when everything worked.
**
**  Return codes: 0 OK, 5 some entries weren't found, 10 nothing was done
**  (bad arguments, archive errors), 20 out of memory.
**
** Change history:
**  2026-11-14 orgin, created
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcformat.h"
#include "platform.h"

#ifdef __amigaos4__
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#else
#define RETURN_OK    0
#define RETURN_WARN  5
#define RETURN_ERROR 10
#define RETURN_FAIL  20
#endif

#define VERSION_STRING "arcedit 1.0 (14.11.2026)"

static const char __attribute__((used)) verstag[] = "\0$VER: " VERSION_STRING;
/* the shell gives commands this much stack at least */
static const char __attribute__((used)) stackcookie[] = "$STACK: 65536";

#define TEMPLATE "ARCHIVE/A,ENTRIES/M,DELETE/S,RENAME/K,AS/K,COMMENT/K,PROTECT/K,ADD/S,TO/K,FROM=LIST/K,QUIET/S"

enum
{
 ARG_ARCHIVE, ARG_ENTRIES, ARG_DELETE, ARG_RENAME, ARG_AS, ARG_COMMENT, ARG_PROTECT,
 ARG_ADD, ARG_TO, ARG_LIST, ARG_QUIET, ARG_COUNT
};

/* Archive formats, tried in this order */
static const struct arc_format *Formats[] =
{
 &LhaFormat,
 NULL
};

static int Quiet = 0;

/****************************************************************************/
/* argument parsing                                                         */
/****************************************************************************/

#ifndef __amigaos4__
/*
** Host builds: a small parser for the same template (keywords in any case,
** KEY value or KEY=value, /S switches, /M collects the remaining words).
** Returns 0 on success.
*/
static int host_readargs(int argc, char **argv, void **args)
{
 static const char *keys[ARG_COUNT] = { "ARCHIVE", "ENTRIES", "DELETE", "RENAME", "AS", "COMMENT",
                                        "PROTECT", "ADD", "TO", "FROM", "QUIET" };
 static char *multi[256];
 int i, nmulti = 0;

 for(i = 1; i < argc; i++)
 {
  char *a = argv[i], *eq = strchr(a, '=');
  int k, found = -1;
  size_t klen = eq ? (size_t)(eq - a) : strlen(a);
  for(k = 0; k < ARG_COUNT; k++)
  {
   if(strlen(keys[k]) == klen && strncasecmp(a, keys[k], klen) == 0) found = k;
  }
  if(klen == 4 && strncasecmp(a, "LIST", 4) == 0) found = ARG_LIST;
  if(found == ARG_DELETE || found == ARG_ADD || found == ARG_QUIET)
  {
   args[found] = (void *)1;
  }
  else if(found >= 0 && found != ARG_ENTRIES && found != ARG_ARCHIVE)
  {
   if(eq) args[found] = eq + 1;
   else if(i + 1 < argc) args[found] = argv[++i];
   else return -1;
  }
  else if(args[ARG_ARCHIVE] == NULL)
  {
   args[ARG_ARCHIVE] = a;
  }
  else if(nmulti < 255)
  {
   multi[nmulti++] = a;
  }
 }
 multi[nmulti] = NULL;
 if(nmulti) args[ARG_ENTRIES] = multi;
 return args[ARG_ARCHIVE] ? 0 : -1;
}
#endif

/****************************************************************************/
/* entry lists                                                              */
/****************************************************************************/

struct names
{
 char **name;
 int count, max;
};

/*
** Add a copy of s to the list. Returns 0, or -1 when out of memory.
*/
static int names_add(struct names *n, const char *s)
{
 if(n->count == n->max)
 {
  int max = n->max ? n->max * 2 : 32;
  char **p = realloc(n->name, (size_t)max * sizeof(char *));
  if(p == NULL) return -1;
  n->name = p;
  n->max = max;
 }
 n->name[n->count] = malloc(strlen(s) + 1);
 if(n->name[n->count] == NULL) return -1;
 strcpy(n->name[n->count++], s);
 return 0;
}

static void names_free(struct names *n)
{
 int i;
 for(i = 0; i < n->count; i++) free(n->name[i]);
 free(n->name);
}

/*
** Read entries from a list file, one per line (empty lines are skipped).
** Returns 0, or -1 when the file can't be read.
*/
static int names_from_file(struct names *n, const char *file)
{
 FILE *fh = fopen(file, "r");
 char line[1024];
 if(fh == NULL) return -1;
 while(fgets(line, sizeof(line), fh))
 {
  size_t l = strlen(line);
  while(l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
  if(l > 0 && names_add(n, line) != 0)
  {
   fclose(fh);
   return -1;
  }
 }
 fclose(fh);
 return 0;
}

/*
** An archive path as entered: '\' becomes '/', no leading or trailing '/'
*/
static void normalize(char *s)
{
 size_t n, i;
 for(i = 0; s[i]; i++) if(s[i] == '\\') s[i] = '/';
 while(s[0] == '/') memmove(s, s + 1, strlen(s));
 n = strlen(s);
 while(n > 0 && s[n - 1] == '/') s[--n] = 0;
}

static int lower(int c)
{
 c &= 0xFF;
 if((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 32;
 return c;
}

/*
** How an entry path relates to a name given by the user:
** 1 the entry itself (a drawer entry "x/" is matched by "x"),
** 2 inside the drawer name ("x/..."), 0 no match
*/
static int match(const char *path, const char *name)
{
 size_t n = strlen(name), i;
 if(n == 0) return 0;
 for(i = 0; i < n; i++)
 {
  if(lower(path[i]) != lower(name[i])) return 0;
 }
 if(path[n] == 0) return 1;
 if(path[n] == '/') return path[n + 1] == 0 ? 1 : 2;
 return 0;
}

/****************************************************************************/
/* operations                                                               */
/****************************************************************************/

/*
** Delete every entry matching one of the names (drawers with their
** contents). Returns the number of names that matched nothing.
*/
static int do_delete(const struct arc_format *f, ARC *arc, struct names *n, int *changed)
{
 int i, missing = 0;
 for(i = 0; i < n->count; i++)
 {
  ENTRY *e = f->first(arc), *next;
  int found = 0;
  while(e)
  {
   next = f->next(arc, e);
   if(match(f->path(e), n->name[i]))
   {
    if(!Quiet) printf("Deleted %s\n", f->path(e));
    f->remove(arc, e);
    found = 1;
   }
   e = next;
  }
  if(found) *changed = 1;
  else
  {
   printf("%s: not in the archive\n", n->name[i]);
   missing++;
  }
 }
 return missing;
}

/*
** Rename a file, or a drawer with everything in it.
** Returns RETURN_OK or RETURN_ERROR (not found, name taken).
*/
static int do_rename(const struct arc_format *f, ARC *arc, const char *from, const char *to, int *changed)
{
 ENTRY *e;
 size_t fl = strlen(from);
 int found = 0;
 char path[1024];

 if(to[0] == 0)
 {
  printf("AS needs a new name\n");
  return RETURN_ERROR;
 }
 /* the new names must not be taken by entries that aren't renamed */
 for(e = f->first(arc); e; e = f->next(arc, e))
 {
  const char *p = f->path(e);
  int how = match(p, from);
  if(how == 0) continue;
  found = 1;
  snprintf(path, sizeof(path), "%s%s", to, p + fl);
  {
   ENTRY *o;
   for(o = f->first(arc); o; o = f->next(arc, o))
   {
    if(o != e && match(f->path(o), from) == 0 && match(f->path(o), path) == 1)
    {
     printf("%s: already in the archive\n", path);
     return RETURN_ERROR;
    }
   }
  }
 }
 if(!found)
 {
  printf("%s: not in the archive\n", from);
  return RETURN_ERROR;
 }
 for(e = f->first(arc); e; e = f->next(arc, e))
 {
  const char *p = f->path(e);
  if(match(p, from) == 0) continue;
  snprintf(path, sizeof(path), "%s%s", to, p + fl);
  if(!Quiet) printf("Renamed %s as %s\n", p, path);
  f->set_path(e, path);
 }
 *changed = 1;
 return RETURN_OK;
}

/*
** New protection from a PROTECT argument: "rwed" style letters set the
** bits exactly; with + or - in front, letters are added or removed.
** Returns 0, or -1 for a bad letter.
*/
static int parse_protection(const char *spec, uint8_t old, uint8_t *out)
{
 static const char letters[] = "hsparwed";
 static const uint8_t bits[] = { ARC_PROT_H, ARC_PROT_S, ARC_PROT_P, ARC_PROT_A,
                                 ARC_PROT_R, ARC_PROT_W, ARC_PROT_E, ARC_PROT_D };
 const uint8_t rwed = ARC_PROT_R | ARC_PROT_W | ARC_PROT_E | ARC_PROT_D;
 int mode = 0;   /* 0 exact, '+' or '-' */
 uint8_t prot;

 if(spec[0] == '+' || spec[0] == '-')
 {
  prot = old;
 }
 else
 {
  prot = rwed;   /* rwed not allowed, hspa off, then set by the letters */
 }
 for(; *spec; spec++)
 {
  const char *l;
  int on;
  if(*spec == '+' || *spec == '-')
  {
   mode = *spec;
   continue;
  }
  l = strchr(letters, lower(*spec));
  if(l == NULL || *spec == 0) return -1;
  on = mode != '-';
  /* rwed bits are set when the action is NOT allowed */
  if(bits[l - letters] & rwed) on = !on;
  if(on) prot |= bits[l - letters];
  else prot &= (uint8_t)~bits[l - letters];
 }
 *out = prot;
 return 0;
}

/*
** Set the comment and/or protection of the named entries (drawers with
** their contents). Returns the number of names that matched nothing, or
** -1 for a bad protection.
*/
static int do_attributes(const struct arc_format *f, ARC *arc, struct names *n, const char *comment,
                         const char *protect, int *changed)
{
 int i, missing = 0;
 for(i = 0; i < n->count; i++)
 {
  ENTRY *e;
  int found = 0;
  for(e = f->first(arc); e; e = f->next(arc, e))
  {
   if(!match(f->path(e), n->name[i])) continue;
   found = 1;
   if(comment)
   {
    f->set_comment(e, comment);
    if(!Quiet) printf("Comment of %s set\n", f->path(e));
   }
   if(protect)
   {
    uint8_t prot;
    if(parse_protection(protect, f->get_protection(e), &prot) != 0)
    {
     printf("%s: bad protection, use the letters hsparwed\n", protect);
     return -1;
    }
    f->set_protection(e, prot);
    if(!Quiet) printf("Protection of %s set\n", f->path(e));
   }
  }
  if(found) *changed = 1;
  else
  {
   printf("%s: not in the archive\n", n->name[i]);
   missing++;
  }
 }
 return missing;
}

/* State of an ADD while drawers are scanned */
struct adder
{
 const struct arc_format *f;
 ARC *arc;
 const char *archive;      /* not added to itself */
 const char *local;        /* drawer being scanned */
 const char *prefix;       /* its path in the archive, "" or ending with '/' */
 int count;                /* entries added below it */
 int error;
};

static int add_path(struct adder *a, const char *local, const char *archpath);

static int add_scan_entry(const char *name, void *userdata)
{
 struct adder *a = userdata;
 char local[1024], archpath[1024];
 plat_join(local, sizeof(local), a->local, name);
 snprintf(archpath, sizeof(archpath), "%s%s", a->prefix, name);
 a->count++;
 return add_path(a, local, archpath);
}

/*
** Add a local file, or a drawer with all it holds, as archpath.
** Empty drawers get a drawer entry. Returns 0 or an error.
*/
static int add_path(struct adder *a, const char *local, const char *archpath)
{
 struct plat_info info;

 if(plat_info(local, &info) != 0)
 {
  plat_perror(local);
  return -1;
 }
 if(info.isdir)
 {
  struct adder sub = *a;
  char prefix[1024];
  int rc;
  if(archpath[0]) snprintf(prefix, sizeof(prefix), "%s/", archpath);
  else prefix[0] = 0;
  sub.local = local;
  sub.prefix = prefix;
  sub.count = 0;
  rc = plat_scan(local, add_scan_entry, &sub);
  if(rc != 0)
  {
   if(rc < 0 && sub.error == 0) plat_perror(local);
   return -1;
  }
  if(sub.count == 0 && prefix[0])
  {
   int err = a->f->add(a->arc, prefix, NULL, 0, info.attr, info.comment, info.seconds);
   if(err)
   {
    printf("%s: %s\n", prefix, a->f->error_text(err));
    return -1;
   }
   if(!Quiet) printf("Added %s\n", prefix);
  }
  return 0;
 }
 else
 {
  uint8_t *buf;
  uint32_t len;
  int err;
  if(strcmp(local, a->archive) == 0) return 0;   /* the archive itself */
  if(plat_read(local, &buf, &len) != 0)
  {
   plat_perror(local);
   return -1;
  }
  err = a->f->add(a->arc, archpath, buf, len, info.attr, info.comment, info.seconds);
  free(buf);
  if(err)
  {
   printf("%s: %s\n", local, a->f->error_text(err));
   return -1;
  }
  if(!Quiet) printf("Added %s\n", archpath);
  return 0;
 }
}

/*
** Add local files and drawers below the archive drawer to.
** Returns RETURN_OK or RETURN_ERROR.
*/
static int do_add(const struct arc_format *f, ARC *arc, const char *archive, struct names *n,
                  const char *to, int *changed)
{
 struct adder a;
 char prefix[1024];
 int i;

 if(to && to[0]) snprintf(prefix, sizeof(prefix), "%.1000s/", to);
 else prefix[0] = 0;
 memset(&a, 0, sizeof(a));
 a.f = f;
 a.arc = arc;
 a.archive = archive;
 for(i = 0; i < n->count; i++)
 {
  char archpath[1024];
  snprintf(archpath, sizeof(archpath), "%s%s", prefix, plat_filepart(n->name[i]));
  {
   /* a volume or assign ("RAM:") adds what it holds */
   size_t l = strlen(archpath);
   if(l > 0 && archpath[l - 1] == '/') archpath[l - 1] = 0;
  }
  if(add_path(&a, n->name[i], archpath) != 0) return RETURN_ERROR;
  *changed = 1;
 }
 return RETURN_OK;
}

/****************************************************************************/
/* main                                                                     */
/****************************************************************************/

/*
** The format of an existing archive (from its first bytes), or of a new
** one (from its name when create is set). NULL if none fits.
*/
static const struct arc_format *find_format(const char *archive, int create, int *exists)
{
 static uint8_t head[64 * 1024];
 FILE *fh = fopen(archive, "rb");
 int i;

 *exists = fh != NULL;
 if(fh)
 {
  uint32_t len = (uint32_t)fread(head, 1, sizeof(head), fh);
  fclose(fh);
  for(i = 0; Formats[i]; i++)
  {
   if(Formats[i]->probe(head, len)) return Formats[i];
  }
  return NULL;
 }
 if(create)
 {
  for(i = 0; Formats[i]; i++)
  {
   if(Formats[i]->probe_name(archive)) return Formats[i];
  }
 }
 return NULL;
}

int main(int argc, char **argv)
{
 void *args[ARG_COUNT];
 struct names names = { NULL, 0, 0 };
 const struct arc_format *f;
 ARC *arc = NULL;
 const char *archive;
 char tmp[1024];
 int rc = RETURN_ERROR, changed = 0, exists, err, ops, i;
#ifdef __amigaos4__
 struct RDArgs *rda;
#endif

 memset(args, 0, sizeof(args));
#ifdef __amigaos4__
 rda = IDOS->ReadArgs(TEMPLATE, (int32 *)args, NULL);
 if(rda == NULL)
 {
  IDOS->PrintFault(IDOS->IoErr(), "arcedit");
  return RETURN_ERROR;
 }
#else
 if(host_readargs(argc, argv, args) != 0)
 {
  printf("Usage: arcedit %s\n", TEMPLATE);
  return RETURN_ERROR;
 }
#endif
 (void)argc;
 (void)argv;

 archive = args[ARG_ARCHIVE];
 Quiet = args[ARG_QUIET] != NULL;

 /* exactly one kind of operation; COMMENT and PROTECT may go together */
 ops = (args[ARG_DELETE] != NULL) + (args[ARG_RENAME] != NULL) + (args[ARG_ADD] != NULL) +
       (args[ARG_COMMENT] != NULL || args[ARG_PROTECT] != NULL);
 if(ops != 1)
 {
  printf("Give one of DELETE, RENAME ... AS, ADD, COMMENT and/or PROTECT\n");
  goto done;
 }
 if(args[ARG_RENAME] && (args[ARG_AS] == NULL || args[ARG_ENTRIES] || args[ARG_LIST]))
 {
  printf("Use RENAME <entry> AS <new name>\n");
  goto done;
 }

 /* the names: ENTRIES, then the lines of the LIST file */
 if(args[ARG_ENTRIES])
 {
  char **e = args[ARG_ENTRIES];
  for(i = 0; e[i]; i++)
  {
   if(names_add(&names, e[i]) != 0)
   {
    rc = RETURN_FAIL;
    goto done;
   }
  }
 }
 if(args[ARG_LIST] && names_from_file(&names, args[ARG_LIST]) != 0)
 {
  plat_perror(args[ARG_LIST]);
  goto done;
 }
 if(!args[ARG_ADD])
 {
  for(i = 0; i < names.count; i++) normalize(names.name[i]);
 }
 if(names.count == 0 && !args[ARG_RENAME])
 {
  printf("No entries given\n");
  goto done;
 }

 f = find_format(archive, args[ARG_ADD] != NULL, &exists);
 if(f == NULL)
 {
  if(exists) printf("%s: not an archive type arcedit knows (LHA)\n", archive);
  else printf("%s: no such archive\n", archive);
  goto done;
 }
 arc = f->open(archive, args[ARG_ADD] != NULL, &err);
 if(arc == NULL)
 {
  printf("%s: %s\n", archive, f->error_text(err));
  goto done;
 }

 rc = RETURN_OK;
 if(args[ARG_DELETE])
 {
  if(do_delete(f, arc, &names, &changed) > 0) rc = RETURN_WARN;
 }
 else if(args[ARG_RENAME])
 {
  char from[1024], to[1024];
  snprintf(from, sizeof(from), "%s", (char *)args[ARG_RENAME]);
  snprintf(to, sizeof(to), "%s", (char *)args[ARG_AS]);
  normalize(from);
  normalize(to);
  rc = do_rename(f, arc, from, to, &changed);
 }
 else if(args[ARG_ADD])
 {
  char to[1024];
  snprintf(to, sizeof(to), "%s", args[ARG_TO] ? (char *)args[ARG_TO] : "");
  normalize(to);
  rc = do_add(f, arc, archive, &names, to, &changed);
 }
 else
 {
  int missing = do_attributes(f, arc, &names, args[ARG_COMMENT], args[ARG_PROTECT], &changed);
  if(missing < 0) rc = RETURN_ERROR;
  else if(missing > 0) rc = RETURN_WARN;
 }

 /* write a new archive next to the old one, then replace it */
 if(changed && rc != RETURN_ERROR)
 {
  snprintf(tmp, sizeof(tmp), "%s.arcedit", archive);
  err = f->write(arc, tmp);
  f->close(arc);
  arc = NULL;
  if(err)
  {
   printf("%s: %s\n", tmp, f->error_text(err));
   plat_delete(tmp);
   rc = RETURN_ERROR;
  }
  else if(plat_replace(tmp, archive) != 0)
  {
   plat_perror(archive);
   plat_delete(tmp);
   rc = RETURN_ERROR;
  }
 }

done:
 if(arc) f->close(arc);
 names_free(&names);
#ifdef __amigaos4__
 IDOS->FreeArgs(rda);
#endif
 return rc;
}
