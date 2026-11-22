// Exact token metadata shared by lexer classification, parser matching and
// diagnostics. Every fixed spelling has one authoritative TokenKind entry.

#include "frontend/token.h"

#include "lexer_internal.h"

#include "config.h"
#include "util/common.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *spelling;
    const char *name;
} TokenInfo;

static const TokenInfo g_token_info[TOK_KIND_COUNT] = {
    [TOK_EOF] = { NULL, "EOF" },
    [TOK_IDENTIFIER] = { NULL, "identifier" },
    [TOK_NUMBER] = { NULL, "number" },
    [TOK_CHARACTER] = { NULL, "character constant" },
    [TOK_STRING] = { NULL, "string literal" },

    [TOK_KW_RETURN] = { "return", "return" },
    [TOK_KW_IF] = { "if", "if" },
    [TOK_KW_ELSE] = { "else", "else" },
    [TOK_KW_FOR] = { "for", "for" },
    [TOK_KW_WHILE] = { "while", "while" },
    [TOK_KW_INT] = { "int", "int" },
    [TOK_KW_SIZEOF] = { "sizeof", "sizeof" },
    [TOK_KW_CHAR] = { "char", "char" },
    [TOK_KW_STRUCT] = { "struct", "struct" },
    [TOK_KW_UNION] = { "union", "union" },
    [TOK_KW_SHORT] = { "short", "short" },
    [TOK_KW_LONG] = { "long", "long" },
    [TOK_KW_VOID] = { "void", "void" },
    [TOK_KW_TYPEDEF] = { "typedef", "typedef" },
    [TOK_KW_BOOL] = { "_Bool", "_Bool" },
    [TOK_KW_ENUM] = { "enum", "enum" },
    [TOK_KW_STATIC] = { "static", "static" },
    [TOK_KW_GOTO] = { "goto", "goto" },
    [TOK_KW_BREAK] = { "break", "break" },
    [TOK_KW_CONTINUE] = { "continue", "continue" },
    [TOK_KW_SWITCH] = { "switch", "switch" },
    [TOK_KW_CASE] = { "case", "case" },
    [TOK_KW_DEFAULT] = { "default", "default" },
    [TOK_KW_EXTERN] = { "extern", "extern" },
    [TOK_KW_ALIGNOF] = { "_Alignof", "_Alignof" },
    [TOK_KW_ALIGNAS] = { "_Alignas", "_Alignas" },
    [TOK_KW_DO] = { "do", "do" },
    [TOK_KW_SIGNED] = { "signed", "signed" },
    [TOK_KW_UNSIGNED] = { "unsigned", "unsigned" },
    [TOK_KW_CONST] = { "const", "const" },
    [TOK_KW_VOLATILE] = { "volatile", "volatile" },
    [TOK_KW_AUTO] = { "auto", "auto" },
    [TOK_KW_REGISTER] = { "register", "register" },
    [TOK_KW_RESTRICT] = { "restrict", "restrict" },
    [TOK_KW_NORETURN] = { "_Noreturn", "_Noreturn" },
    [TOK_KW_FLOAT] = { "float", "float" },
    [TOK_KW_DOUBLE] = { "double", "double" },
    [TOK_KW_THREAD_LOCAL] = { "_Thread_local", "_Thread_local" },
    [TOK_KW_ATOMIC] = { "_Atomic", "_Atomic" },
    [TOK_KW_INLINE] = { "inline", "inline" },
    [TOK_KW_STATIC_ASSERT] = { "_Static_assert", "_Static_assert" },
    [TOK_KW_GENERIC] = { "_Generic", "_Generic" },
    [TOK_KW_COMPLEX] = { "_Complex", "_Complex" },
    [TOK_KW_IMAGINARY] = { "_Imaginary", "_Imaginary" },

    [TOK_KW_TYPEOF] = { "typeof", "typeof" },
    [TOK_KW_ASM] = { "asm", "asm" },
    [TOK_KW_ATTRIBUTE] = { "__attribute__", "__attribute__" },
    [TOK_KW_THREAD] = { "__thread", "__thread" },
    [TOK_KW_RESTRICT_GNU] = { "__restrict", "__restrict" },
    [TOK_KW_RESTRICT_GNU_ALT] = { "__restrict__", "__restrict__" },
    [TOK_KW_ASM_GNU] = { "__asm__", "__asm__" },
    [TOK_KW_INLINE_GNU] = { "__inline__", "__inline__" },
    [TOK_KW_TYPEOF_GNU] = { "__typeof__", "__typeof__" },
    [TOK_KW_VOLATILE_GNU] = { "__volatile__", "__volatile__" },
    [TOK_KW_CONST_GNU] = { "__const__", "__const__" },
    [TOK_KW_SIGNED_GNU] = { "__signed__", "__signed__" },
    [TOK_KW_ALIGNOF_GNU] = { "__alignof__", "__alignof__" },

    [TOK_LBRACKET] = { "[", "[" },
    [TOK_RBRACKET] = { "]", "]" },
    [TOK_LPAREN] = { "(", "(" },
    [TOK_RPAREN] = { ")", ")" },
    [TOK_LBRACE] = { "{", "{" },
    [TOK_RBRACE] = { "}", "}" },
    [TOK_DOT] = { ".", "." },
    [TOK_ARROW] = { "->", "->" },
    [TOK_INCREMENT] = { "++", "++" },
    [TOK_DECREMENT] = { "--", "--" },
    [TOK_AMPERSAND] = { "&", "&" },
    [TOK_STAR] = { "*", "*" },
    [TOK_PLUS] = { "+", "+" },
    [TOK_MINUS] = { "-", "-" },
    [TOK_TILDE] = { "~", "~" },
    [TOK_BANG] = { "!", "!" },
    [TOK_SLASH] = { "/", "/" },
    [TOK_PERCENT] = { "%", "%" },
    [TOK_SHIFT_LEFT] = { "<<", "<<" },
    [TOK_SHIFT_RIGHT] = { ">>", ">>" },
    [TOK_LESS] = { "<", "<" },
    [TOK_GREATER] = { ">", ">" },
    [TOK_LESS_EQUAL] = { "<=", "<=" },
    [TOK_GREATER_EQUAL] = { ">=", ">=" },
    [TOK_EQUAL_EQUAL] = { "==", "==" },
    [TOK_NOT_EQUAL] = { "!=", "!=" },
    [TOK_CARET] = { "^", "^" },
    [TOK_PIPE] = { "|", "|" },
    [TOK_LOGICAL_AND] = { "&&", "&&" },
    [TOK_LOGICAL_OR] = { "||", "||" },
    [TOK_QUESTION] = { "?", "?" },
    [TOK_COLON] = { ":", ":" },
    [TOK_SEMICOLON] = { ";", ";" },
    [TOK_ELLIPSIS] = { "...", "..." },
    [TOK_ASSIGN] = { "=", "=" },
    [TOK_MUL_ASSIGN] = { "*=", "*=" },
    [TOK_DIV_ASSIGN] = { "/=", "/=" },
    [TOK_MOD_ASSIGN] = { "%=", "%=" },
    [TOK_ADD_ASSIGN] = { "+=", "+=" },
    [TOK_SUB_ASSIGN] = { "-=", "-=" },
    [TOK_SHIFT_LEFT_ASSIGN] = { "<<=", "<<=" },
    [TOK_SHIFT_RIGHT_ASSIGN] = { ">>=", ">>=" },
    [TOK_AND_ASSIGN] = { "&=", "&=" },
    [TOK_XOR_ASSIGN] = { "^=", "^=" },
    [TOK_OR_ASSIGN] = { "|=", "|=" },
    [TOK_COMMA] = { ",", "," },
    [TOK_HASH] = { "#", "#" },
    [TOK_HASH_HASH] = { "##", "##" },
};

