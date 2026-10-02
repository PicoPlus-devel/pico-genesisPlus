/*
hosttest/fatfs_host.c — the FatFs API of hosttest/shim/ff.h on POSIX, for
the Sega CD disc and BIOS code. Taken from pico-pcePlus/hosttest.

Single-threaded; the firmware's SD lock is a no-op on the host.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "ff.h"
#undef DIR /* POSIX's DIR from <dirent.h>, not FF_DIR */

/* Map a firmware path to a host path (see ff.h). Two rotating buffers:
   callers map at most two paths per operation. */
static const char *map_path(const char *p)
{
	static char bufs[2][FF_MAX_LFN + 64];
	static int  cur = 0;
	if (!p || p[0] != '/')
		return p;
	const char *root = getenv("GEN_SD_ROOT");
	if (!root || !*root)
		return p;
	char *out = bufs[cur ^= 1];
	snprintf(out, sizeof(bufs[0]), "%s%s", root, p);
	return out;
}

FRESULT f_open(FIL *fp, const char *path, uint8_t mode)
{
	if (!fp || !path) return FR_DISK_ERR;
	const char *m = "rb";
	if (mode & FA_WRITE) m = (mode & FA_CREATE_ALWAYS) ? "wb" : "r+b";
	FILE *f = fopen(map_path(path), m);
	if (!f) return FR_NO_FILE;

	fseek(f, 0, SEEK_END);
	fp->size = (FSIZE_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	fp->fp = f;
	return FR_OK;
}

FRESULT f_close(FIL *fp)
{
	if (!fp || !fp->fp) return FR_OK;
	fclose((FILE *)fp->fp);
	fp->fp = NULL;
	return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
	if (!fp || !fp->fp || !buff || !br) return FR_DISK_ERR;
	*br = (UINT)fread(buff, 1, btr, (FILE *)fp->fp);
	return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
	if (!fp || !fp->fp || !buff || !bw) return FR_DISK_ERR;
	size_t n = fwrite(buff, 1, btw, (FILE *)fp->fp);
	*bw = (UINT)n;
	return (n == btw) ? FR_OK : FR_DISK_ERR;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
	if (!fp || !fp->fp) return FR_DISK_ERR;
	return fseeko((FILE *)fp->fp, (off_t)ofs, SEEK_SET) == 0 ? FR_OK : FR_DISK_ERR;
}

FSIZE_t f_tell(FIL *fp)
{
	if (!fp || !fp->fp) return 0;
	off_t t = ftello((FILE *)fp->fp);
	return t < 0 ? 0 : (FSIZE_t)t;
}

FRESULT f_opendir(FF_DIR *dp, const char *path)
{
	if (!dp || !path) return FR_DISK_ERR;
	const char *mapped = map_path(path);
	DIR *d = opendir(mapped);
	if (!d) return FR_NO_PATH;
	dp->dp = d;
	strncpy(dp->path, mapped, sizeof(dp->path) - 1);
	dp->path[sizeof(dp->path) - 1] = '\0';
	return FR_OK;
}

FRESULT f_readdir(FF_DIR *dp, FILINFO *fno)
{
	if (!dp || !dp->dp || !fno) return FR_DISK_ERR;
	fno->fname[0] = '\0';
	fno->fattrib = 0;
	fno->fsize = 0;

	struct dirent *e;
	for (;;) {
		e = readdir((DIR *)dp->dp);
		if (!e) return FR_OK; /* end of dir: fname[0] == 0 */
		if (e->d_name[0] == '.' &&
		    (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
			continue;
		break;
	}
	strncpy(fno->fname, e->d_name, sizeof(fno->fname) - 1);
	fno->fname[sizeof(fno->fname) - 1] = '\0';

	char full[FF_MAX_LFN * 2 + 64];
	snprintf(full, sizeof(full), "%s/%s", dp->path, e->d_name);
	struct stat st;
	if (stat(full, &st) == 0) {
		if (S_ISDIR(st.st_mode))
			fno->fattrib |= AM_DIR;
		fno->fsize = (FSIZE_t)st.st_size;
	}
	return FR_OK;
}

FRESULT f_closedir(FF_DIR *dp)
{
	if (!dp || !dp->dp) return FR_OK;
	closedir((DIR *)dp->dp);
	dp->dp = NULL;
	return FR_OK;
}

FRESULT f_stat(const char *path, FILINFO *fno)
{
	struct stat st;
	if (stat(map_path(path), &st) != 0)
		return FR_NO_FILE;
	if (fno) {
		const char *base = strrchr(path, '/');
		strncpy(fno->fname, base ? base + 1 : path, sizeof(fno->fname) - 1);
		fno->fname[sizeof(fno->fname) - 1] = '\0';
		fno->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
		fno->fsize = (FSIZE_t)st.st_size;
	}
	return FR_OK;
}

FRESULT f_mkdir(const char *path)
{
	return mkdir(map_path(path), 0755) == 0 ? FR_OK : FR_EXIST;
}

char *f_gets(char *buff, int len, FIL *fp)
{
	if (!buff || len <= 0 || !fp || !fp->fp) return NULL;
	return fgets(buff, len, (FILE *)fp->fp);
}
