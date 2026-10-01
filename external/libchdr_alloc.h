// Force-included into every libchdr_pico translation unit. Redirects
// stdlib malloc/free/calloc/realloc to PSRAM-backed wrappers so only
// libchdr's own allocations land in PSRAM. The rest of the binary's
// allocations (notably the emulator's PCE.VRAM / PCE.RAM / MemoryMap)
// continue to use the regular C library and stay in fast SRAM.
//
// Why function-like macros are safe here (vs. our earlier failed attempt):
// the macro body uses the macro argument verbatim, without wrapping it in
// extra parens. With `#define malloc(s) chdr_libchdr_malloc(s)`, a
// declaration `extern void *malloc(size_t);` expands to
// `extern void *chdr_libchdr_malloc(size_t);` — a valid declaration.
// The earlier `#define calloc(n, sz) chdr_chdr_calloc((n), (sz))` form
// expanded the declaration to `chdr_chdr_calloc((size_t), (size_t))`,
// which the parser read as casts with no operand.
//
// Caveat: function-like macros still fire when the macro NAME is followed
// by `(`, even in struct-method dispatches like `chd->codecintf[i]->free(x)`.
// Two such sites in libchdr_chd.c are parenthesised (the macro doesn't
// match `free)(` so the struct-method call survives).

#ifndef PICOPCEPLUS_LIBCHDR_ALLOC_H
#define PICOPCEPLUS_LIBCHDR_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void *chdr_libchdr_malloc(size_t sz);
extern void  chdr_libchdr_free(void *p);
extern void *chdr_libchdr_calloc(size_t n, size_t sz);
extern void *chdr_libchdr_realloc(void *p, size_t sz);

#ifdef __cplusplus
}
#endif

#define malloc(s)     chdr_libchdr_malloc(s)
#define free(p)       chdr_libchdr_free(p)
#define calloc(n, s)  chdr_libchdr_calloc(n, s)
#define realloc(p, s) chdr_libchdr_realloc(p, s)

#endif
