// The driver runs source loading, tokenization, parsing and semantic analysis.
// The exit status follows error_count().

#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/sema.h"
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

    token_free(tokens);
    SemaResult *result = NULL;
    if (root && !error_count())
        sema_analyze(root, &result);
    sema_free(result);
    parse_free(root);
    source_free(current_source());

    return error_count() > 0 ? 1 : 0;
}
