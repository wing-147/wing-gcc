// Diagnostics only render: locations are interpreted once by source_span(), so
// this file never re-implements tab/UTF-8/CR rules and survives changes to the
// location model unchanged.

#include "util/diagnostic.h"
#include "util/source.h"

#include <stdarg.h>
#include <stdio.h>

// Width of the line-number gutter, matching GCC's default diagnostics.
#define GUTTER 5

typedef enum {
    DIAG_ERROR,
    DIAG_WARNING,
} DiagKind;

// Number of errors reported so far. Warnings are not counted.
static int g_errors = 0;

// Print `str[0..len)` with tabs expanded to spaces, so that the caret on the
// following line lines up with the rendered source line.
static void print_source_line(const char *str, int len)
{
    int width = 0;

    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        int next = display_width_step(width, c);

        if (c == '\t') {
            for (int j = width; j < next; j++)
                fputc(' ', stderr);
        } else {
            fputc(c, stderr);
        }

        width = next;
    }
}

static const char *diag_name(DiagKind kind)
{
    return kind == DIAG_WARNING ? "warning" : "error";
}

// Print a GCC-style diagnostic for `span`:
//
//   file.c:3:12: error: 'x' undeclared
//       3 |     return x;
//         |            ^
static void vdiagnostic(DiagKind kind, const SourceSpan *span, const char *format, va_list ap)
{
    // Find the beginning and end of the line containing `span->start`.
    const char *line_start = span->start;
    while (line_start > span->file->contents && !is_line_break(line_start[-1]))
        line_start--;

    const char *line_end = span->start;
    while (*line_end && !is_line_break(*line_end))
        line_end++;

    int indent = (int)span->column - 1;

    // file:line:column: kind: message
    fprintf(stderr, "%s:%u:%u: %s: ", span->file->name, span->line, span->column, diag_name(kind));
    vfprintf(stderr, format, ap);
    fprintf(stderr, "\n");

    // Source line with the line-number gutter.
    fprintf(stderr, "%*u | ", GUTTER, span->line);
    print_source_line(line_start, line_end - line_start);
    fputc('\n', stderr);

    // Caret line. Underline the whole span, clamped to the line end.
    int length = span->length;
    if (length < 1)
        length = 1;
    if (span->start + length > line_end)
        length = line_end - span->start;

    int caret = display_width(span->start, length);
    if (caret < 1)
        caret = 1;

    fprintf(stderr, "%*s | %*s^", GUTTER, "", indent, "");
    for (int i = 1; i < caret; i++)
        fputc('~', stderr);
    fputc('\n', stderr);
}

bool had_error(void)
{
    return g_errors > 0;
}
void add_error(int count)
{
    g_errors += count;
}

void error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    add_error(1);
    fprintf(stderr, "error: ");
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void warn(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "warning: ");
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void error_at(const SourceSpan *span, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    add_error(1);
    vdiagnostic(DIAG_ERROR, span, format, args);
    va_end(args);
}

void warn_at(const SourceSpan *span, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vdiagnostic(DIAG_WARNING, span, format, args);
    va_end(args);
}

void error_span(const char *start, const char *end, const char *format, ...)
{
    SourceSpan span = source_span(start, end);

    va_list args;
    va_start(args, format);
    add_error(1);
    vdiagnostic(DIAG_ERROR, &span, format, args);
    va_end(args);
}

void warn_span(const char *start, const char *end, const char *format, ...)
{
    SourceSpan span = source_span(start, end);

    va_list args;
    va_start(args, format);
    vdiagnostic(DIAG_WARNING, &span, format, args);
    va_end(args);
}

#ifdef TEST_DIAGNOSTIC

#include <string.h>

int main(void)
{
    char src[] = "int main()\n"
                 "{\n"
                 "    return x;\n"
                 "}\n";

    SourceFile file = { .name = "test.c", .contents = src };
    set_current_source(&file);

    char *p1 = strstr(src, "x;");
    error_span(p1, p1 + 1, "'x' undeclared (first use in this function)");

    char src2[] = "void f()\n"
                  "{\n"
                  "\tint y = 1;\n"
                  "\treturn;\n"
                  "}\n";

    SourceFile file2 = { .name = "test2.c", .contents = src2 };
    set_current_source(&file2);

    char *p2 = strstr(src2, "return;");
    error_span(p2, p2 + 1, "expected expression before ';' token");

    // A pre-resolved span (as a token would carry) uses the span-based API.
    char *p3 = strstr(src2, "int");
    SourceSpan ts = source_span(p3, p3 + 3);
    warn_at(&ts, "unused variable 'y'");

    error_at(&ts, "redefinition of 'y'");

    // UTF-8: columns are counted in display width, not bytes.
    char src3[] = "int main(void)\n"
                  "{\n"
                  "    char *s = \"\xc3\xa9\"; @\n"
                  "}\n";

    SourceFile file3 = { .name = "test3.c", .contents = src3 };
    set_current_source(&file3);

    char *p4 = strstr(src3, "@");
    error_span(p4, p4 + 1, "stray '@' in program");

    warn("implicit declaration of function 'foo'");
    error("something went wrong");

    return 0;
}

#endif
