// Lexing runs in two passes with different contracts: tokenize() only cuts
// token boundaries (testable without any position information), while
// tokenize_file() attaches spans and decodes literal values, so every literal
// diagnostic can be reported against a located token.

#include "frontend/lexer.h"
#include "util/common.h"
#include "util/diagnostic.h"
#include "util/hashmap.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Literal decoding: token_num()/token_char()/token_string() run after
// tokenize() and add_token_metadata(), once every token has a located span.

typedef enum {
    NUMBER_OK,
    NUMBER_INVALID_SUFFIX_INTEGER,
    NUMBER_INVALID_SUFFIX_FLOATING,
    NUMBER_EXPONENT_HAS_NO_DIGITS,
    NUMBER_INVALID_DIGIT_OCTAL,
    NUMBER_INVALID_DIGIT_BINARY,
    NUMBER_TOO_MANY_DECIMAL_POINTS,
    NUMBER_HEX_FLOAT_REQUIRES_EXPONENT,
    NUMBER_HEX_FLOAT_NO_DIGITS,
    NUMBER_BINARY_FLOAT_PREFIX,
} NumberError;

// Result of the syntax pass over a pp-number; values are decoded only after the
// spelling is known to be valid.
typedef struct {
    NumberError error;
    int base;
    int long_count; // 0 = none, 1 = l/L, 2 = ll/LL
    bool is_unsigned;
    bool is_float;
    char bad_digit; // set for NUMBER_INVALID_DIGIT_*
    const char *suffix;
    size_t suffix_len;
    size_t body_len; // digits/exponent part before the suffix
} NumberInfo;

// Failure modes of a universal character name (\uXXXX / \UXXXXXXXX).
typedef enum {
    UCN_OK,
    UCN_INCOMPLETE,
    UCN_INVALID,
} UcnError;

// Keyword lookup table, built on first use and released at process exit.
static HashMap g_keywords;

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

static void keywords_fini(void)
{
    hashmap_free(&g_keywords);
}

