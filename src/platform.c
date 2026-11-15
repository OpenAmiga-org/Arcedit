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
 info->size = (uint64_t)dat->FileSize;
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

void plat_fault(const char *what, char *buf, uint32_t size)
{
 IDOS->Fault(IDOS->IoErr(), what, buf, size);
}

int plat_not_found(void)
{
 int32 e = IDOS->IoErr();
 return e == ERROR_OBJECT_NOT_FOUND || e == ERROR_DIR_NOT_FOUND || e == ERROR_DEVICE_NOT_MOUNTED;
}

/*
** The drawer part of a path into buf ("RAM:x/y.lha" gives "RAM:x")
*/
static void parent_of(const char *path, char *buf, uint32_t size)
{
 IUtility->Strlcpy(buf, path, size);
 *((char *)IDOS->PathPart(buf)) = 0;
}

int plat_writable(const char *path, char *buf, uint32_t size)
{
 struct ExamineData *dat = IDOS->ExamineObjectTags(EX_StringNameInput, path, TAG_END);
 struct InfoData *id = IDOS->AllocDosObject(DOS_INFODATA, NULL);
 char dir[1024];
 int rc = 0;

 if(dat)
 {
  /* the old archive is deleted when the new one replaces it */
  if(dat->Protection & EXDF_NO_DELETE) rc = 1;
  IDOS->FreeDosObject(DOS_EXAMINEDATA, dat);
  if(rc)
  {
   snprintf(buf, size, "%s is protected from deletion", path);
   IDOS->FreeDosObject(DOS_INFODATA, id);
   return rc;
  }
 }
 parent_of(path, dir, sizeof(dir));
 if(id && IDOS->GetDiskInfoTags(GDI_StringNameInput, dir[0] ? dir : "", GDI_InfoData, id, TAG_END))
 {
  if(id->id_DiskState == ID_DISKSTATE_WRITE_PROTECTED)
  {
   snprintf(buf, size, "the volume of %s is write protected", path);
   rc = 1;
  }
 }
 if(id) IDOS->FreeDosObject(DOS_INFODATA, id);
 return rc;
}

static int space_of_dir(const char *dir, uint64_t *bytes)
{
 struct InfoData *id = IDOS->AllocDosObject(DOS_INFODATA, NULL);
 int rc = -1;
 if(id == NULL) return -1;
 if(IDOS->GetDiskInfoTags(GDI_StringNameInput, dir[0] ? dir : "", GDI_InfoData, id, TAG_END))
 {
  /* RAM: and other growing volumes always report themselves full: unknown */
  if(id->id_NumBlocksUsed < id->id_NumBlocks)
  {
   *bytes = (uint64_t)(id->id_NumBlocks - id->id_NumBlocksUsed) * (uint64_t)id->id_BytesPerBlock;
   rc = 0;
  }
 }
 IDOS->FreeDosObject(DOS_INFODATA, id);
 return rc;
}

uint64_t plat_size(const char *path)
{
 struct ExamineData *dat = IDOS->ExamineObjectTags(EX_StringNameInput, path, TAG_END);
 uint64_t size = 0;
 if(dat)
 {
  size = (uint64_t)dat->FileSize;
  IDOS->FreeDosObject(DOS_EXAMINEDATA, dat);
 }
 return size;
}

int plat_free_space(const char *path, uint64_t *bytes)
{
 char dir[1024];
 parent_of(path, dir, sizeof(dir));
 return space_of_dir(dir, bytes);
}

static char WorkDir[1024];

void plat_set_workdir(const char *dir)
{
 IUtility->Strlcpy(WorkDir, dir ? dir : "", sizeof(WorkDir));
}

