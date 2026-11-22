// Location analysis lives in source_span()/source.h so that diagnostics stay
// pure rendering; the current file is reachable only through the accessors so
// its storage (global, thread-local, context) can change without touching callers.

#include "util/source.h"
#include "util/common.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// The single backing store for the current source file. Kept private so its
// representation (plain global, thread-local, or context-scoped) can change
// without touching callers.
static SourceFile *g_current_file = NULL;

// Load `filename` into a newly allocated SourceFile.
// On success, `name` and `contents` are heap-owned (NUL-terminated) and the
// caller must release the result with source_free(). I/O failures return NULL
// with errno set to the cause; allocation failures are fatal (see common.h). This
// function has no side effects on global state.
SourceFile *source_load(const char *filename)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp)
        return NULL;

    struct stat st;
    if (fstat(fileno(fp), &st) != 0) {
        int saved = errno;
        fclose(fp);
        errno = saved;
        return NULL;
    }

    // Only regular files have a reliable size; reject anything else.
    if (st.st_size < 0 || !S_ISREG(st.st_mode)) {
        fclose(fp);
        errno = EINVAL;
        return NULL;
    }

    size_t size = (size_t)st.st_size;
    char *contents = xmalloc(size + 1);

    size_t nread = 0;
    while (nread < size) {
        size_t n = fread(contents + nread, 1, size - nread, fp);
        if (n == 0) {
            if (ferror(fp) && errno == EINTR) {
                clearerr(fp); // clear the sticky error indicator before retrying
                continue;
            }
            break; // short read or I/O error
        }
        nread += n;
    }

    if (nread != size || ferror(fp)) {
        // A short read at EOF has no meaningful errno; report EIO instead of a
        // stale value from an earlier call.
        int saved = ferror(fp) ? errno : EIO;
        free(contents);
        fclose(fp);
        errno = saved ? saved : EIO;
        return NULL;
    }

    fclose(fp);

    // GCC ignores a UTF-8 BOM at the start of a file; strip it so it stays
    // invisible to lexing and column counting.
    if (size >= 3 && (unsigned char)contents[0] == 0xef && (unsigned char)contents[1] == 0xbb &&
        (unsigned char)contents[2] == 0xbf) {
        memmove(contents, contents + 3, size - 3);
        size -= 3;
    }
    contents[size] = '\0';

    SourceFile *file = xmalloc(sizeof(SourceFile));
    file->name = xstrdup(filename);
    file->contents = contents;
    file->size = size;
    return file;
}

// Release a SourceFile previously returned by source_load().
// Safe to call with NULL.
void source_free(SourceFile *file)
{
    if (!file)
        return;

    if (file == current_source())
        set_current_source(NULL);

    free(file->name);
    free(file->contents);
    free(file);
}

// Resolve a byte range against an explicit file. Keeping the file explicit is
// what lets source_span_join() work after current_source() has moved on.
static SourceSpan span_in_file(SourceFile *file, const char *start, const char *end)
{
    if (!file)
        unreachable();

    SourceSpan span = {
        .file = file,
        .text = string_view(start, (size_t)(end - start)),
    };

    unsigned line = 1;
    int width = 0;
    advance_position(file->contents, start, &line, &width);

    span.line = line;
    span.column = (unsigned)width + 1;
    return span;
}

// Resolve the byte range [start, end) of the current source into a SourceSpan
// with 1-based line and display column. This is location analysis, kept out of
// the diagnostic output layer.
SourceSpan source_span(const char *start, const char *end)
{
    return span_in_file(current_source(), start, end);
}

// Join two spans of the same file. Line/column are resolved against the spans'
// own file, so a span may outlive its role as current_source() (tests rejoin
// spans after switching files). Both spans must carry the same file; mixing
// files or missing metadata is an internal contract violation.
SourceSpan source_span_join(const SourceSpan *a, const SourceSpan *b)
{
    if (!a->file || a->file != b->file)
        unreachable();

    const SourceSpan *first =
        string_view_begin(a->text) <= string_view_begin(b->text) ? a : b;
    const char *start = string_view_begin(first->text);
    const char *a_end = string_view_end(a->text);
    const char *b_end = string_view_end(b->text);
    const char *end = a_end > b_end ? a_end : b_end;

    SourceSpan span = *first;
    span.text = string_view(start, (size_t)(end - start));
    return span;
}

SourceFile *current_source(void)
{
    return g_current_file;
}

void set_current_source(SourceFile *file)
{
    g_current_file = file;
}

#ifdef TEST_SOURCE

#include <unistd.h>

static int failures = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s\n", msg);
        failures++;
    }
}

static void write_file(const char *path, const char *contents, size_t len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        exit(1);
    }
    fwrite(contents, 1, len, fp);
    fclose(fp);
}

