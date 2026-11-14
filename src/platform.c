/*
** File:        platform.c
** Description: File system calls for AmigaOS 4 and for host builds.
**
** Change history:
**  2026-11-14 orgin, created
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#ifdef __amigaos4__

#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

/* Days from 1970-01-01 to 1978-01-01, the Amiga epoch */
#define AMIGA_EPOCH_DAYS 2922

int plat_info(const char *path, struct plat_info *info)
{
 struct ExamineData *dat = IDOS->ExamineObjectTags(EX_StringNameInput, path, TAG_END);
 if(dat == NULL) return -1;
 info->isdir = EXD_IS_DIRECTORY(dat) ? 1 : 0;
 info->attr = (uint8_t)(dat->Protection & 0xFF);
 IUtility->Strlcpy(info->comment, dat->Comment ? dat->Comment : "", sizeof(info->comment));
 info->seconds = (uint32_t)(dat->Date.ds_Days + AMIGA_EPOCH_DAYS) * 86400 +
                 (uint32_t)dat->Date.ds_Minute * 60 + (uint32_t)dat->Date.ds_Tick / TICKS_PER_SECOND;
 IDOS->FreeDosObject(DOS_EXAMINEDATA, dat);
 return 0;
}

int plat_scan(const char *dir, int (*fn)(const char *name, void *userdata), void *userdata)
{
 APTR context = IDOS->ObtainDirContextTags(EX_StringNameInput, dir, EX_DataFields, EXF_NAME, TAG_END);
 struct ExamineData *dat;
 int rc = 0;

 if(context == NULL) return -1;
 while(rc == 0 && (dat = IDOS->ExamineDir(context)) != NULL)
 {
  rc = fn(dat->Name, userdata);
 }
 if(rc == 0 && IDOS->IoErr() != ERROR_NO_MORE_ENTRIES) rc = -1;
 IDOS->ReleaseDirContext(context);
 return rc;
}

void plat_join(char *buf, uint32_t size, const char *dir, const char *name)
{
 IUtility->Strlcpy(buf, dir, size);
 IDOS->AddPart(buf, name, size);
}

const char *plat_filepart(const char *path)
{
 return IDOS->FilePart(path);
}

int plat_replace(const char *tmp, const char *target)
{
 struct ExamineData *dat = IDOS->ExamineObjectTags(EX_StringNameInput, target, TAG_END);
 uint32_t prot = 0;
 char comment[PLAT_MAXCOMMENT];

 comment[0] = 0;
 if(dat)
 {
  prot = dat->Protection;
  IUtility->Strlcpy(comment, dat->Comment ? dat->Comment : "", sizeof(comment));
  IDOS->FreeDosObject(DOS_EXAMINEDATA, dat);
  if(!IDOS->Delete(target)) return -1;
 }
 if(!IDOS->Rename(tmp, target)) return -1;
 if(dat)
 {
  IDOS->SetProtection(target, prot);
  if(comment[0]) IDOS->SetComment(target, comment);
 }
 return 0;
}

void plat_delete(const char *path)
{
 IDOS->Delete(path);
}

void plat_perror(const char *what)
{
 IDOS->PrintFault(IDOS->IoErr(), what);
}

#else  /* host build (POSIX) */

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int plat_info(const char *path, struct plat_info *info)
{
 struct stat st;
 struct tm tm;
 if(stat(path, &st) != 0) return -1;
 info->isdir = S_ISDIR(st.st_mode) ? 1 : 0;
 info->attr = 0;   /* rwed allowed */
 if(!(st.st_mode & S_IRUSR)) info->attr |= 0x08;
 if(!(st.st_mode & S_IWUSR)) info->attr |= 0x04 | 0x01;
 if(!(st.st_mode & S_IXUSR) && !info->isdir) info->attr |= 0x02;
 info->comment[0] = 0;
 /* local time written as seconds, as LhA does */
 localtime_r(&st.st_mtime, &tm);
 info->seconds = (uint32_t)timegm(&tm);
 return 0;
}

int plat_read(const char *path, uint8_t **buf, uint32_t *len);

int plat_scan(const char *dir, int (*fn)(const char *name, void *userdata), void *userdata)
{
 DIR *d = opendir(dir);
 struct dirent *de;
 int rc = 0;
 if(d == NULL) return -1;
 while(rc == 0 && (de = readdir(d)) != NULL)
 {
  if(strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
  rc = fn(de->d_name, userdata);
 }
 closedir(d);
 return rc;
}

void plat_join(char *buf, uint32_t size, const char *dir, const char *name)
{
 size_t n;
 snprintf(buf, size, "%s", dir);
 n = strlen(buf);
 if(n > 0 && buf[n - 1] != '/' && buf[n - 1] != ':') snprintf(buf + n, size - n, "/");
 n = strlen(buf);
 snprintf(buf + n, size - n, "%s", name);
}

const char *plat_filepart(const char *path)
{
 const char *p = strrchr(path, '/');
 return p ? p + 1 : path;
}

int plat_replace(const char *tmp, const char *target)
{
 return rename(tmp, target);
}

void plat_delete(const char *path)
{
 remove(path);
}

void plat_perror(const char *what)
{
 perror(what);
}

#endif

/*
** Read a whole file into a malloc'ed buffer (stdio on both platforms)
*/
int plat_read(const char *path, uint8_t **buf, uint32_t *len)
{
 FILE *fh = fopen(path, "rb");
 long size;
 *buf = NULL;
 *len = 0;
 if(fh == NULL) return -1;
 if(fseek(fh, 0, SEEK_END) != 0 || (size = ftell(fh)) < 0 || fseek(fh, 0, SEEK_SET) != 0)
 {
  fclose(fh);
  return -1;
 }
 *buf = malloc(size ? (size_t)size : 1);
 if(*buf == NULL || (size && fread(*buf, 1, (size_t)size, fh) != (size_t)size))
 {
  free(*buf);
  *buf = NULL;
  fclose(fh);
  return -1;
 }
 fclose(fh);
 *len = (uint32_t)size;
 return 0;
}
