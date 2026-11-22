// The driver runs the front end: load -> tokenize -> parse, then releases the
// AST; later phases plug into the same pipeline. The exit status is decided by
// error_count(), not by the driver.

#include "frontend/lexer.h"
#include "frontend/parser.h"
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

    Node *root = parse(tokens);

    parse_free(root);
    token_free(tokens);
    source_free(current_source());

    return error_count() > 0 ? 1 : 0;
}
