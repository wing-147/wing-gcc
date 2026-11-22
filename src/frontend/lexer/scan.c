// Token-boundary scanning owns source traversal and recovery that advances past
// malformed lexical units; literal interpretation happens only after spans are located.

#include "lexer_internal.h"

#include "config.h"
#include "util/common.h"
#include "util/diagnostic.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

// Check whether str starts with the given prefix.
static inline bool startswith(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

// Create a token from the source range [start, end).
// The token keeps a reference to the original source buffer.
static Token *new_token(TokenKind kind, const char *start, const char *end)
{
    Token *tok = xcalloc(1, sizeof(Token));
    tok->kind = kind;
    tok->span.text = string_view_from_range(start, end);
    return tok;
}

// Return the byte length of the UTF-8 sequence beginning at `p`, so that an
// unrecognized character is reported and skipped as a whole. The length is
// clamped to the terminating NUL and to well-formed continuation bytes, so
// the result never points past the end of the buffer.
static size_t utf8_len(const char *p)
{
    unsigned char c = (unsigned char)p[0];
    size_t n;

    if (c < 0x80 || (c & 0xc0) == 0x80)
        n = 1; // ASCII or a stray continuation byte
    else if ((c & 0xe0) == 0xc0)
        n = 2;
    else if ((c & 0xf0) == 0xe0)
        n = 3;
    else if ((c & 0xf8) == 0xf0)
        n = 4;
    else
        n = 1;

    for (size_t i = 1; i < n; i++) {
        if (p[i] == '\0' || ((unsigned char)p[i] & 0xc0) != 0x80)
            return i; // truncated/invalid: stop at the last complete byte
    }

    return n;
}

// Check whether the current position starts a string literal.
static bool is_string_literal(const char *p)
{
    return *p == '"' || startswith(p, "u8\"") || startswith(p, "u\"") || startswith(p, "U\"") ||
           startswith(p, "L\"");
}

// Check whether the current position starts a character literal.
static bool is_char_literal(const char *p)
{
    return *p == '\'' || startswith(p, "u'") || startswith(p, "U'") || startswith(p, "L'");
}

// Scan a numeric literal.
// This function only determines the token boundary; token_num() validates and
// decodes it later. '_' belongs to a pp-number, which keeps spellings like
// `1_000` and `2._` in one token so the validator can reject them like GCC.
static Token *lex_number(LexerCursor *cursor)
{
    const char *start = cursor->position;
    const char *p = start;
    while (true) {
        // Consume exponent signs such as e+10 or p-2.
        if (p[0] && p[1] && strchr("eEpP", (unsigned char)p[0]) &&
            strchr("+-", (unsigned char)p[1])) {
            p += 2;
        }
        // Consume identifier-nondigits, digits and dots within the token.
        else if (isalnum((unsigned char)*p) || *p == '_' || *p == '.') {
            p++;
        } else {
            break;
        }
    }
    cursor->position = p;
    return new_token(TOK_NUMBER, start, p);
}

// Scan a string literal.
// This function only determines the token boundary; token_string() decodes the
// contents later.
static Token *lex_string(LexerCursor *cursor)
{
    const char *start = cursor->position;
    const char *p = start;

    if (startswith(p, "u8\""))
        p += 3;
    else if (startswith(p, "u\"") || startswith(p, "U\"") || startswith(p, "L\""))
        p += 2;
    else
        p++;

    while (p < cursor->limit && !is_line_break(*p)) {
        if (*p == '\\') {
            p++;
            if (p < cursor->limit && !is_line_break(*p)) {
                p++;
            }
        } else if (*p == '"') {
            p++;
            cursor->position = p;
            return new_token(TOK_STRING, start, p);
        } else {
            p++;
        }
    }

    // Recovery: report and stop at the newline (or EOF); the caller continues
    // lexing the following line. Underline the malformed literal like GCC.
    error_span(start, p, "missing terminating \" character");
    cursor->position = p;
    return NULL;
}

// Scan a character constant.
// This function only determines the token boundary; token_char() decodes the
// value later. An empty constant is reported here so its diagnostic keeps the
// source position GCC uses.
static Token *lex_char(LexerCursor *cursor)
{
    const char *start = cursor->position;
    const char *p = start;

    if (startswith(p, "u'") || startswith(p, "U'") || startswith(p, "L'"))
        p += 2;
    else
        p++;

    const char *content = p;

    while (p < cursor->limit && !is_line_break(*p)) {
        if (*p == '\\') {
            p++;
            if (p < cursor->limit && !is_line_break(*p))
                p++;
        } else if (*p == '\'') {
            if (p == content)
                error_span(start, p + 1, "empty character constant");
            p++;
            cursor->position = p;
            return new_token(TOK_CHARACTER, start, p);
        } else {
            p++;
        }
    }

    // Recovery: report and stop at the newline (or EOF). Underline the
    // malformed literal like GCC.
    error_span(start, p, "missing terminating ' character");
    cursor->position = p;
    return NULL;
}

// Scan an identifier or keyword token.
// The first character must already be validated by the caller.
// The scanned text is classified through token.c's exact keyword inventory.
static Token *lex_identifier(LexerCursor *cursor)
{
    const char *start = cursor->position;
    // '$' is accepted in identifiers only when GNU extensions are enabled.
    while (isalnum((unsigned char)*cursor->position) ||
           *cursor->position == '_'
#if GNU_EXTENSION
           || *cursor->position == '$'
#endif
    ) {
        cursor->position++;
    }
    TokenKind kind;
    StringView spelling = string_view_from_range(start, cursor->position);
    if (token_lookup_keyword(spelling, &kind))
        return new_token(kind, start, cursor->position);

    return new_token(TOK_IDENTIFIER, start, cursor->position);
}

// Render an unrecognized byte the way GCC does: printable ASCII as itself,
// anything else as the octal value of the first byte.
static void report_stray(const char *start)
{
    unsigned char c = (unsigned char)*start;

    if (c >= 0x20 && c < 0x7f)
        error_span(start, start + 1, "stray '%c' in program", (int)c);
    else
        error_span(start, start + 1, "stray '\\%o' in program", (unsigned)c);
}

// Scan a punctuation token and advance the current position.
// On an unrecognized character, reports it, skips one code point and returns
// NULL so that lexing can continue.
static Token *lex_punct(LexerCursor *cursor)
{
    const char *start = cursor->position;
    TokenKind kind;
    size_t len = token_lookup_punctuator(start, &kind);

    if (len == 0) {
        size_t n = utf8_len(start);
        report_stray(start);
        cursor->position = start + n;
        return NULL;
    }

    cursor->position += len;
    return new_token(kind, start, cursor->position);
}

// Scan source bytes into an ordered token chain with borrowed spelling ranges.
// Diagnostics consume malformed units and the returned chain always ends in EOF;
// metadata and literal payloads remain owned by the pipeline coordinator.
Token *scan_tokens(const char *contents, size_t length)
{
    LexerCursor cursor = {
        .position = contents,
        .limit = contents + length,
    };

    Token head = { 0 };
    Token *curr = &head;

    while (cursor.position < cursor.limit) {
        Token *tok = NULL;
        const char *before = cursor.position;

        // GCC ignores NUL bytes embedded in the file; skip them so lexing
        // continues past them.
        if (*cursor.position == '\0') {
            cursor.position++;
            continue;
        }
        // skip line comments (a lone CR also ends the comment)
        if (startswith(cursor.position, "//")) {
            cursor.position += 2;
            while (cursor.position < cursor.limit &&
                   !is_line_break(*cursor.position))
                cursor.position++;
            continue;
        }
        // skip block comments; a NUL inside is data, so match the terminator
        // byte-wise instead of with strstr()
        else if (startswith(cursor.position, "/*")) {
            const char *p = cursor.position + 2;
            while (p + 1 < cursor.limit && !(p[0] == '*' && p[1] == '/'))
                p++;
            if (p + 1 >= cursor.limit) {
                // Recovery: unterminated comment consumes the rest of the file.
                error_span(cursor.position, cursor.position + 1,
                           "unterminated comment");
                cursor.position = cursor.limit;
                break;
            }
            cursor.position = p + 2;
            continue;
        }
        //  skip possible space
        else if (isspace((unsigned char)*cursor.position)) {
            cursor.position++;
            continue;
        }
        //  Numeric literal (a pp-number may also start with '.' + digit)
        else if (isdigit((unsigned char)*cursor.position) ||
                 (*cursor.position == '.' &&
                  isdigit((unsigned char)cursor.position[1]))) {
            tok = lex_number(&cursor);
        }
        // String literal
        else if (is_string_literal(cursor.position)) {
            tok = lex_string(&cursor);
        }
        // Character literal
        else if (is_char_literal(cursor.position)) {
            tok = lex_char(&cursor);
        }
        // Identifier or keyword ('$' is a GCC extension in identifiers)
        else if (isalpha((unsigned char)*cursor.position) ||
                 *cursor.position == '_'
#if GNU_EXTENSION
                 || *cursor.position == '$'
#endif
        ) {
            tok = lex_identifier(&cursor);
        }
        // Punctuators
        else {
            tok = lex_punct(&cursor);
        }

        // Guard the recovery invariant: never re-lex the same position.
        if (cursor.position == before) {
            unreachable();
        }

        // A NULL token means a lexical error was already reported and the
        // pointer was advanced; keep going.
        if (!tok)
            continue;

        curr->next = tok;
        curr = tok;
    }

    // Add EOF token at the end of token stream
    Token *eof = new_token(TOK_EOF, cursor.position, cursor.position);
    curr->next = eof;
    return head.next;
}