// Check whether str starts with the given prefix.
static inline bool startswith(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

// Create a token from the source range [start, end).
// The token keeps a reference to the original source buffer.
static Token *new_token(TokenType type, const char *start, const char *end)
{
    Token *tok = xcalloc(1, sizeof(Token));
    tok->type = type;
    tok->span.start = start;
    tok->span.length = end - start;
    return tok;
}

// Value of a hexadecimal digit. Callers must pass a digit they have already
// validated as hex, octal, decimal or binary; no range check happens here.
static inline int digit_value(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
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

// Check whether a token text is a reserved keyword.
static bool is_keyword(const char *start, size_t length)
{
    if (g_keywords.capacity == 0) {
        // clang-format off
        static const char *kw[] = {
            "return",    "if",         "else",
            "for",       "while",      "int",
            "sizeof",    "char",       "struct",
            "union",     "short",      "long",
            "void",      "typedef",    "_Bool",
            "enum",      "static",     "goto",
            "break",     "continue",   "switch",
            "case",      "default",    "extern",
            "_Alignof",  "_Alignas",   "do",
            "signed",    "unsigned",   "const",
            "volatile",  "auto",       "register",
            "restrict",  "__restrict", "__restrict__",
            "_Noreturn", "float",      "double",
            "typeof",    "asm",        "_Thread_local",
            "__thread",  "_Atomic",    "__attribute__",
            "inline",    "_Static_assert", "_Generic",
            "_Complex",  "_Imaginary",
            "__asm__",   "__inline__", "__typeof__",
            "__volatile__", "__const__", "__signed__",
            "__alignof__",
        };
        // clang-format on

        for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++)
            hashmap_put(&g_keywords, kw[i], (void *)1);

        atexit(keywords_fini);
    }

    return hashmap_get_range(&g_keywords, start, length) != NULL;
}

// ---------------------------------------------------------------------------
// Token scanning
// ---------------------------------------------------------------------------

// Scan a numeric literal.
// This function only determines the token boundary; token_num() validates and
// decodes it later. '_' belongs to a pp-number, which keeps spellings like
// `1_000` and `2._` in one token so the validator can reject them like GCC.
static Token *lex_number(const char **current)
{
    const char *start = *current;
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
    *current = p;
    return new_token(TOKEN_NUMBER, start, p);
}

// Scan a string literal.
// This function only determines the token boundary; token_string() decodes the
// contents later.
static Token *lex_string(const char **current, const char *limit)
{
    const char *start = *current;
    const char *p = start;

    if (startswith(p, "u8\""))
        p += 3;
    else if (startswith(p, "u\"") || startswith(p, "U\"") || startswith(p, "L\""))
        p += 2;
    else
        p++;

    while (p < limit && !is_line_break(*p)) {
        if (*p == '\\') {
            p++;
            if (p < limit && !is_line_break(*p)) {
                p++;
            }
        } else if (*p == '"') {
            p++;
            *current = p;
            return new_token(TOKEN_STRING, start, p);
        } else {
            p++;
        }
    }

    // Recovery: report and stop at the newline (or EOF); the caller continues
    // lexing the following line. Underline the malformed literal like GCC.
    error_span(start, p, "missing terminating \" character");
    *current = p;
    return NULL;
}

// Scan a character constant.
// This function only determines the token boundary; token_char() decodes the
// value later. An empty constant is reported here so its diagnostic keeps the
// source position GCC uses.
static Token *lex_char(const char **current, const char *limit)
{
    const char *start = *current;
    const char *p = start;

    if (startswith(p, "u'") || startswith(p, "U'") || startswith(p, "L'"))
        p += 2;
    else
        p++;

    const char *content = p;

    while (p < limit && !is_line_break(*p)) {
        if (*p == '\\') {
            p++;
            if (p < limit && !is_line_break(*p))
                p++;
        } else if (*p == '\'') {
            if (p == content)
                error_span(start, p + 1, "empty character constant");
            p++;
            *current = p;
            return new_token(TOKEN_CHAR, start, p);
        } else {
            p++;
        }
    }

    // Recovery: report and stop at the newline (or EOF). Underline the
    // malformed literal like GCC.
    error_span(start, p, "missing terminating ' character");
    *current = p;
    return NULL;
}

// Return the length of the punctuation token at the current position.
// Matching is driven by the first byte; multi-byte punctuators are checked
// before their prefixes, so the longest punctuator wins.
static size_t read_punct(const char *p)
{
    switch (*p) {
    case '[':
    case ']':
    case '(':
    case ')':
    case '{':
    case '}':
    case ';':
    case ',':
    case '?':
    case ':':
    case '~':
        return 1;
    case '.':
        return p[1] == '.' && p[2] == '.' ? 3 : 1;
    case '<':
        if (p[1] == '<')
            return p[2] == '=' ? 3 : 2;
        return p[1] == '=' ? 2 : 1;
    case '>':
        if (p[1] == '>')
            return p[2] == '=' ? 3 : 2;
        return p[1] == '=' ? 2 : 1;
    case '=':
    case '!':
    case '*':
    case '/':
    case '%':
    case '^':
        return p[1] == '=' ? 2 : 1;
    case '+':
        return (p[1] == '+' || p[1] == '=') ? 2 : 1;
    case '-':
        if (p[1] == '-')
            return 2;
        return (p[1] == '=' || p[1] == '>') ? 2 : 1;
    case '&':
        return (p[1] == '&' || p[1] == '=') ? 2 : 1;
    case '|':
        return (p[1] == '|' || p[1] == '=') ? 2 : 1;
    case '#':
        return p[1] == '#' ? 2 : 1;
    default:
        return 0;
    }
}

// Scan an identifier or keyword token.
// The first character must already be validated by the caller.
// The scanned text is classified after checking the keyword table.
static Token *lex_identifier(const char **current)
{
    const char *start = *current;
    // '$' is a GCC extension accepted in identifiers by default.
    while (isalnum((unsigned char)**current) || **current == '_' || **current == '$') {
        (*current)++;
    }
    size_t len = *current - start;
    if (is_keyword(start, len))
        return new_token(TOKEN_KEYWORD, start, *current);

    return new_token(TOKEN_IDENT, start, *current);
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
static Token *lex_punct(const char **current)
{
    const char *start = *current;
    size_t len = read_punct(start);

    if (len == 0) {
        size_t n = utf8_len(start);
        report_stray(start);
        *current = start + n;
        return NULL;
    }

    *current += len;
    return new_token(TOKEN_PUNCT, start, *current);
}

// ---------------------------------------------------------------------------
// Literal decoding
// ---------------------------------------------------------------------------

// Parse the integer suffix of a constant (C11 6.4.4.1). Case is significant
// for the long suffix: "lL"/"Ll" are invalid, while "u"/"U" may appear on
// either side and at most once.
static bool parse_int_suffix(const char *s, size_t n, int *long_count, bool *is_unsigned)
{
    size_t i = 0;
    bool u = false;
    int l = 0;

    if (i < n && (s[i] == 'u' || s[i] == 'U')) {
        u = true;
        i++;
    }
    if (i < n && (s[i] == 'l' || s[i] == 'L')) {
        char c = s[i++];
        l = 1;
        if (i < n && s[i] == c) {
            l = 2;
            i++;
        }
    }
    if (!u && i < n && (s[i] == 'u' || s[i] == 'U')) {
        u = true;
        i++;
    }
    if (i != n)
        return false;

    *long_count = l;
    *is_unsigned = u;
    return true;
}

static bool is_valid_float_suffix(const char *s, size_t n)
{
    return n == 0 || (n == 1 && (s[0] == 'f' || s[0] == 'F' || s[0] == 'l' || s[0] == 'L'));
}

// Split a pp-number into a numeric body and a suffix, reporting the malformed
// spellings GCC diagnoses. This is pure: it reports nothing itself.
static NumberInfo scan_number(const char *start, const char *end)
{
    NumberInfo info = { .error = NUMBER_OK, .base = 10, .suffix = start };
    const char *p = start;
    bool has_dot = false;
    bool has_exp = false;

    if (end - start > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        (isxdigit((unsigned char)p[2]) || p[2] == '.')) {
        info.base = 16;
        p += 2;
    } else if (end - start > 2 && p[0] == '0' && (p[1] == 'b' || p[1] == 'B') &&
               (p[2] == '0' || p[2] == '1')) {
        info.base = 2;
        p += 2;
    }

    if (info.base == 16) {
        int digits = 0;
        while (p < end && isxdigit((unsigned char)*p)) {
            p++;
            digits++;
        }
        if (p < end && *p == '.') {
            has_dot = true;
            p++;
            while (p < end && isxdigit((unsigned char)*p)) {
                p++;
                digits++;
            }
        }
        if (digits == 0) {
            info.error = NUMBER_HEX_FLOAT_NO_DIGITS;
            return info;
        }
        if (p < end && (*p == 'p' || *p == 'P')) {
            has_exp = true;
            p++;
            if (p < end && (*p == '+' || *p == '-'))
                p++;
            if (p == end || !isdigit((unsigned char)*p)) {
                info.error = NUMBER_EXPONENT_HAS_NO_DIGITS;
                return info;
            }
            while (p < end && isdigit((unsigned char)*p))
                p++;
        }
        if (has_dot && !has_exp) {
            info.error = NUMBER_HEX_FLOAT_REQUIRES_EXPONENT;
            return info;
        }
        info.is_float = has_dot || has_exp;
    } else if (info.base == 2) {
        while (p < end && isdigit((unsigned char)*p)) {
            if (*p > '1') {
                info.error = NUMBER_INVALID_DIGIT_BINARY;
                info.bad_digit = *p;
                return info;
            }
            p++;
        }
        if (p < end && (*p == '.' || *p == 'e' || *p == 'E')) {
            info.error = NUMBER_BINARY_FLOAT_PREFIX;
            return info;
        }
        info.is_float = false;
    } else {
        // Decimal or octal mantissa, optionally followed by a decimal exponent.
        while (p < end) {
            if (isdigit((unsigned char)*p)) {
                p++;
            } else if (*p == '.') {
                if (has_exp)
                    break; // a dot after the exponent becomes an invalid suffix
                if (has_dot) {
                    info.error = NUMBER_TOO_MANY_DECIMAL_POINTS;
                    return info;
                }
                has_dot = true;
                p++;
            } else {
                break;
            }
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            has_exp = true;
            p++;
            if (p < end && (*p == '+' || *p == '-'))
                p++;
            if (p == end || !isdigit((unsigned char)*p)) {
                info.error = NUMBER_EXPONENT_HAS_NO_DIGITS;
                return info;
            }
            while (p < end && isdigit((unsigned char)*p))
                p++;
        }
        info.is_float = has_dot || has_exp;
        if (!info.is_float && start[0] == '0' && end - start > 1) {
            info.base = 8;
            for (const char *q = start + 1; q < end; q++) {
                if (*q == '8' || *q == '9') {
                    info.error = NUMBER_INVALID_DIGIT_OCTAL;
                    info.bad_digit = *q;
                    return info;
                }
                if (!isdigit((unsigned char)*q))
                    break; // the suffix starts here
            }
        }
    }

    info.body_len = (size_t)(p - start);
    info.suffix = p;
    info.suffix_len = (size_t)(end - p);

    if (info.is_float) {
        if (!is_valid_float_suffix(info.suffix, info.suffix_len)) {
            info.error = NUMBER_INVALID_SUFFIX_FLOATING;
            return info;
        }
    } else if (!parse_int_suffix(info.suffix, info.suffix_len, &info.long_count,
                                 &info.is_unsigned)) {
        info.error = NUMBER_INVALID_SUFFIX_INTEGER;
    }
    return info;
}

// C11 6.4.4.1 type of an integer constant on an LP64 target. A suffixless
// decimal constant beyond LLONG_MAX is ill-formed; GCC extends it to unsigned
// long long, which we match (its warning is skipped).
// LP32 needs a target limits parameter; add it when such a target
// actually exists.
static LiteralType infer_int_type(uint64_t value, int base, int long_count, bool is_unsigned)
{
    bool fits_int = value <= (uint64_t)INT32_MAX;
    bool fits_uint = value <= (uint64_t)UINT32_MAX;
    bool fits_long = value <= (uint64_t)INT64_MAX;

    if (is_unsigned) {
        if (long_count == 2)
            return LITERAL_ULLONG;
        if (long_count == 1)
            return LITERAL_ULONG;
        return fits_uint ? LITERAL_UINT : LITERAL_ULONG;
    }

    if (long_count == 2)
        return fits_long ? LITERAL_LLONG : LITERAL_ULLONG;
    if (long_count == 1)
        return fits_long ? LITERAL_LONG : LITERAL_ULONG;
    if (base == 10)
        return fits_int ? LITERAL_INT : fits_long ? LITERAL_LONG : LITERAL_ULLONG;
    if (fits_int)
        return LITERAL_INT;
    if (fits_uint)
        return LITERAL_UINT;
    return fits_long ? LITERAL_LONG : LITERAL_ULONG;
}

static uint64_t decode_int(const char *start, size_t body_len, int base)
{
    const char *p = start;
    const char *end = start + body_len;
    uint64_t value = 0;

    if (base == 16 || base == 2)
        p += 2;

    for (; p < end; p++) {
        unsigned char c = (unsigned char)*p;
        unsigned digit = (unsigned)digit_value((char)c);
        value = value * (uint64_t)base + digit;
    }
    return value;
}

// Validate and decode one numeric token; on error report and leave the value 0.
static void token_num(Token *tok)
{
    NumberInfo info = scan_number(tok->span.start, tok->span.start + tok->span.length);

    switch (info.error) {
    case NUMBER_OK:
        break;
    case NUMBER_INVALID_SUFFIX_INTEGER:
        error_at(&tok->span, "invalid suffix \"%.*s\" on integer constant", (int)info.suffix_len,
                 info.suffix);
        return;
    case NUMBER_INVALID_SUFFIX_FLOATING:
        error_at(&tok->span, "invalid suffix \"%.*s\" on floating constant", (int)info.suffix_len,
                 info.suffix);
        return;
    case NUMBER_EXPONENT_HAS_NO_DIGITS:
        error_at(&tok->span, "exponent has no digits");
        return;
    case NUMBER_INVALID_DIGIT_OCTAL:
        error_at(&tok->span, "invalid digit \"%c\" in octal constant", info.bad_digit);
        return;
    case NUMBER_INVALID_DIGIT_BINARY:
        error_at(&tok->span, "invalid digit \"%c\" in binary constant", info.bad_digit);
        return;
    case NUMBER_TOO_MANY_DECIMAL_POINTS:
        error_at(&tok->span, "too many decimal points in number");
        return;
    case NUMBER_HEX_FLOAT_REQUIRES_EXPONENT:
        error_at(&tok->span, "hexadecimal floating constants require an exponent");
        return;
    case NUMBER_HEX_FLOAT_NO_DIGITS:
        error_at(&tok->span, "no digits in hexadecimal floating constant");
        return;
    case NUMBER_BINARY_FLOAT_PREFIX:
        error_at(&tok->span, "invalid prefix \"%.2s\" for floating constant", tok->span.start);
        return;
    }

    if (info.is_float) {
        char *endp;
        tok->fval = strtold(tok->span.start, &endp);
        if (endp != tok->span.start + info.body_len)
            unreachable();
        if (info.suffix_len == 0)
            tok->literal = LITERAL_DOUBLE;
        else if (info.suffix[0] == 'f' || info.suffix[0] == 'F')
            tok->literal = LITERAL_FLOAT;
        else
            tok->literal = LITERAL_LDOUBLE;
    } else {
        tok->val = (int64_t)decode_int(tok->span.start, info.body_len, info.base);
        tok->literal =
            infer_int_type((uint64_t)tok->val, info.base, info.long_count, info.is_unsigned);
    }
}

// Parse a universal character name; *pp points at the 'u' or 'U'. On success
// stores the code point in *cp. An incomplete name consumes the hex digits it
// saw so the diagnostic can quote that spelling.
static UcnError parse_ucn(const char **pp, const char *end, uint32_t *cp)
{
    const char *p = *pp; // points at the 'u' or 'U'
    int want = (*p == 'u') ? 4 : 8;
    p++;

    uint32_t value = 0;
    int digits = 0;

    while (digits < want && p < end && isxdigit((unsigned char)*p)) {
        unsigned char d = (unsigned char)*p++;
        value = value * 16u + (uint32_t)digit_value((char)d);
        digits++;
    }

    *pp = p;
    if (digits < want)
        return UCN_INCOMPLETE;

    // C11 6.4.3p2: values below 0xA0 are reserved except $, @ and `; surrogate
    // pairs are invalid. GCC accepts up to 0x7FFFFFFF (warning beyond the UCS
    // range) and rejects anything larger.
    if ((value < 0xa0 && value != '$' && value != '@' && value != '`') ||
        (value >= 0xd800 && value <= 0xdfff) || value > 0x7fffffffu)
        return UCN_INVALID;

    *cp = value;
    return UCN_OK;
}

// Encode one code point as UTF-8 and return the number of bytes written.
// GCC accepts universal character names up to 0x7FFFFFFF and emits the original
// 1-6 byte layout (5- and 6-byte forms beyond 0x1FFFFF); the lexer must match
// those bytes exactly.
static int encode_utf8(uint32_t cp, unsigned char out[6])
{
    if (cp <= 0x7f) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp <= 0x7ff) {
        out[0] = (unsigned char)(0xc0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp <= 0xffff) {
        out[0] = (unsigned char)(0xe0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (unsigned char)(0x80 | (cp & 0x3f));
        return 3;
    }
    if (cp <= 0x1fffff) {
        out[0] = (unsigned char)(0xf0 | ((cp >> 18) & 0x07));
        out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        out[3] = (unsigned char)(0x80 | (cp & 0x3f));
        return 4;
    }
    if (cp <= 0x3ffffff) {
        out[0] = (unsigned char)(0xf8 | (cp >> 24));
        out[1] = (unsigned char)(0x80 | ((cp >> 18) & 0x3f));
        out[2] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        out[3] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        out[4] = (unsigned char)(0x80 | (cp & 0x3f));
        return 5;
    }
    out[0] = (unsigned char)(0xfc | (cp >> 30));
    out[1] = (unsigned char)(0x80 | ((cp >> 24) & 0x3f));
    out[2] = (unsigned char)(0x80 | ((cp >> 18) & 0x3f));
    out[3] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
    out[4] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    out[5] = (unsigned char)(0x80 | (cp & 0x3f));
    return 6;
}

// Decode one escape sequence; *pp points just after the backslash. Universal
// character names are encoded as UTF-8. Unknown escapes keep the character
// itself, matching GCC's tolerant behavior (it warns, and the test harness
// builds with -w). On success stores the decoded element value (unmasked, so
// character constants can keep its low bytes) in *value_out and returns the
// number of bytes written to out. Returns 0 when the escape was already
// diagnosed and contributes no bytes.
static int decode_escape(Token *tok, const char **pp, const char *end, unsigned char out[6],
                         uint32_t *value_out)
{
    const char *spell = *pp - 1;
    const char *p = *pp;
    unsigned char c = (unsigned char)*p++;
    uint32_t value = 0;
    bool has_value = false;

    switch (c) {
    case 'a':
        out[0] = 7;
        break;
    case 'b':
        out[0] = 8;
        break;
    case 'e': // GNU extension
        out[0] = 27;
        break;
    case 'f':
        out[0] = 12;
        break;
    case 'n':
        out[0] = 10;
        break;
    case 'r':
        out[0] = 13;
        break;
    case 't':
        out[0] = 9;
        break;
    case 'v':
        out[0] = 11;
        break;
    case '\\':
    case '\'':
    case '"':
    case '?':
        out[0] = c;
        break;
    case 'u':
    case 'U': {
        const char *ucn = spell + 1; // parse_ucn() expects the marker itself
        uint32_t cp = 0;
        UcnError err = parse_ucn(&ucn, end, &cp);
        *pp = ucn;
        if (err == UCN_INCOMPLETE) {
            error_at(&tok->span, "incomplete universal character name %.*s", (int)(ucn - spell),
                     spell);
            return 0;
        }
        if (err == UCN_INVALID) {
            error_at(&tok->span, "%.*s is not a valid universal character", (int)(ucn - spell),
                     spell);
            return 0;
        }
        *value_out = cp;
        return encode_utf8(cp, out);
    }
    case 'x': {
        const char *digits = p;
        value = 0;
        while (p < end && isxdigit((unsigned char)*p)) {
            unsigned char d = (unsigned char)*p++;
            value = value * 16u + (uint32_t)digit_value((char)d);
        }
        if (p == digits) {
            error_at(&tok->span, "\\x used with no following hex digits");
            *pp = p;
            return 0;
        }
        out[0] = (unsigned char)(value & 0xff);
        has_value = true;
        break;
    }
    default:
        if (c >= '0' && c <= '7') {
            value = c - '0';
            for (int i = 0; i < 2 && p < end && *p >= '0' && *p <= '7'; i++)
                value = value * 8 + (uint32_t)(*p++ - '0');
            out[0] = (unsigned char)(value & 0xff);
            has_value = true;
        } else {
            out[0] = c;
            value = c;
            has_value = true;
        }
        break;
    }

    if (!has_value)
        value = out[0];
    *value_out = value;
    *pp = p;
    return 1;
}

// Decode the contents of a TOKEN_STRING into a heap buffer.
static void token_string(Token *tok)
{
    const char *start = tok->span.start;
    const char *end = start + tok->span.length;
    const char *p = start;

    if (startswith(p, "u8\""))
        p += 3;
    else if (startswith(p, "u\"") || startswith(p, "U\"") || startswith(p, "L\""))
        p += 2;
    else
        p++;

    const char *close = end - 1;
    char *buf = xmalloc((size_t)(close - p) + 1);
    int len = 0;

    while (p < close) {
        unsigned char bytes[6];
        uint32_t element;
        if (*p == '\\') {
            p++;
            int n = decode_escape(tok, &p, close, bytes, &element);
            for (int i = 0; i < n; i++)
                buf[len++] = (char)bytes[i];
        } else {
            buf[len++] = *p++;
        }
    }
    buf[len] = '\0';

    tok->str = buf;
    tok->str_len = len;
}

// GCC value of a 32-bit pattern reinterpreted as a signed int, without relying
// on an implementation-defined conversion.
static int64_t truncate_int32(uint32_t value)
{
    return (value & 0x80000000u) ? (int64_t)value - 0x100000000LL : (int64_t)value;
}

// Decode a TOKEN_CHAR value the way GCC does:
//   - a plain constant with exactly one decoded byte is sign-extended
//     ('\xff' == -1), otherwise the bytes pack MSB first and truncate to int
//     ('hello' == 0x656C6C6F);
//   - u/U/L constants keep the last element's value (u masks to 16 bits,
//     U/L to 32).
static void token_char(Token *tok)
{
    const char *start = tok->span.start;
    const char *end = start + tok->span.length;
    const char *p = start;
    bool wide = false;

    if (startswith(p, "u'") || startswith(p, "U'") || startswith(p, "L'")) {
        wide = true;
        p += 2;
    } else {
        p++;
    }

    const char *close = end - 1;
    uint32_t value = 0;
    int count = 0; // number of decoded bytes (plain) or elements (wide)

    while (p < close) {
        unsigned char bytes[6];
        uint32_t element = 0;
        int n;
        if (*p == '\\') {
            p++;
            n = decode_escape(tok, &p, close, bytes, &element);
            if (n == 0)
                continue; // already diagnosed
        } else {
            bytes[0] = (unsigned char)*p++;
            element = bytes[0];
            n = 1;
        }
        if (wide) {
            value = element;
        } else {
            for (int i = 0; i < n; i++)
                value = (value << 8) | bytes[i];
        }
        count += n;
    }

    if (wide) {
        if (start[0] == 'u')
            value &= 0xffffu;
        else
            value &= 0xffffffffu;
        tok->val = truncate_int32(value);
    } else if (count == 1) {
        tok->val = (value & 0x80u) ? (int64_t)value - 0x100 : (int64_t)value;
    } else {
        tok->val = truncate_int32(value);
    }
    tok->literal = LITERAL_INT;
}

// Decode every literal token in the list; lexical errors keep the list going.
static void parse_token_values(Token *tok)
{
    for (; tok; tok = tok->next) {
        if (tok->type == TOKEN_NUMBER)
            token_num(tok);
        else if (tok->type == TOKEN_CHAR)
            token_char(tok);
        else if (tok->type == TOKEN_STRING)
            token_string(tok);
    }
}

// ---------------------------------------------------------------------------
// Token pipeline
// ---------------------------------------------------------------------------

// Attach `file` to every token in the list.
static void add_token_file(Token *tok, SourceFile *file)
{
    for (; tok; tok = tok->next)
        tok->span.file = file;
}

// Assign 1-based line/column to every token by walking the source once.
// Columns are display columns (see source.h), matching diagnostics. Tokens must
// be ordered by span.start, which tokenize() guarantees.
static void add_line_column(Token *tok, SourceFile *file)
{
    const char *p = file->contents;
    unsigned line = 1;
    int width = 0; // 0-based display width within the current line

    for (; tok; tok = tok->next) {
        if (tok->span.start < p)
            unreachable();

        advance_position(p, tok->span.start, &line, &width);
        p = tok->span.start;

        tok->span.line = line;
        tok->span.column = (unsigned)width + 1;
    }
}

// Attach all token metadata (file, then line/column) in one call.
static void add_token_metadata(Token *tok, SourceFile *file)
{
    add_token_file(tok, file);
    add_line_column(tok, file);
}

// Convert source code into a linked list of tokens.
// This is the entry point of lexical analysis.
Token *tokenize(const char *contents, size_t length)
{
    const char *content = contents;
    const char *limit = contents + length;

    Token head = { 0 };
    Token *curr = &head;

    while (content < limit) {
        Token *tok = NULL;
        const char *before = content;

        // GCC ignores NUL bytes embedded in the file; skip them so lexing
        // continues past them.
        if (*content == '\0') {
            content++;
            continue;
        }
        // skip line comments (a lone CR also ends the comment)
        if (startswith(content, "//")) {
            content += 2;
            while (content < limit && !is_line_break(*content))
                content++;
            continue;
        }
        // skip block comments; a NUL inside is data, so match the terminator
        // byte-wise instead of with strstr()
        else if (startswith(content, "/*")) {
            const char *p = content + 2;
            while (p + 1 < limit && !(p[0] == '*' && p[1] == '/'))
                p++;
            if (p + 1 >= limit) {
                // Recovery: unterminated comment consumes the rest of the file.
                error_span(content, content + 1, "unterminated comment");
                content = limit; // EOF token lands at end of file
                break;
            }
            content = p + 2;
            continue;
        }
        //  skip possible space
        else if (isspace((unsigned char)*content)) {
            content++;
            continue;
        }
        //  Numeric literal (a pp-number may also start with '.' + digit)
        else if (isdigit((unsigned char)*content) ||
                 (*content == '.' && isdigit((unsigned char)content[1]))) {
            tok = lex_number(&content);
        }
        // String literal
        else if (is_string_literal(content)) {
            tok = lex_string(&content, limit);
        }
        // Character literal
        else if (is_char_literal(content)) {
            tok = lex_char(&content, limit);
        }
        // Identifier or keyword ('$' is a GCC extension in identifiers)
        else if (isalpha((unsigned char)*content) || *content == '_' || *content == '$') {
            tok = lex_identifier(&content);
        }
        // Punctuators
        else {
            tok = lex_punct(&content);
        }

        // Guard the recovery invariant: never re-lex the same position.
        if (content == before) {
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
    Token *eof = new_token(TOKEN_EOF, content, content);
    curr->next = eof;
    return head.next;
}

// Load `path`, lex it, attach metadata and decode literal values.
// Returns the token list, or NULL on I/O failure with errno preserved. On
// success the loaded file becomes current_source(); the caller releases the
// tokens with token_free() and the file with source_free(current_source()).
Token *tokenize_file(const char *path)
{
    SourceFile *file = source_load(path);
    if (!file)
        return NULL;

    set_current_source(file); // used by diagnostics inside tokenize()

    Token *tokens = tokenize(file->contents, file->size);
    add_token_metadata(tokens, file);
    parse_token_values(tokens);
    return tokens;
}

// Release a token list produced by tokenize()/tokenize_file(). Safe with NULL.
void token_free(Token *tok)
{
    while (tok) {
        Token *next = tok->next;
        free(tok->str);
        free(tok);
        tok = next;
    }
}

#ifdef TEST_LEXER

static int failures = 0;

static void check_loc(Token *tok, uint32_t line, uint32_t column, const char *msg)
{
    if (tok && tok->span.line == line && tok->span.column == column) {
        printf("ok   : %s (%u:%u)\n", msg, tok->span.line, tok->span.column);
    } else {
        printf("FAIL : %s (got %u:%u, want %u:%u)\n", msg, tok ? tok->span.line : 0,
               tok ? tok->span.column : 0, line, column);
        failures++;
    }
}

typedef struct {
    TokenType type;
    const char *text;
} ExpectedToken;

// Tokenize `len` bytes of `src` and assert the whole stream (ending in EOF)
// matches `exp`.
static void check_stream_bytes(const char *src, size_t len, const ExpectedToken *exp, size_t n,
                               const char *msg)
{
    SourceFile file = { .name = "stream", .contents = (char *)src };
    set_current_source(&file);

    Token *head = tokenize(src, len);
    add_token_metadata(head, &file);
    Token *tok = head;
    int ok = 1;
    size_t i = 0;

    while (i < n && tok) {
        size_t len = strlen(exp[i].text);
        if (tok->type != exp[i].type || (size_t)tok->span.length != len ||
            memcmp(tok->span.start, exp[i].text, len) != 0) {
            ok = 0;
            break;
        }
        i++;
        tok = tok->next;
    }

    if (ok && i == n && !tok) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (matched %zu/%zu)\n", msg, i, n);
        failures++;
    }

    token_free(head);
}

static void check_stream(const char *src, const ExpectedToken *exp, size_t n, const char *msg)
{
    check_stream_bytes(src, strlen(src), exp, n, msg);
}

// Literal decoding checks. scan_number() is pure, so the error table needs no
// diagnostic capture; value checks run tokenize + metadata + parse_token_values.
static void check_number_error(const char *src, NumberError want, const char *msg)
{
    NumberInfo info = scan_number(src, src + strlen(src));

    if (info.error == want) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (got error %d, want %d)\n", msg, (int)info.error, (int)want);
        failures++;
    }
}

static Token *literal_token(const char *src, SourceFile *file)
{
    file->name = "literal";
    file->contents = (char *)src;
    set_current_source(file);

    Token *tok = tokenize(src, strlen(src));
    add_token_metadata(tok, file);
    parse_token_values(tok);
    return tok;
}

static void check_int_value(const char *src, int64_t val, LiteralType type, const char *msg)
{
    SourceFile file = { 0 };
    Token *tok = literal_token(src, &file);

    if (tok && tok->type == TOKEN_NUMBER && tok->val == val && tok->literal == type) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (type %d, val %lld, lit %d)\n", msg, tok ? (int)tok->type : -1,
               tok ? (long long)tok->val : 0LL, tok ? (int)tok->literal : -1);
        failures++;
    }
    token_free(tok);
}

static void check_float_value(const char *src, long double fval, LiteralType type, const char *msg)
{
    SourceFile file = { 0 };
    Token *tok = literal_token(src, &file);

    if (tok && tok->type == TOKEN_NUMBER && tok->fval == fval && tok->literal == type) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (type %d, fval %Lf, lit %d)\n", msg, tok ? (int)tok->type : -1,
               tok ? tok->fval : 0.0L, tok ? (int)tok->literal : -1);
        failures++;
    }
    token_free(tok);
}

