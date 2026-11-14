/*
** File:        tar.h
** Description: The TAR backend's calls, for the gzip backend (TGZ), which
**              runs it on an unpacked copy of the archive.
**
** Change history:
**  2026-11-14 orgin, created
*/

#ifndef TAR_H
#define TAR_H

#include "arcformat.h"

extern const struct arc_format TarFormat;

ARC *tar_open_file(const char *path, int create, int *err);
void tar_close_file(ARC *arc);
int tar_write_file(ARC *arc, const char *path);

#endif