int plat_check_workdir(const char *dir, char *buf, uint32_t size)
{
 struct ExamineData *dat = IDOS->ExamineObjectTags(EX_StringNameInput, dir, TAG_END);
 struct InfoData *id;
 int rc = 0;
 if(dat == NULL)
 {
  if(plat_not_found()) snprintf(buf, size, "work drawer %s doesn't exist", dir);
  else IDOS->Fault(IDOS->IoErr(), dir, buf, size);
  return 1;
 }
 if(!EXD_IS_DIRECTORY(dat))
 {
  snprintf(buf, size, "work drawer %s is not a drawer", dir);
  rc = 1;
 }
 IDOS->FreeDosObject(DOS_EXAMINEDATA, dat);
 if(rc) return rc;
 id = IDOS->AllocDosObject(DOS_INFODATA, NULL);
 if(id && IDOS->GetDiskInfoTags(GDI_StringNameInput, dir, GDI_InfoData, id, TAG_END))
 {
  if(id->id_DiskState == ID_DISKSTATE_WRITE_PROTECTED)
  {
   snprintf(buf, size, "the volume of work drawer %s is write protected", dir);
   rc = 1;
  }
 }
 if(id) IDOS->FreeDosObject(DOS_INFODATA, id);
 return rc;
}

static uint32_t TempCount;

static void temp_in(const char *dir, char *buf, uint32_t size)
{
 char name[32];
 uint32_t id = (uint32_t)IExec->FindTask(NULL) ^ (++TempCount * 0x9E3779B9u);
 IUtility->Strlcpy(buf, dir, size);
 snprintf(name, sizeof(name), "ae%08lx.tmp", (unsigned long)id);
 IDOS->AddPart(buf, name, size);
}

void plat_temp_name_near(const char *path, char *buf, uint32_t size)
{
 char dir[1024];
 parent_of(path, dir, sizeof(dir));
 temp_in(dir, buf, size);
}

void plat_temp_name(const char *path, char *buf, uint32_t size)
{
 if(WorkDir[0]) temp_in(WorkDir, buf, size);
 else plat_temp_name_near(path, buf, size);
}

int plat_temp_free_space(const char *path, uint64_t *bytes)
{
 return WorkDir[0] ? space_of_dir(WorkDir, bytes) : plat_free_space(path, bytes);
}

int plat_move(const char *from, const char *to)
{
 BPTR in, out;
 uint8_t *buf;
 int32 n = 0, err = 0;
 if(IDOS->Rename(from, to)) return 0;
 /* another volume: copy */
 buf = malloc(65536);
 in = IDOS->FOpen(from, MODE_OLDFILE, 0);
 out = in ? IDOS->FOpen(to, MODE_NEWFILE, 0) : 0;
 if(buf == NULL || in == 0 || out == 0) err = buf ? IDOS->IoErr() : ERROR_NO_FREE_STORE;
 while(err == 0 && (n = IDOS->FRead(in, buf, 1, 65536)) > 0)
 {
  if(IDOS->FWrite(out, buf, 1, n) != (uint32)n) err = IDOS->IoErr();
 }
 if(err == 0 && n < 0) err = IDOS->IoErr();
 if(out && !IDOS->FClose(out) && err == 0) err = IDOS->IoErr();
 if(in) IDOS->FClose(in);
 free(buf);
 if(err)
 {
  if(out) IDOS->Delete(to);
  IDOS->SetIoErr(err);
  return -1;
 }
 IDOS->Delete(from);
 return 0;
}

int plat_break(void)
{
 static int broken = 0;
 if(!broken && (IExec->SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)) broken = 1;
 return broken;
}

#else  /* host build (POSIX) */

#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
 info->size = (uint64_t)st.st_size;
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

void plat_fault(const char *what, char *buf, uint32_t size)
{
 snprintf(buf, size, "%s: %s", what, strerror(errno));
}

int plat_not_found(void)
{
 return errno == ENOENT || errno == ENOTDIR;
}

static void parent_of(const char *path, char *buf, uint32_t size)
{
 char *p;
 snprintf(buf, size, "%s", path);
 p = strrchr(buf, '/');
 if(p) *p = 0;
 else snprintf(buf, size, ".");
}

