#ifndef DIAGNOSTIC_H
#define DIAGNOSTIC_H

#include "util/source.h"

#include <stdbool.h>

// True once any error has been reported (warnings do not set it).
bool had_error(void);
void add_error(int count);

// Print a message without source location.
void error(const char *format, ...) __attribute__((format(printf, 1, 2)));
void warn(const char *format, ...) __attribute__((format(printf, 1, 2)));
// Print an error/warning for a resolved source span (see source_span()).
// Diagnostics only render; they do not analyse locations.
void error_at(const SourceSpan *span, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void warn_at(const SourceSpan *span, const char *format, ...) __attribute__((format(printf, 2, 3)));

// Convenience: resolve [start, end) of the current source and report.
// The single entry point for "report at a raw source range".
void error_span(const char *start, const char *end, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
void warn_span(const char *start, const char *end, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

#endif // DIAGNOSTIC_H