static void check_char_value(const char *src, int64_t val, const char *msg)
{
    SourceFile file = { 0 };
    Token *tok = literal_token(src, &file);

    if (tok && tok->type == TOKEN_CHAR && tok->val == val && tok->literal == LITERAL_INT) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (type %d, val %lld)\n", msg, tok ? (int)tok->type : -1,
               tok ? (long long)tok->val : 0LL);
        failures++;
    }
    token_free(tok);
}

static void check_string_value(const char *src, const char *want, int want_len, const char *msg)
{
    SourceFile file = { 0 };
    Token *tok = literal_token(src, &file);

    if (tok && tok->type == TOKEN_STRING && tok->str && tok->str_len == want_len &&
        memcmp(tok->str, want, (size_t)want_len) == 0) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (type %d, len %d)\n", msg, tok ? (int)tok->type : -1,
               tok ? tok->str_len : -1);
        failures++;
    }
    token_free(tok);
}

static void check_ucn(const char *spelling, UcnError want, uint32_t want_cp, const char *msg)
{
    const char *p = spelling;
    uint32_t cp = 0;
    UcnError got = parse_ucn(&p, spelling + strlen(spelling), &cp);

    if (got == want && (want != UCN_OK || cp == want_cp)) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (got %d, cp %#x)\n", msg, (int)got, (unsigned)cp);
        failures++;
    }
}