int plat_writable(const char *path, char *buf, uint32_t size)
{
 char dir[1024];
 parent_of(path, dir, sizeof(dir));
 if(access(dir, W_OK) != 0)
 {
  snprintf(buf, size, "the drawer of %s can't be written", path);
  return 1;
 }
 return 0;
}

static int space_of_dir(const char *dir, uint64_t *bytes)
{
 struct statvfs sv;
 if(statvfs(dir, &sv) != 0) return -1;
 *bytes = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
 return 0;
}

int plat_free_space(const char *path, uint64_t *bytes)
{
 char dir[1024];
 parent_of(path, dir, sizeof(dir));
 return space_of_dir(dir, bytes);
}

uint64_t plat_size(const char *path)
{
 struct stat st;
 return stat(path, &st) == 0 ? (uint64_t)st.st_size : 0;
}

static char WorkDir[1024];

void plat_set_workdir(const char *dir)
{
 snprintf(WorkDir, sizeof(WorkDir), "%s", dir ? dir : "");
}

int plat_check_workdir(const char *dir, char *buf, uint32_t size)
{
 struct stat st;
 if(stat(dir, &st) != 0)
 {
  if(plat_not_found()) snprintf(buf, size, "work drawer %s doesn't exist", dir);
  else plat_fault(dir, buf, size);
  return 1;
 }
 if(!S_ISDIR(st.st_mode))
 {
  snprintf(buf, size, "work drawer %s is not a drawer", dir);
  return 1;
 }
 if(access(dir, W_OK) != 0)
 {
  snprintf(buf, size, "work drawer %s can't be written", dir);
  return 1;
 }
 return 0;
}

static uint32_t TempCount;

static void temp_in(const char *dir, char *buf, uint32_t size)
{
 char name[32];
 snprintf(name, sizeof(name), "ae%08lx.tmp", (unsigned long)((uint32_t)getpid() ^ (++TempCount * 0x9E3779B9u)));
 plat_join(buf, size, dir, name);
}

void plat_temp_name_near(const char *path, char *buf, uint32_t size)
{
 char dir[1024];
 parent_of(path, dir, sizeof(dir));
 temp_in(dir, buf, size);
}

void plat_temp_name(const char *path, char *buf, uint32_t size)
{
 if(WorkDir[0]) temp_in(WorkDir, buf, size);
 else plat_temp_name_near(path, buf, size);
}

int plat_temp_free_space(const char *path, uint64_t *bytes)
{
 return WorkDir[0] ? space_of_dir(WorkDir, bytes) : plat_free_space(path, bytes);
}

int plat_move(const char *from, const char *to)
{
 FILE *in, *out = NULL;
 uint8_t *buf;
 size_t n;
 int err = 0;
 if(rename(from, to) == 0) return 0;
 if(errno != EXDEV) return -1;
 /* another file system: copy */
 buf = malloc(65536);
 in = fopen(from, "rb");
 if(buf && in) out = fopen(to, "wb");
 if(buf == NULL) err = ENOMEM;
 else if(in == NULL || out == NULL) err = errno;
 while(err == 0 && (n = fread(buf, 1, 65536, in)) > 0)
 {
  if(fwrite(buf, 1, n, out) != n) err = errno ? errno : EIO;
 }
 if(err == 0 && ferror(in)) err = EIO;
 if(out && fclose(out) != 0 && err == 0) err = errno;
 if(in) fclose(in);
 free(buf);
 if(err)
 {
  if(out) remove(to);
  errno = err;
  return -1;
 }
 remove(from);
 return 0;
}

static volatile sig_atomic_t Broken;

static void on_sigint(int sig)
{
 (void)sig;
 Broken = 1;
}

int plat_break(void)
{
 static int installed = 0;
 if(!installed)
 {
  signal(SIGINT, on_sigint);
  installed = 1;
 }
 return Broken;
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
