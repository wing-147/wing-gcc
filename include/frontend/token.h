#ifndef TOKEN_H
#define TOKEN_H

#include "util/source.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TOKEN_KEYWORD,
    TOKEN_STRING,
    TOKEN_IDENT,
    TOKEN_NUMBER,
    TOKEN_CHAR,
    TOKEN_PUNCT,
    TOKEN_EOF,
} TokenType;

// Type of a literal constant, mirroring the C constant kinds. Kept as a label
// (not a full Type) so the token layer stays independent of the parser's type
// system; the parser maps it once types exist.
typedef enum {
    LITERAL_INT,
    LITERAL_UINT,
    LITERAL_LONG,
    LITERAL_ULONG,
    LITERAL_LLONG,
    LITERAL_ULLONG,
    LITERAL_FLOAT,
    LITERAL_DOUBLE,
    LITERAL_LDOUBLE,
} LiteralType;

typedef struct Token Token;

struct Token {
    TokenType type;

    SourceSpan span;

    int64_t val;         // Integer/character value; unsigned keeps its bit pattern.
    long double fval;    // Floating value.
    char *str;           // TOKEN_STRING contents (heap, NUL-terminated).
    int str_len;         // Decoded byte length (may contain embedded NULs).
    LiteralType literal; // Constant type of a number or character token.

    Token *next;
};

static inline bool literal_is_floating(LiteralType type)
{
    return type >= LITERAL_FLOAT;
}

#endif // TOKEN_H
