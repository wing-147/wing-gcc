// The driver deliberately stops at lexing: later phases plug into the same
// load -> tokenize -> release pipeline, and the exit status is decided by
// had_error() rather than by the driver.

#include "frontend/lexer.h"
#include "frontend/token.h"
#include "util/diagnostic.h"
#include "util/source.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <source-file>\n", argv[0]);
        return 1;
    }

    Token *tokens = tokenize_file(argv[1]);
    if (!tokens) {
        fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    token_free(tokens);
    source_free(current_source());

    return had_error() ? 1 : 0;
}
