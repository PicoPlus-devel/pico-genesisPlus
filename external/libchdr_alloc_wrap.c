/*
external/libchdr_alloc_wrap.c — where libchdr's heap goes on the Pico.

external/libchdr_alloc.h (from pico-pcePlus) is force-included into every
libchdr translation unit and renames its malloc/free/calloc/realloc calls to
these, so libchdr alone allocates from PSRAM -- its hunk map is ~450 KB for a
full disc -- while the emulator's own buffers stay in SRAM.

Unlike pico-pcePlus these use the non-panicking PSRAM allocator: libchdr
reports CHDERR_OUT_OF_MEMORY, and the disc then fails to open with a message,
instead of the board stopping. realloc (used by nothing on libchdr's decode
path) keeps pico_shared's PSRAM realloc.
*/
#include <stddef.h>
#include <string.h>

extern void *gwcd_port_psram_alloc(size_t size); /* zeroed */
extern void gwcd_port_psram_free(void *p);
extern void *frens_f_realloc(void *ptr, size_t newSize);

void *chdr_libchdr_malloc(size_t sz)
{
    return sz ? gwcd_port_psram_alloc(sz) : NULL;
}

void chdr_libchdr_free(void *p)
{
    if (p)
        gwcd_port_psram_free(p);
}

void *chdr_libchdr_calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    return total ? gwcd_port_psram_alloc(total) : NULL;
}

void *chdr_libchdr_realloc(void *p, size_t sz)
{
    return frens_f_realloc(p, sz);
}