static void check_utf8(uint32_t cp, const char *want, int want_len, const char *msg)
{
    unsigned char out[6] = { 0 };
    int n = encode_utf8(cp, out);

    if (n == want_len && memcmp(out, want, (size_t)want_len) == 0) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (got %d byte(s))\n", msg, n);
        failures++;
    }
}

static int run_tests(void)
{
    // Tab-indented source: display columns must match diagnostics.
    char src[] = "int main()\n"
                 "{\n"
                 "\treturn 0;\n"
                 "}\n";

    SourceFile file = { .name = "lexer_test.c", .contents = src };
    set_current_source(&file);

    Token *tokens = tokenize(src, strlen(src));
    add_token_metadata(tokens, &file);

    Token *t = tokens;
    check_loc(t, 1, 1, "int");
    t = t->next;
    check_loc(t, 1, 5, "main");
    t = t->next;
    check_loc(t, 1, 9, "(");
    t = t->next;
    check_loc(t, 1, 10, ")");
    t = t->next;
    check_loc(t, 2, 1, "{");
    t = t->next;
    check_loc(t, 3, 9, "return");
    t = t->next;
    check_loc(t, 3, 16, "0");
    t = t->next;
    check_loc(t, 3, 17, ";");
    t = t->next;
    check_loc(t, 4, 1, "}");
    t = t->next;
    check_loc(t, 5, 1, "EOF");

    // Every token, including EOF, carries the source file.
    int file_ok = 1;
    for (Token *p = tokens; p; p = p->next)
        if (p->span.file != &file)
            file_ok = 0;

    if (file_ok) {
        printf("ok   : file attached to all tokens\n");
    } else {
        printf("FAIL : file not attached to all tokens\n");
        failures++;
    }

    // UTF-8 is counted by code point, not by byte.
    char src2[] = "char *s = \"\xc3\xa9\"; int x;\n";
    SourceFile file2 = { .name = "lexer_test2.c", .contents = src2 };
    set_current_source(&file2);

    Token *tokens2 = tokenize(src2, strlen(src2));
    add_token_metadata(tokens2, &file2);

    Token *i = tokens2;
    while (i && !(i->type == TOKEN_KEYWORD && i->span.length == 3))
        i = i->next;

    check_loc(i, 1, 16, "int after UTF-8 string");

    token_free(tokens);
    token_free(tokens2);

    // Full token-stream assertion: comments are skipped, literals classified.
    const ExpectedToken exp[] = {
        { TOKEN_KEYWORD, "int" },  { TOKEN_IDENT, "main" }, { TOKEN_PUNCT, "(" },
        { TOKEN_PUNCT, ")" },      { TOKEN_PUNCT, "{" },    { TOKEN_KEYWORD, "return" },
        { TOKEN_NUMBER, "0" },     { TOKEN_PUNCT, ";" },    { TOKEN_PUNCT, "}" },
        { TOKEN_STRING, "\"s\"" }, { TOKEN_CHAR, "'c'" },   { TOKEN_EOF, "" },
    };
    check_stream("int main() { return 0; } // line\n/* block */ \"s\" 'c'\n", exp,
                 sizeof(exp) / sizeof(*exp), "token stream (comments, string, char)");

    // Malformed numbers: the spelling pass selects the same first error as GCC.
    check_number_error("1foo", NUMBER_INVALID_SUFFIX_INTEGER, "reject integer suffix foo");
    check_number_error("0lL", NUMBER_INVALID_SUFFIX_INTEGER, "reject mixed lL suffix");
    check_number_error("0uu", NUMBER_INVALID_SUFFIX_INTEGER, "reject duplicate u suffix");
    check_number_error("1ulx", NUMBER_INVALID_SUFFIX_INTEGER, "reject junk after suffix");
    check_number_error("1ullU", NUMBER_INVALID_SUFFIX_INTEGER, "reject u after long-long");
    check_number_error("1.5foo", NUMBER_INVALID_SUFFIX_FLOATING, "reject float suffix foo");
    check_number_error("1E2x", NUMBER_INVALID_SUFFIX_FLOATING, "reject exponent suffix x");
    check_number_error("1e+", NUMBER_EXPONENT_HAS_NO_DIGITS, "reject empty exponent");
    check_number_error("0x1p", NUMBER_EXPONENT_HAS_NO_DIGITS, "reject empty hex exponent");
    check_number_error("08", NUMBER_INVALID_DIGIT_OCTAL, "reject octal 8");
    check_number_error("0b102", NUMBER_INVALID_DIGIT_BINARY, "reject binary 2");
    check_number_error("1.2.3", NUMBER_TOO_MANY_DECIMAL_POINTS, "reject two decimal points");
    check_number_error("0x.5", NUMBER_HEX_FLOAT_REQUIRES_EXPONENT, "reject hex float without p");
    check_number_error("0b1.0", NUMBER_BINARY_FLOAT_PREFIX, "reject binary float prefix");
    check_number_error(".0foo", NUMBER_INVALID_SUFFIX_FLOATING, "reject suffix after leading dot");
    check_number_error("0x.p1", NUMBER_HEX_FLOAT_NO_DIGITS, "reject hex float without digits");

    // Decoded values and the C11 type each spelling implies.
    check_int_value("42", 42, LITERAL_INT, "decimal int value");
    check_int_value("0xff", 255, LITERAL_INT, "hex int value");
    check_int_value("0b1010", 10, LITERAL_INT, "binary int value");
    check_int_value("0123", 83, LITERAL_INT, "octal int value");
    check_int_value("0xffffffff", 4294967295LL, LITERAL_UINT, "hex unsigned int type");
    check_int_value("4294967296", 4294967296LL, LITERAL_LONG, "wide decimal is long");
    check_int_value("18446744073709551615", -1LL, LITERAL_ULLONG, "oversized decimal is ullong");
    check_int_value("123ULL", 123, LITERAL_ULLONG, "unsigned long long suffix");
    check_float_value("1.5", 1.5L, LITERAL_DOUBLE, "double literal");
    check_float_value("1.5f", 1.5L, LITERAL_FLOAT, "float suffix");
    check_float_value("0x1.fp3", 15.5L, LITERAL_DOUBLE, "hex float value");
    check_float_value(".5", 0.5L, LITERAL_DOUBLE, "leading-dot float value");
    check_char_value("'A'", 65, "char literal");
    check_char_value("'\\n'", 10, "char escape");
    check_string_value("\"a\\n\\x41\\101\"", "a\nAA", 4, "string escapes");

    // Universal character names: spelling rules and UTF-8 expansion.
    check_ucn("u12", UCN_INCOMPLETE, 0, "UCN u12 is incomplete");
    check_ucn("U0011000", UCN_INCOMPLETE, 0, "UCN U0011000 is incomplete");
    check_ucn("uZZZZ", UCN_INCOMPLETE, 0, "UCN without hex digits is incomplete");
    check_ucn("u0041", UCN_INVALID, 0, "UCN u0041 is not a valid character");
    check_ucn("u009F", UCN_INVALID, 0, "UCN u009F is not a valid character");
    check_ucn("uD800", UCN_INVALID, 0, "UCN surrogate is invalid");
    check_ucn("uDFFF", UCN_INVALID, 0, "UCN surrogate is invalid");
    check_ucn("u00e9", UCN_OK, 0xe9, "UCN u00e9");
    check_ucn("u0024", UCN_OK, 0x24, "UCN u0024 keeps $");
    check_ucn("u0040", UCN_OK, 0x40, "UCN u0040 keeps @");
    check_ucn("u0060", UCN_OK, 0x60, "UCN u0060 keeps `");
    check_ucn("U0001F600", UCN_OK, 0x1f600, "UCN U0001F600");
    check_ucn("U00110000", UCN_OK, 0x110000, "UCN beyond UCS is accepted");

    check_utf8(0x41, "A", 1, "UTF-8 one byte");
    check_utf8(0xe9, "\xc3\xa9", 2, "UTF-8 two bytes");
    check_utf8(0x20ac, "\xe2\x82\xac", 3, "UTF-8 three bytes");
    check_utf8(0x1f600, "\xf0\x9f\x98\x80", 4, "UTF-8 four bytes");
    check_utf8(0x1fffff, "\xf7\xbf\xbf\xbf", 4, "UTF-8 four-byte maximum");
    check_utf8(0x200000, "\xf8\x88\x80\x80\x80", 5, "UTF-8 five bytes, like GCC");
    check_utf8(0x7fffffff, "\xfd\xbf\xbf\xbf\xbf\xbf", 6, "UTF-8 six bytes, like GCC");

    check_string_value("\"\\u00e9\"", "\xc3\xa9", 2, "string UCN two bytes");
    check_string_value("\"\\U0001F600\"", "\xf0\x9f\x98\x80", 4, "string UCN four bytes");
    check_string_value("\"\\u0024\"", "$", 1, "string UCN keeps $");
    check_char_value("'\\u00e9'", 0xC3A9, "char UCN packs two bytes");
    check_char_value("'\\u20ac'", 0xE282AC, "char UCN packs three bytes");
    check_char_value("'ab'", 0x6162, "multi-char constant packs bytes");

    // C11 keywords and the '$' identifier extension.
    const ExpectedToken kw_exp[] = {
        { TOKEN_KEYWORD, "inline" },     { TOKEN_KEYWORD, "_Static_assert" },
        { TOKEN_KEYWORD, "_Generic" },   { TOKEN_KEYWORD, "_Complex" },
        { TOKEN_KEYWORD, "_Imaginary" }, { TOKEN_EOF, "" },
    };
    check_stream("inline _Static_assert _Generic _Complex _Imaginary\n", kw_exp,
                 sizeof(kw_exp) / sizeof(*kw_exp), "C11 keywords are classified");

    const ExpectedToken dollar_exp[] = {
        { TOKEN_IDENT, "$dollar" },
        { TOKEN_EOF, "" },
    };
    check_stream("$dollar\n", dollar_exp, sizeof(dollar_exp) / sizeof(*dollar_exp),
                 "dollar sign is an identifier character");

    // Binary constants take the integer-suffix error path, not the float prefix.
    check_number_error("0b1p3", NUMBER_INVALID_SUFFIX_INTEGER, "reject binary p suffix");
    check_number_error("0b1p+3", NUMBER_INVALID_SUFFIX_INTEGER, "reject binary signed p suffix");
    check_number_error("0b1P3", NUMBER_INVALID_SUFFIX_INTEGER, "reject binary P suffix");
    check_ucn("U7FFFFFFF", UCN_OK, 0x7fffffff, "UCN up to 0x7FFFFFFF is accepted");
    check_ucn("U80000000", UCN_INVALID, 0, "UCN beyond 0x7FFFFFFF is invalid");

    // GCC character-constant value rules.
    check_char_value("'\\xff'", -1, "plain high byte is sign-extended");
    check_char_value("'\\x80'", -128, "plain 0x80 is sign-extended");
    check_char_value("'\\xffa'", -6, "hex escape keeps the whole value");
    check_char_value("'\\xff\\xff\\xff\\xff'", -1, "four-byte constant truncates to int");
    check_char_value("'ffff'", 0x66666666, "multichar packs MSB first");
    check_char_value("'\\U0001F600'", -257976192, "UCN multichar packs UTF-8 bytes");
    check_char_value("u'\\u00e9'", 233, "u prefix keeps the code point");
    check_char_value("L'\\u20ac'", 8364, "L prefix keeps the code point");
    check_char_value("u'\\777'", 511, "u prefix keeps unmasked octal value");
    check_char_value("u'\\x1234'", 4660, "u prefix keeps maskable hex value");
    check_char_value("u'ab'", 98, "wide constant keeps the last element");

    // A lone CR also ends a line (a CRLF pair counts once).
    char src3[] = "int a;\r\nint b;\rint c;";
    SourceFile file3 = { .name = "lexer_test3.c", .contents = src3 };
    set_current_source(&file3);
    Token *tokens3 = tokenize(src3, strlen(src3));
    add_token_metadata(tokens3, &file3);
    Token *t3 = tokens3;
    for (int i = 0; i < 3; i++)
        t3 = t3->next;
    check_loc(t3, 2, 1, "CRLF advances one line");
    for (int i = 0; i < 3; i++)
        t3 = t3->next;
    check_loc(t3, 3, 1, "lone CR advances one line");
    token_free(tokens3);

    // NUL bytes are skipped instead of ending the file.
    char src4[] = "int a;\0 int b;";
    const ExpectedToken nul_exp[] = {
        { TOKEN_KEYWORD, "int" }, { TOKEN_IDENT, "a" }, { TOKEN_PUNCT, ";" },
        { TOKEN_KEYWORD, "int" }, { TOKEN_IDENT, "b" }, { TOKEN_PUNCT, ";" },
        { TOKEN_EOF, "" },
    };
    check_stream_bytes(src4, sizeof(src4) - 1, nul_exp, sizeof(nul_exp) / sizeof(*nul_exp),
                       "NUL bytes are ignored");

    // NUL inside a comment is data: it must not end the comment early.
    char src5[] = "int a; // c\0 @\n";
    const ExpectedToken line_comment_exp[] = {
        { TOKEN_KEYWORD, "int" },
        { TOKEN_IDENT, "a" },
        { TOKEN_PUNCT, ";" },
        { TOKEN_EOF, "" },
    };
    check_stream_bytes(src5, sizeof(src5) - 1, line_comment_exp,
                       sizeof(line_comment_exp) / sizeof(*line_comment_exp),
                       "NUL does not end a line comment");

    char src6[] = "int a; /* c\0*/ int b;";
    const ExpectedToken block_comment_exp[] = {
        { TOKEN_KEYWORD, "int" }, { TOKEN_IDENT, "a" }, { TOKEN_PUNCT, ";" },
        { TOKEN_KEYWORD, "int" }, { TOKEN_IDENT, "b" }, { TOKEN_PUNCT, ";" },
        { TOKEN_EOF, "" },
    };
    check_stream_bytes(src6, sizeof(src6) - 1, block_comment_exp,
                       sizeof(block_comment_exp) / sizeof(*block_comment_exp),
                       "NUL does not hide a block comment terminator");

    // NUL inside a literal is preserved (GCC keeps it too).
    char src7[] = "char s[] = \"a\0b\";";
    SourceFile file7 = { .name = "lexer_test4.c", .contents = src7 };
    set_current_source(&file7);
    Token *tokens7 = tokenize(src7, sizeof(src7) - 1);
    add_token_metadata(tokens7, &file7);
    parse_token_values(tokens7);
    Token *t7 = tokens7;
    while (t7 && t7->type != TOKEN_STRING)
        t7 = t7->next;
    if (t7 && t7->str_len == 3 && t7->str[0] == 'a' && t7->str[1] == '\0' && t7->str[2] == 'b') {
        printf("ok   : NUL inside a string literal is preserved\n");
    } else {
        printf("FAIL : NUL inside a string literal was not preserved\n");
        failures++;
    }
    token_free(tokens7);

    char src8[] = "'a\0b'";
    SourceFile file8 = { .name = "lexer_test5.c", .contents = src8 };
    set_current_source(&file8);
    Token *tokens8 = tokenize(src8, sizeof(src8) - 1);
    add_token_metadata(tokens8, &file8);
    parse_token_values(tokens8);
    if (tokens8 && tokens8->type == TOKEN_CHAR && tokens8->val == 0x610062) {
        printf("ok   : NUL inside a character constant packs as a byte\n");
    } else {
        printf("FAIL : NUL inside a character constant\n");
        failures++;
    }
    token_free(tokens8);

    // Last diagnostic check: \x without hex digits reports and contributes no
    // bytes. Earlier checks must stay error-free, so had_error() is still false.
    SourceFile err_file = { 0 };
    Token *bad = literal_token("\"\\x\"", &err_file);
    if (bad && bad->type == TOKEN_STRING && bad->str_len == 0 && had_error()) {
        printf("ok   : \\x without hex digits is diagnosed\n");
    } else {
        printf("FAIL : \\x without hex digits was accepted\n");
        failures++;
    }
    token_free(bad);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all lexer tests passed\n");
    return 0;
}

// Debug aid: print a token list as "TYPE<tab>text", one token per line.
static const char *token_type_name(TokenType type)
{
    switch (type) {
    case TOKEN_KEYWORD:
        return "KEYWORD";
    case TOKEN_IDENT:
        return "IDENT";
    case TOKEN_NUMBER:
        return "NUMBER";
    case TOKEN_CHAR:
        return "CHAR";
    case TOKEN_PUNCT:
        return "PUNCT";
    case TOKEN_EOF:
        return "EOF";
    case TOKEN_STRING:
        return "STRING";
    default:
        return "UNKNOWN";
    }
}

static void dump_tokens(Token *tok)
{
    for (; tok; tok = tok->next) {
        printf("%-10s", token_type_name(tok->type));
        fwrite(tok->span.start, 1, tok->span.length, stdout);
        putchar('\n');
    }
}

int main(int argc, char **argv)
{
    // No argument: run the in-memory assertion tests.
    if (argc <= 1)
        return run_tests();

    // With a file: dump its token stream.
    Token *tokens = tokenize_file(argv[1]);
    if (!tokens) {
        fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    dump_tokens(tokens);

    token_free(tokens);
    source_free(current_source());

    return had_error() ? 1 : 0;
}

#endif