int main(void)
{
    char view_bytes[] = { 'a', '\0', 'b' };
    StringView view = string_view_from_range(view_bytes, view_bytes + sizeof(view_bytes));
    check(string_view_begin(view) == view_bytes, "StringView begin preserves the range start");
    check(string_view_end(view) == view_bytes + sizeof(view_bytes),
          "StringView end preserves the range end");
    check(string_view_length(view) == sizeof(view_bytes),
          "StringView length includes embedded NUL bytes");
    check(string_view_end((StringView){ 0 }) == NULL,
          "empty null StringView has a null end");

    const char *tmp = "/tmp/source_test.txt";
    const char *text = "int main()\n{\n    return 0;\n}\n";
    size_t len = strlen(text);

    // Valid file.
    write_file(tmp, text, len);
    SourceFile *file = source_load(tmp);
    check(file != NULL, "load existing file returns non-NULL");
    check(file && strcmp(file->name, tmp) == 0, "name is preserved");
    check(file && strcmp(file->contents, text) == 0, "contents match");
    check(file && file->size == len, "size matches the file length");
    check(file && file->contents[len] == '\0', "contents NUL-terminated at size");
    source_free(file);

    // A UTF-8 BOM at the start of the file is stripped.
    const char bom_text[] = "\xef\xbb\xbfint x;\n";
    size_t bom_len = sizeof(bom_text) - 1;
    write_file(tmp, bom_text, bom_len);
    file = source_load(tmp);
    check(file != NULL, "load BOM file returns non-NULL");
    check(file && strcmp(file->contents, "int x;\n") == 0, "BOM is stripped");
    check(file && file->size == bom_len - 3, "size excludes the stripped BOM");
    source_free(file);

    // Empty file.
    write_file(tmp, "", 0);
    file = source_load(tmp);
    check(file != NULL, "load empty file returns non-NULL");
    check(file && file->contents[0] == '\0', "empty file yields empty string");
    source_free(file);

    // source_span_join() combines spans of the current source.
    const char spans_text[] = "int x;\nint y;\n";
    write_file(tmp, spans_text, sizeof(spans_text) - 1);
    file = source_load(tmp);
    set_current_source(file);
    const char *base = file->contents;
    SourceSpan sx = source_span(base + 4, base + 5);   // 'x', 1:5
    SourceSpan sy = source_span(base + 11, base + 12); // 'y', 2:5
    SourceSpan joined = source_span_join(&sx, &sy);
    check(string_view_begin(joined.text) == string_view_begin(sx.text),
          "join starts at the earlier span");
    check(string_view_length(joined.text) ==
              (size_t)(string_view_end(sy.text) - string_view_begin(sx.text)),
          "join covers both spans");
    check(joined.line == 1 && joined.column == 5, "join keeps the earlier position");
    SourceSpan reversed = source_span_join(&sy, &sx);
    check(string_view_begin(reversed.text) == string_view_begin(joined.text) &&
              string_view_length(reversed.text) == string_view_length(joined.text) &&
              reversed.line == joined.line && reversed.column == joined.column,
          "join is order-independent");
    SourceSpan same = source_span_join(&sx, &sx);
    check(string_view_equal(same.text, sx.text),
          "join of a span with itself");
    SourceSpan empty = source_span(base + 7, base + 7); // zero-length span after 'x;\n'
    SourceSpan with_empty = source_span_join(&empty, &sx);
    check(string_view_begin(with_empty.text) == string_view_begin(sx.text) &&
              string_view_length(with_empty.text) == 3,
          "join with an empty span");
    SourceSpan semi = source_span(base + 5, base + 6); // ';' adjacent to 'x'
    SourceSpan adjacent = source_span_join(&sx, &semi);
    check(string_view_begin(adjacent.text) == string_view_begin(sx.text) &&
              string_view_length(adjacent.text) == 2,
          "join of adjacent spans");

    // Spans merge even after current_source() has moved to another file.
    const char other_text[] = "int z;\n";
    write_file("/tmp/source_test2.txt", other_text, sizeof(other_text) - 1);
    SourceFile *other = source_load("/tmp/source_test2.txt");
    set_current_source(other);
    SourceSpan off_current = source_span_join(&sx, &sy);
    check(string_view_equal(off_current.text, joined.text) &&
              off_current.line == joined.line && off_current.column == joined.column,
          "join resolves against the span's own file");
    source_free(other);
    source_free(file);

    // Missing file.
    errno = 0;
    file = source_load("/tmp/definitely_missing_file.txt");
    check(file == NULL, "missing file returns NULL");
    check(errno != 0, "errno is set on failure");

    // source_free(NULL) is a no-op.
    source_free(NULL);
    check(1, "source_free(NULL) is a no-op");

    unlink(tmp);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all source tests passed\n");
    return 0;
}

#endif
