#ifndef LEXER_INTERNAL_H
#define LEXER_INTERNAL_H

#include "frontend/lexer.h"
#include "frontend/token.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *position;
    const char *limit;
} LexerCursor;

// Scan source bytes into an EOF-terminated chain whose spellings borrow the
// source buffer; the caller completes metadata and literal payloads.
Token *scan_tokens(const char *contents, size_t length);

// Decode literal payloads in place after every token has a located SourceSpan.
// Diagnostics leave the chain structurally valid so later errors remain visible.
void decode_token_literals(Token *tokens);

// Apply C11 translation phase 6 to decoded adjacent string-literal tokens.
void concatenate_string_literals(Token *tokens);

// Lexer-private views of the single fixed-terminal inventory in token.c.
const char *token_kind_spelling(TokenKind kind);
bool token_kind_is_keyword(TokenKind kind);
bool token_lookup_keyword(StringView spelling, TokenKind *out);
size_t token_lookup_punctuator(const char *text, TokenKind *out);

#endif // LEXER_INTERNAL_H
