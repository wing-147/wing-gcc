#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Borrowed byte range. The representation is visible so views can be embedded
// and passed by value, but its fields are logically private: construct and
// inspect views through the functions below. The owner of the bytes must
// outlive every copied view; embedded NUL bytes are ordinary data.
typedef struct {
    const char *data;
    size_t length;
} StringView;

// Construct a borrowed view over exactly `length` bytes starting at `data`.
static inline StringView string_view(const char *data, size_t length)
{
    return (StringView){ .data = data, .length = length };
}

// Construct a borrowed view over [begin, end); both pointers must delimit one
// valid byte range and `end` must not precede `begin`.
static inline StringView string_view_from_range(const char *begin, const char *end)
{
    return string_view(begin, (size_t)(end - begin));
}

// Construct a borrowed view over a non-NULL NUL-terminated string.
static inline StringView string_view_from_cstr(const char *text)
{
    return string_view(text, strlen(text));
}

// Return the first byte of the borrowed range, or NULL for a null view.
static inline const char *string_view_begin(StringView view)
{
    return view.data;
}

// Return the one-past-last byte without performing arithmetic on NULL.
static inline const char *string_view_end(StringView view)
{
    return view.data ? view.data + view.length : NULL;
}

// Return the number of bytes in the borrowed range.
static inline size_t string_view_length(StringView view)
{
    return view.length;
}

// Compare two byte views without treating embedded NUL bytes as terminators.
static inline bool string_view_equal(StringView lhs, StringView rhs)
{
    size_t length = string_view_length(lhs);
    return length == string_view_length(rhs) &&
           (length == 0 || memcmp(string_view_begin(lhs), string_view_begin(rhs), length) == 0);
}

// Compare a byte view with a non-NULL NUL-terminated string.
static inline bool string_view_equal_cstr(StringView lhs, const char *rhs)
{
    return string_view_equal(lhs, string_view_from_cstr(rhs));
}

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
