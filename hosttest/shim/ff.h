/*
hosttest/shim/ff.h — just enough of the FatFs API for the Sega CD code
(port/scd_disc.c, port/scd_bios.c) to run unchanged on the host, backed by
POSIX in hosttest/fatfs_host.c. Taken from pico-pcePlus/hosttest/shim/ff.h.

Paths: with GEN_SD_ROOT set, an absolute firmware path ("/bios/x.md") is
looked up under that directory, so an SD card layout can be reproduced on
disk; without it absolute paths are used as they are, so the harness can
point straight at a rom collection.
*/
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FF_MAX_LFN 255

#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_CREATE_ALWAYS 0x08

#define AM_DIR 0x10
#define AM_HID 0x02

typedef unsigned int UINT;
typedef uint64_t     FSIZE_t;

typedef enum {
	FR_OK = 0,
	FR_DISK_ERR,
	FR_NO_FILE,
	FR_NO_PATH,
	FR_DENIED,
	FR_EXIST,
} FRESULT;

/* FIL: void *fp holds a host FILE *; size cached at open for f_size(). */
typedef struct { FSIZE_t size; void *fp; } FIL;

/* FILINFO: only fname / fattrib / fsize are read. */
typedef struct {
	FSIZE_t  fsize;
	char     fname[FF_MAX_LFN + 1];
	uint8_t  fattrib;
} FILINFO;

/* FF_DIR wraps a POSIX DIR *; `DIR` clashes with <dirent.h>, which the POSIX
   backend includes, so the core's `DIR` is #defined to FF_DIR. */
typedef struct { void *dp; char path[FF_MAX_LFN + 8]; } FF_DIR;
#ifndef DIR
#define DIR FF_DIR
#endif

FRESULT f_open    (FIL *fp, const char *path, uint8_t mode);
FRESULT f_close   (FIL *fp);
FRESULT f_read    (FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write   (FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek   (FIL *fp, FSIZE_t ofs);
FRESULT f_opendir (FF_DIR *dp, const char *path);
FRESULT f_readdir (FF_DIR *dp, FILINFO *fno);
FRESULT f_closedir(FF_DIR *dp);
FRESULT f_stat    (const char *path, FILINFO *fno);
FRESULT f_mkdir   (const char *path);
char   *f_gets    (char *buff, int len, FIL *fp);

static inline FSIZE_t f_size(FIL *fp) { return fp->size; }
FSIZE_t f_tell(FIL *fp);
