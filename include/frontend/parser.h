#ifndef PARSER_H
#define PARSER_H

#include "frontend/ast.h"

// Parse a `tokenize_file()` token list (which must end with TOK_EOF).
// The list must belong to the current source file (`tokenize_file()` guarantees
// this; diagnostics are resolved against `current_source()`).
// Recoverable syntax errors are reported and parsing continues, so the returned
// tree may be partial; NULL is only returned for a NULL token list. Check
// error_count() for validity. The caller keeps ownership of the token list.
Node *parse(Token *tokens);

// Release a tree returned by parse() (safe with NULL). Node spans point into the
// source buffer, so release the tree before source_free().
void parse_free(Node *tree);

#endif // PARSER_H
