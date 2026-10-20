#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>

// Fatal internal error: print a message and terminate.
__attribute__((noreturn)) void fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));

#define unreachable() fatal("unreachable code reached at %s:%d", __FILE__, __LINE__)

// Allocation wrappers. A failed allocation is unrecoverable for the compiler,
// so they report a descriptive message (size + errno) and terminate instead of
// returning NULL.
void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
char *xstrdup(const char *str);

#endif // COMMON_H