// Return the fixed source spelling for a classified keyword or punctuator;
// dynamic token kinds and out-of-range values have no spelling.
const char *token_kind_spelling(TokenKind kind)
{
    if (kind < 0 || kind >= TOK_KIND_COUNT)
        return NULL;
    return g_token_info[kind].spelling;
}

// Return the stable name used to present a token kind in diagnostics.
const char *token_kind_name(TokenKind kind)
{
    if (kind < 0 || kind >= TOK_KIND_COUNT)
        return "unknown token";
    return g_token_info[kind].name;
}

// Classify only keyword kinds enabled by the selected language mode.
bool token_kind_is_keyword(TokenKind kind)
{
    if (kind >= TOK_KW_RETURN && kind <= TOK_KW_IMAGINARY)
        return true;
#if GNU_EXTENSION
    return kind >= TOK_KW_TYPEOF && kind <= TOK_KW_ALIGNOF_GNU;
#else
    return false;
#endif
}

// Classify the contiguous punctuator range shared with parser diagnostics.
bool token_kind_is_punctuator(TokenKind kind)
{
    return kind >= TOK_LBRACKET && kind <= TOK_HASH_HASH;
}

// Resolve an exact spelling through the enabled keyword portion of the single
// token inventory without changing caller state on failure.
bool token_lookup_keyword(StringView spelling, TokenKind *out)
{
    TokenKind last = TOK_KW_IMAGINARY;
#if GNU_EXTENSION
    last = TOK_KW_ALIGNOF_GNU;
#endif
    for (TokenKind kind = TOK_KW_RETURN; kind <= last; kind++) {
        if (string_view_equal_cstr(spelling, g_token_info[kind].spelling)) {
            *out = kind;
            return true;
        }
    }
    return false;
}

// Select the longest punctuator prefix from the single token inventory;
// returns zero and leaves `out` unchanged when no punctuator matches.
size_t token_lookup_punctuator(const char *text, TokenKind *out)
{
    size_t longest = 0;
    for (TokenKind kind = TOK_LBRACKET; kind <= TOK_HASH_HASH; kind++) {
        const char *spelling = g_token_info[kind].spelling;
        size_t length = strlen(spelling);
        if (length > longest && strncmp(text, spelling, length) == 0) {
            longest = length;
            *out = kind;
        }
    }
    return longest;
}

// Release one complete lexer-owned token chain, including decoded string
// buffers not transferred into the AST. Safe with NULL.
void token_free(Token *tokens)
{
    while (tokens) {
        Token *next = tokens->next;
        free(tokens->str);
        free(tokens);
        tokens = next;
    }
}
