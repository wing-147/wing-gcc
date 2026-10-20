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

// Resolve the byte range [start, end) of the current source into a SourceSpan
// with 1-based line and display column. This is location analysis, kept out of
// the diagnostic output layer.
SourceSpan source_span(const char *start, const char *end)
{
    SourceFile *file = current_source();
    if (!file)
        unreachable();

    SourceSpan span = { .file = file, .start = start, .length = (int)(end - start) };

    unsigned line = 1;
    int width = 0;
    advance_position(file->contents, start, &line, &width);

    span.line = line;
    span.column = (unsigned)width + 1;
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
