/*
** File:        platform.h
** Description: The few file system calls lhaedit needs, for AmigaOS 4
**              (dos.library) and for host builds (POSIX, used by the tests).
**
** Change history:
**  2026-11-14 orgin, created
*/

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

#define PLAT_MAXCOMMENT 256

struct plat_info
{
 int isdir;
 uint8_t attr;                      /* Amiga protection low byte, see LHA_PROT_* */
 char comment[PLAT_MAXCOMMENT];
 uint32_t seconds;                  /* modification time, seconds since 1970, local time */
};

/* File or drawer information. Returns 0 on success. */
int plat_info(const char *path, struct plat_info *info);

/* Read a whole file into a malloc'ed buffer. Returns 0 on success. */
int plat_read(const char *path, uint8_t **buf, uint32_t *len);

/* Drawer scan: calls fn(name, userdata) for each entry. Returns 0 on success. */
int plat_scan(const char *dir, int (*fn)(const char *name, void *userdata), void *userdata);

/* Join a drawer and a name into buf ("RAM:" + "x" = "RAM:x", "a" + "b" = "a/b") */
void plat_join(char *buf, uint32_t size, const char *dir, const char *name);

/* The last part of a path ("" for a volume or assign) */
const char *plat_filepart(const char *path);

/* Replace target with the new file tmp, keeping target's protection and
   comment. Returns 0 on success. */
int plat_replace(const char *tmp, const char *target);

/* Delete a file (the temporary archive on failure) */
void plat_delete(const char *path);

/* Print an error message for the last failed system call */
void plat_perror(const char *what);

#endif
