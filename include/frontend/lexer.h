#ifndef LEXER_H
#define LEXER_H

#include "frontend/token.h"

// Lex `length` bytes of `contents` into a token list (metadata and literal
// values not yet attached). `contents`/`length` should come from
// current_source() so diagnostics can locate errors; `contents[length]` must
// be readable and NUL-terminated, because lookahead may inspect it. NUL bytes
// inside the file are skipped, matching GCC.
Token *tokenize(const char *contents, size_t length);

// Load, lex, attach file/line/column metadata and decode literal values for
// `path`. Returns the token list, or NULL on I/O failure with errno preserved.
// On success the loaded file becomes current_source(); the caller releases the
// tokens with token_free() and the file with source_free(current_source()).
Token *tokenize_file(const char *path);

// Release a token list produced by tokenize()/tokenize_file(). Temporary API;
// to be removed once the parser owns token lifetime (see TODO.md).
void token_free(Token *tok);

#endif // LEXER_H
