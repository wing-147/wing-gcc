#ifndef TOKEN_H
#define TOKEN_H

#include "util/source.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Exact lexical terminals shared by lexer and parser. Fixed-spelling C11/GNU
// keywords and punctuators have distinct kinds, so grammar code never combines
// a broad lexical category with a runtime spelling string.
typedef enum {
    TOK_EOF,
    TOK_IDENTIFIER,
    TOK_NUMBER,
    TOK_CHARACTER,
    TOK_STRING,

    TOK_KW_RETURN,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_FOR,
    TOK_KW_WHILE,
    TOK_KW_INT,
    TOK_KW_SIZEOF,
    TOK_KW_CHAR,
    TOK_KW_STRUCT,
    TOK_KW_UNION,
    TOK_KW_SHORT,
    TOK_KW_LONG,
    TOK_KW_VOID,
    TOK_KW_TYPEDEF,
    TOK_KW_BOOL,
    TOK_KW_ENUM,
    TOK_KW_STATIC,
    TOK_KW_GOTO,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,
    TOK_KW_SWITCH,
    TOK_KW_CASE,
    TOK_KW_DEFAULT,
    TOK_KW_EXTERN,
    TOK_KW_ALIGNOF,
    TOK_KW_ALIGNAS,
    TOK_KW_DO,
    TOK_KW_SIGNED,
    TOK_KW_UNSIGNED,
    TOK_KW_CONST,
    TOK_KW_VOLATILE,
    TOK_KW_AUTO,
    TOK_KW_REGISTER,
    TOK_KW_RESTRICT,
    TOK_KW_NORETURN,
    TOK_KW_FLOAT,
    TOK_KW_DOUBLE,
    TOK_KW_THREAD_LOCAL,
    TOK_KW_ATOMIC,
    TOK_KW_INLINE,
    TOK_KW_STATIC_ASSERT,
    TOK_KW_GENERIC,
    TOK_KW_COMPLEX,
    TOK_KW_IMAGINARY,

    TOK_KW_TYPEOF,
    TOK_KW_ASM,
    TOK_KW_ATTRIBUTE,
    TOK_KW_THREAD,
    TOK_KW_RESTRICT_GNU,
    TOK_KW_RESTRICT_GNU_ALT,
    TOK_KW_ASM_GNU,
    TOK_KW_INLINE_GNU,
    TOK_KW_TYPEOF_GNU,
    TOK_KW_VOLATILE_GNU,
    TOK_KW_CONST_GNU,
    TOK_KW_SIGNED_GNU,
    TOK_KW_ALIGNOF_GNU,

    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_DOT,
    TOK_ARROW,
    TOK_INCREMENT,
    TOK_DECREMENT,
    TOK_AMPERSAND,
    TOK_STAR,
    TOK_PLUS,
    TOK_MINUS,
    TOK_TILDE,
    TOK_BANG,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_SHIFT_LEFT,
    TOK_SHIFT_RIGHT,
    TOK_LESS,
    TOK_GREATER,
    TOK_LESS_EQUAL,
    TOK_GREATER_EQUAL,
    TOK_EQUAL_EQUAL,
    TOK_NOT_EQUAL,
    TOK_CARET,
    TOK_PIPE,
    TOK_LOGICAL_AND,
    TOK_LOGICAL_OR,
    TOK_QUESTION,
    TOK_COLON,
    TOK_SEMICOLON,
    TOK_ELLIPSIS,
    TOK_ASSIGN,
    TOK_MUL_ASSIGN,
    TOK_DIV_ASSIGN,
    TOK_MOD_ASSIGN,
    TOK_ADD_ASSIGN,
    TOK_SUB_ASSIGN,
    TOK_SHIFT_LEFT_ASSIGN,
    TOK_SHIFT_RIGHT_ASSIGN,
    TOK_AND_ASSIGN,
    TOK_XOR_ASSIGN,
    TOK_OR_ASSIGN,
    TOK_COMMA,
    TOK_HASH,
    TOK_HASH_HASH,

    TOK_KIND_COUNT,
} TokenKind;

// Scalar type and value selected while decoding a numeric or character token.
// The AST copies this completed lexical payload without reinterpreting it.
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

typedef union {
    int64_t integer;
    long double floating;
} LiteralValue;

typedef struct Token Token;

struct Token {
    TokenKind kind;

    SourceSpan span;

    LiteralValue value;
    char *str;        // TOK_STRING contents (heap, NUL-terminated).
    int str_len;      // Decoded byte length (may contain embedded NULs).
    LiteralType literal; // Constant type of a number or character token.

    Token *next;
};

// Return the stable diagnostic/debug name of every token kind.
const char *token_kind_name(TokenKind kind);

// Classify punctuators for diagnostic presentation.
bool token_kind_is_punctuator(TokenKind kind);

#endif // TOKEN_H
