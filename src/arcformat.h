/*
** File:        arcformat.h
** Description: The interface every archive format of arcedit provides.
**              The command (main.c) only uses these calls, so a new format
**              is a new backend added to the Formats[] list in main.c.
**
** Change history:
**  2026-11-14 orgin, created
*/

#ifndef ARCFORMAT_H
#define ARCFORMAT_H

#include <stdint.h>

/* Protection bits as in the low byte of the Amiga fib_Protection:
   bits 0-3 (d e w r) are set when NOT allowed, bits 4-7 (a p s h) are set
   when the flag is on */
#define ARC_PROT_D 0x01
#define ARC_PROT_E 0x02
#define ARC_PROT_W 0x04
#define ARC_PROT_R 0x08
#define ARC_PROT_A 0x10
#define ARC_PROT_P 0x20
#define ARC_PROT_S 0x40
#define ARC_PROT_H 0x80

/* Operations a format supports (caps()), may depend on the archive */
#define ARC_CAN_DELETE  0x01
#define ARC_CAN_RENAME  0x02
#define ARC_CAN_COMMENT 0x04
#define ARC_CAN_PROTECT 0x08
#define ARC_CAN_ADD     0x10
#define ARC_CAN_ALL     0x1F

typedef void ARC;     /* an open archive */
typedef void ENTRY;   /* one entry of it */

struct arc_format
{
 const char *name;

 /* 2 if the file's first bytes (len of them) start like this format, 1 if
    the format is found further in (self-extracting archives), else 0 */
 int (*probe)(const uint8_t *head, uint32_t len);
 /* TRUE if a new archive with this name should be this format (extension) */
 int (*probe_name)(const char *path);

 /* open (create: a missing file is a new, empty archive); NULL on error,
    *err gets a code for error_text() */
 ARC *(*open)(const char *path, int create, int *err);
 void (*close)(ARC *arc);
 /* write the edited archive to another file; 0 on success, else an error code */
 int (*write)(ARC *arc, const char *path);
 const char *(*error_text)(int err);
 /* ARC_CAN_* operations possible on this archive; calls for the others may be NULL */
 uint32_t (*caps)(ARC *arc);

 /* entries in archive order */
 ENTRY *(*first)(ARC *arc);
 ENTRY *(*next)(ARC *arc, ENTRY *e);
 /* full path, '/' separated; drawer entries end with '/' */
 const char *(*path)(ENTRY *e);

 void (*set_path)(ENTRY *e, const char *path);
 void (*set_comment)(ENTRY *e, const char *comment);
 uint8_t (*get_protection)(ENTRY *e);
 void (*set_protection)(ENTRY *e, uint8_t prot);
 void (*remove)(ARC *arc, ENTRY *e);

 /* add or replace an entry from memory; 0 on success, else an error code */
 int (*add)(ARC *arc, const char *path, const uint8_t *buf, uint32_t len,
            uint8_t prot, const char *comment, uint32_t seconds);

 /* optional: the type of this archive when it differs from name (TGZ) */
 const char *(*type_name)(ARC *arc);
};

extern const struct arc_format LhaFormat;
extern const struct arc_format ZipFormat;
extern const struct arc_format TarFormat;
extern const struct arc_format GzFormat;
extern const struct arc_format LzxFormat;

#endif
