// The public lexer pipeline never exposes half-initialized tokens: scanning
// establishes spelling boundaries, metadata locates them, then literal decoding
// completes the same owned chain before it crosses the lexer boundary.

#include "frontend/lexer.h"

#include "lexer_internal.h"
#include "util/common.h"
#include "util/source.h"

// Attach the borrowed source owner to every token without changing spelling or
// chain ownership.
static void attach_token_file(Token *tokens, SourceFile *source)
{
    for (Token *token = tokens; token; token = token->next)
        token->span.file = source;
}

// Resolve token starts in one forward source walk. scan_tokens() guarantees
// ordered spelling ranges, including the zero-width EOF range.
static void attach_token_positions(Token *tokens, const SourceFile *source)
{
    const char *position = source->contents;
    unsigned line = 1;
    int width = 0;

    for (Token *token = tokens; token; token = token->next) {
        if (string_view_begin(token->span.text) < position)
            unreachable();
        advance_position(position, string_view_begin(token->span.text),
                         &line, &width);
        position = string_view_begin(token->span.text);
        token->span.line = line;
        token->span.column = (unsigned)width + 1;
    }
}

// Complete the location phase as one information-only operation before any
// decoder emits a diagnostic anchored to a token.
static void attach_token_metadata(Token *tokens, SourceFile *source)
{
    attach_token_file(tokens, source);
    attach_token_positions(tokens, source);
}

// Lex one borrowed source into a fully located and decoded token chain. The
// caller retains the source; the returned chain owns only tokens and decoded
// string buffers.
Token *tokenize(SourceFile *source)
{
    set_current_source(source);
    Token *tokens = scan_tokens(source->contents, source->size);
    attach_token_metadata(tokens, source);
    decode_token_literals(tokens);
    concatenate_string_literals(tokens);
    return tokens;
}

// Load and tokenize one file while preserving errno on load failure. On
// success current_source() owns the loaded SourceFile until the driver frees it.
Token *tokenize_file(const char *path)
{
    SourceFile *source = source_load(path);
    if (!source)
        return NULL;
    return tokenize(source);
}
