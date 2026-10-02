// Host-build allocator wrappers for libchdr (from pico-pcePlus/hosttest).
// force-included into every libchdr source so it can route allocations to
// PSRAM on the Pico — here we just bounce them to the system allocator.
//
// The force-include is applied globally on the host build's gcc command
// line (vs. per-target on the Pico cmake), so it lands on THIS file too.
// We must undef BEFORE pulling in <stdlib.h> so the standard declarations
// retain their original names (and `return malloc(sz);` calls real malloc,
// not chdr_libchdr_malloc → infinite recursion).
#undef malloc
#undef free
#undef calloc
#undef realloc
#include <stdlib.h>
#include <stddef.h>

void *chdr_libchdr_malloc(size_t sz)            { return malloc(sz); }
void  chdr_libchdr_free  (void *p)              { free(p); }
void *chdr_libchdr_calloc(size_t n, size_t sz)  { return calloc(n, sz); }
void *chdr_libchdr_realloc(void *p, size_t sz)  { return realloc(p, sz); }
