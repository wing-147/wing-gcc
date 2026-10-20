#ifndef SOURCE_H
#define SOURCE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct SourceFile {
    char *name;
    char *contents;
    size_t size; // byte length of contents (UTF-8 BOM stripped by source_load)
} SourceFile;

// A resolved region of a source file: start pointer + byte length, plus the
// 1-based start line and display column. Filled in phases:
//   tokenize           -> start, length
//   add_token_metadata -> file, line, column   (lexer.c, after lexing)
typedef struct {
    SourceFile *file;
    const char *start;
    int length; // byte length of the span; bounded by the file size
    unsigned int line;
    unsigned int column;
} SourceSpan;

// Display-width rules for source text, kept with source locations so that the
// column recorded on a token and the caret column printed by diagnostics agree.
// `width` is a 0-based display width; tabs advance to the next multiple of 8
// and UTF-8 continuation bytes contribute nothing. NUL bytes are ignored by the
// compiler and are zero-width.

static inline int display_width_step(int width, unsigned char c)
{
    if (c == '\0')
        return width; // NUL bytes are ignored, like GCC
    if (c == '\t')
        return width + 8 - width % 8;
    if ((c & 0xc0) == 0x80)
        return width; // UTF-8 continuation byte
    return width + 1;
}

// Display width of the first `len` bytes of `str`.
static inline int display_width(const char *str, int len)
{
    int width = 0;
    for (int i = 0; i < len; i++)
        width = display_width_step(width, (unsigned char)str[i]);
    return width;
}

// Line break rule shared by the lexer's scanning loops and advance_position().
static inline bool is_line_break(char c)
{
    return c == '\n' || c == '\r';
}

// Advance a position across the byte range [from, to), updating `*line` and
// `*width` (0-based display width within the current line). Both '\n' and a
// lone '\r' end a line, matching GCC; a "\r\n" pair counts as one newline.
static inline void advance_position(const char *from, const char *to, unsigned *line, int *width)
{
    for (const char *p = from; p < to; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r') {
            (*line)++;
            *width = 0;
        } else if (c == '\n') {
            if (p == from || p[-1] != '\r')
                (*line)++;
            *width = 0;
        } else {
            *width = display_width_step(*width, c);
        }
    }
}

SourceFile *source_load(const char *filename);
void source_free(SourceFile *file);

// Resolve the byte range [start, end) of the current source into a SourceSpan
// (line/column filled in). Location analysis lives here, not in the diagnostics
// module.
SourceSpan source_span(const char *start, const char *end);

// The file currently being processed. Only the driver should set it; read-only
// consumers (lexer, diagnostics) use current_source(). Keeping the storage
// behind accessors lets it later become thread-local or context-scoped without
// changing callers.
SourceFile *current_source(void);
void set_current_source(SourceFile *file);

#endif // SOURCE_H
