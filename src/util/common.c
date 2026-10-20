// A compiler front-end has no recoverable path after an allocation failure, so
// the x* wrappers report the size/errno and exit; callers never check for NULL.

#include "util/common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((noreturn)) void fatal(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

void *xmalloc(size_t size)
{
    void *ptr = malloc(size);
    if (!ptr)
        fatal("out of memory: malloc(%zu) failed: %s", size, strerror(errno));
    return ptr;
}

void *xcalloc(size_t count, size_t size)
{
    void *ptr = calloc(count, size);
    if (!ptr)
        fatal("out of memory: calloc(%zu, %zu) failed: %s", count, size, strerror(errno));
    return ptr;
}

char *xstrdup(const char *str)
{
    char *copy = strdup(str);
    if (!copy)
        fatal("out of memory: strdup failed: %s", strerror(errno));
    return copy;
}
