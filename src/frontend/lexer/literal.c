// Literal decoding validates pp-number spellings and transfers decoded scalar
// or byte-string payloads only after the lexer coordinator has located tokens.

#include "lexer_internal.h"

#include "config.h"
#include "util/common.h"
#include "util/diagnostic.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Check whether str starts with the given prefix.
static inline bool startswith(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

// Value of a hexadecimal digit. Callers must pass a digit they have already
// validated as hex, octal, decimal or binary; no range check happens here.
static inline int digit_value(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

// ---------------------------------------------------------------------------
// Literal validation and decoding
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

// Recognize the complete optional suffix of a floating constant.
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
    }
#if GNU_EXTENSION
    else if (end - start > 2 && p[0] == '0' && (p[1] == 'b' || p[1] == 'B') &&
             (p[2] == '0' || p[2] == '1')) {
        info.base = 2;
        p += 2;
    }
#endif

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
    }
#if GNU_EXTENSION
    else if (info.base == 2) {
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
    }
#endif
    else {
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
    NumberInfo info = scan_number(string_view_begin(tok->span.text),
                                  string_view_end(tok->span.text));

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
        error_at(&tok->span, "invalid prefix \"%.2s\" for floating constant",
                 string_view_begin(tok->span.text));
        return;
    }

    if (info.is_float) {
        char *endp;
        tok->value.floating = strtold(string_view_begin(tok->span.text), &endp);
        if (endp != string_view_begin(tok->span.text) + info.body_len)
            unreachable();
        if (info.suffix_len == 0)
            tok->literal = LITERAL_DOUBLE;
        else if (info.suffix[0] == 'f' || info.suffix[0] == 'F')
            tok->literal = LITERAL_FLOAT;
        else
            tok->literal = LITERAL_LDOUBLE;
    } else {
        tok->value.integer =
            (int64_t)decode_int(string_view_begin(tok->span.text), info.body_len, info.base);
        tok->literal =
            infer_int_type((uint64_t)tok->value.integer, info.base, info.long_count, info.is_unsigned);
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
#if GNU_EXTENSION
    case 'e': // GNU extension
        out[0] = 27;
        break;
#else
    case 'e':
        error_at(&tok->span, "non-ISO-standard escape sequence, '\\e'");
        out[0] = 27;
        break;
#endif
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

// Decode the contents of a TOK_STRING into a heap buffer.
static void token_string(Token *tok)
{
    const char *start = string_view_begin(tok->span.text);
    const char *end = string_view_end(tok->span.text);
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

// Decode a TOK_CHARACTER value the way GCC does:
//   - a plain constant with exactly one decoded byte is sign-extended
//     ('\xff' == -1), otherwise the bytes pack MSB first and truncate to int
//     ('hello' == 0x656C6C6F);
//   - u/U/L constants keep the last element's value (u masks to 16 bits,
//     U/L to 32).
static void token_char(Token *tok)
{
    const char *start = string_view_begin(tok->span.text);
    const char *end = string_view_end(tok->span.text);
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
        tok->value.integer = truncate_int32(value);
    } else if (count == 1) {
        tok->value.integer = (value & 0x80u) ? (int64_t)value - 0x100 : (int64_t)value;
    } else {
        tok->value.integer = truncate_int32(value);
    }
    tok->literal = LITERAL_INT;
}

// Decode every literal token in the list; lexical errors keep the list going.
void decode_token_literals(Token *tok)
{
    for (; tok; tok = tok->next) {
        if (tok->kind == TOK_NUMBER)
            token_num(tok);
        else if (tok->kind == TOK_CHARACTER)
            token_char(tok);
        else if (tok->kind == TOK_STRING)
            token_string(tok);
    }
}

// Concatenate each maximal run of decoded TOK_STRING payloads as C11
// translation phase 6 requires. The first token survives with the complete
// source extent and owns one combined buffer; absorbed tokens are detached and
// released through the normal lexer lifecycle.
void concatenate_string_literals(Token *tokens)
{
    for (Token *first = tokens; first; first = first->next) {
        if (first->kind != TOK_STRING)
            continue;

        size_t length = (size_t)first->str_len;
        Token *last = first;
        while (last->next && last->next->kind == TOK_STRING) {
            last = last->next;
            length += (size_t)last->str_len;
        }
        if (last == first)
            continue;

        char *joined = xmalloc(length + 1);
        size_t offset = 0;
        for (Token *part = first;; part = part->next) {
            memcpy(joined + offset, part->str, (size_t)part->str_len);
            offset += (size_t)part->str_len;
            if (part == last)
                break;
        }
        joined[length] = '\0';

        SourceSpan span = source_span_join(&first->span, &last->span);
        Token *after = last->next;
        Token *absorbed = first->next;
        free(first->str);
        first->span = span;
        first->str = joined;
        first->str_len = (int)length;
        first->next = after;

        while (absorbed != after) {
            Token *next = absorbed->next;
            absorbed->next = NULL;
            token_free(absorbed);
            absorbed = next;
        }
    }
}


// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#ifdef TEST_LEXER

static int failures = 0;
static SourceFile g_test_source = { .name = (char *)"<lexer-test>" };

// Build a complete token stream for byte-oriented lexer tests. The shared
// fixture outlives every token inspection in a single test call.
static Token *tokenize_text(const char *src, size_t length)
{
    g_test_source.contents = (char *)src;
    g_test_source.size = length;
    return tokenize(&g_test_source);
}

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
    TokenKind kind;
    const char *text;
} ExpectedToken;

// Tokenize `len` bytes of `src` and assert the whole stream (ending in EOF)
// matches `exp`.
static void check_stream_bytes(const char *src, size_t len, const ExpectedToken *exp, size_t n,
                               const char *msg)
{
    Token *head = tokenize_text(src, len);
    Token *tok = head;
    int ok = 1;
    size_t i = 0;

    while (i < n && tok) {
        size_t len = strlen(exp[i].text);
        if (tok->kind != exp[i].kind || string_view_length(tok->span.text) != len ||
            memcmp(string_view_begin(tok->span.text), exp[i].text, len) != 0) {
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
// diagnostic capture; value checks run the complete public lexer pipeline.
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
    file->size = strlen(src);
    return tokenize(file);
}

static void check_int_value(const char *src, int64_t val, LiteralType type, const char *msg)
{
    SourceFile file = { 0 };
    Token *tok = literal_token(src, &file);

    if (tok && tok->kind == TOK_NUMBER && tok->value.integer == val && tok->literal == type) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (type %d, val %lld, lit %d)\n", msg, tok ? (int)tok->kind : -1,
               tok ? (long long)tok->value.integer : 0LL, tok ? (int)tok->literal : -1);
        failures++;
    }
    token_free(tok);
}

static void check_float_value(const char *src, long double fval, LiteralType type, const char *msg)
{
    SourceFile file = { 0 };
    Token *tok = literal_token(src, &file);

    if (tok && tok->kind == TOK_NUMBER && tok->value.floating == fval && tok->literal == type) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (type %d, fval %Lf, lit %d)\n", msg, tok ? (int)tok->kind : -1,
               tok ? tok->value.floating : 0.0L, tok ? (int)tok->literal : -1);
        failures++;
    }
    token_free(tok);
}

static void check_char_value(const char *src, int64_t val, const char *msg)
{
    SourceFile file = { 0 };
    Token *tok = literal_token(src, &file);

    if (tok && tok->kind == TOK_CHARACTER && tok->value.integer == val && tok->literal == LITERAL_INT) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (type %d, val %lld)\n", msg, tok ? (int)tok->kind : -1,
               tok ? (long long)tok->value.integer : 0LL);
        failures++;
    }
    token_free(tok);
}

static void check_string_value(const char *src, const char *want, int want_len, const char *msg)
{
    SourceFile file = { 0 };
    Token *tok = literal_token(src, &file);

    if (tok && tok->kind == TOK_STRING && tok->str && tok->str_len == want_len &&
        memcmp(tok->str, want, (size_t)want_len) == 0 && tok->next &&
        tok->next->kind == TOK_EOF && !tok->next->next &&
        string_view_begin(tok->span.text) == src &&
        string_view_end(tok->span.text) == src + strlen(src)) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s (type %d, len %d)\n", msg, tok ? (int)tok->kind : -1,
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

// Verify that every fixed token metadata entry round-trips through the lexer
// to its exact kind; one failure identifies the first divergent inventory row.
static void check_fixed_token_inventory(void)
{
    for (TokenKind kind = 0; kind < TOK_KIND_COUNT; kind++) {
        if (!token_kind_is_keyword(kind) && !token_kind_is_punctuator(kind))
            continue;
        const char *spelling = token_kind_spelling(kind);
        Token *tokens = tokenize_text(spelling, strlen(spelling));
        bool matches = tokens && tokens->kind == kind && tokens->next &&
                       tokens->next->kind == TOK_EOF && !tokens->next->next;
        token_free(tokens);
        if (!matches) {
            printf("FAIL : fixed token inventory diverges at %s\n",
                   token_kind_name(kind));
            failures++;
            return;
        }
    }
    printf("ok   : every fixed token spelling maps to its exact kind\n");
}

static int run_tests(void)
{
    check_fixed_token_inventory();

    // Tab-indented source: display columns must match diagnostics.
    char src[] = "int main()\n"
                 "{\n"
                 "\treturn 0;\n"
                 "}\n";

    Token *tokens = tokenize_text(src, strlen(src));

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
        if (p->span.file != &g_test_source)
            file_ok = 0;

    if (file_ok) {
        printf("ok   : file attached to all tokens\n");
    } else {
        printf("FAIL : file not attached to all tokens\n");
        failures++;
    }

    // UTF-8 is counted by code point, not by byte.
    char src2[] = "char *s = \"\xc3\xa9\"; int x;\n";
    Token *tokens2 = tokenize_text(src2, strlen(src2));

    Token *i = tokens2;
    while (i && i->kind != TOK_KW_INT)
        i = i->next;

    check_loc(i, 1, 16, "int after UTF-8 string");

    token_free(tokens);
    token_free(tokens2);

    // Full token-stream assertion: comments are skipped, literals classified.
    const ExpectedToken exp[] = {
        { TOK_KW_INT, "int" },       { TOK_IDENTIFIER, "main" },
        { TOK_LPAREN, "(" },         { TOK_RPAREN, ")" },
        { TOK_LBRACE, "{" },         { TOK_KW_RETURN, "return" },
        { TOK_NUMBER, "0" },         { TOK_SEMICOLON, ";" },
        { TOK_RBRACE, "}" },
        { TOK_STRING, "\"s\"" }, { TOK_CHARACTER, "'c'" },   { TOK_EOF, "" },
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
#if GNU_EXTENSION
    check_number_error("0b102", NUMBER_INVALID_DIGIT_BINARY, "reject binary 2");
#endif
    check_number_error("1.2.3", NUMBER_TOO_MANY_DECIMAL_POINTS, "reject two decimal points");
    check_number_error("0x.5", NUMBER_HEX_FLOAT_REQUIRES_EXPONENT, "reject hex float without p");
#if GNU_EXTENSION
    check_number_error("0b1.0", NUMBER_BINARY_FLOAT_PREFIX, "reject binary float prefix");
#else
    check_number_error("0b101", NUMBER_INVALID_SUFFIX_INTEGER,
                       "strict mode leaves the binary prefix to the decimal path");
#endif
    check_number_error(".0foo", NUMBER_INVALID_SUFFIX_FLOATING, "reject suffix after leading dot");
    check_number_error("0x.p1", NUMBER_HEX_FLOAT_NO_DIGITS, "reject hex float without digits");

    // Decoded values and the C11 type each spelling implies.
    check_int_value("42", 42, LITERAL_INT, "decimal int value");
    check_int_value("0xff", 255, LITERAL_INT, "hex int value");
#if GNU_EXTENSION
    check_int_value("0b1010", 10, LITERAL_INT, "binary int value");
#endif
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
    check_string_value("\"a\" \"b\"", "ab", 2,
                       "adjacent string literals concatenate");
    check_string_value("\"a\"/* gap */\n\"b\" \"c\"", "abc", 3,
                       "comments and newlines preserve string adjacency");
    check_string_value("\"a\\0\" \"b\"", "a\0b", 3,
                       "concatenation preserves embedded NUL bytes");
#if GNU_EXTENSION
    check_string_value("\"\\e\"", "\x1b", 1, "GNU \\e escape");
#else
    {
        int errors = error_count();
        check_string_value("\"\\e\"", "\x1b", 1, "strict mode still decodes \\e");
        if (error_count() == errors + 1)
            printf("ok   : strict mode diagnoses \\e\n");
        else {
            printf("FAIL : strict mode diagnoses \\e\n");
            failures++;
        }
    }
#endif

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
        { TOK_KW_INLINE, "inline" },
        { TOK_KW_STATIC_ASSERT, "_Static_assert" },
        { TOK_KW_GENERIC, "_Generic" },
        { TOK_KW_COMPLEX, "_Complex" },
        { TOK_KW_IMAGINARY, "_Imaginary" },
        { TOK_EOF, "" },
    };
    check_stream("inline _Static_assert _Generic _Complex _Imaginary\n", kw_exp,
                 sizeof(kw_exp) / sizeof(*kw_exp), "C11 keywords are classified");

    const ExpectedToken keyword_prefix_exp[] = {
        { TOK_IDENTIFIER, "inline_value" },
        { TOK_IDENTIFIER, "returning" },
        { TOK_IDENTIFIER, "_GenericValue" },
        { TOK_EOF, "" },
    };
    check_stream("inline_value returning _GenericValue\n", keyword_prefix_exp,
                 sizeof(keyword_prefix_exp) / sizeof(*keyword_prefix_exp),
                 "keyword prefixes remain identifiers");

#if GNU_EXTENSION
    const ExpectedToken dollar_exp[] = {
        { TOK_IDENTIFIER, "$dollar" },
        { TOK_EOF, "" },
    };
    check_stream("$dollar\n", dollar_exp, sizeof(dollar_exp) / sizeof(*dollar_exp),
                 "dollar sign is an identifier character");
#endif

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
    Token *tokens3 = tokenize_text(src3, strlen(src3));
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
        { TOK_KW_INT, "int" }, { TOK_IDENTIFIER, "a" }, { TOK_SEMICOLON, ";" },
        { TOK_KW_INT, "int" }, { TOK_IDENTIFIER, "b" }, { TOK_SEMICOLON, ";" },
        { TOK_EOF, "" },
    };
    check_stream_bytes(src4, sizeof(src4) - 1, nul_exp, sizeof(nul_exp) / sizeof(*nul_exp),
                       "NUL bytes are ignored");

    // NUL inside a comment is data: it must not end the comment early.
    char src5[] = "int a; // c\0 @\n";
    const ExpectedToken line_comment_exp[] = {
        { TOK_KW_INT, "int" },
        { TOK_IDENTIFIER, "a" },
        { TOK_SEMICOLON, ";" },
        { TOK_EOF, "" },
    };
    check_stream_bytes(src5, sizeof(src5) - 1, line_comment_exp,
                       sizeof(line_comment_exp) / sizeof(*line_comment_exp),
                       "NUL does not end a line comment");

    char src6[] = "int a; /* c\0*/ int b;";
    const ExpectedToken block_comment_exp[] = {
        { TOK_KW_INT, "int" }, { TOK_IDENTIFIER, "a" }, { TOK_SEMICOLON, ";" },
        { TOK_KW_INT, "int" }, { TOK_IDENTIFIER, "b" }, { TOK_SEMICOLON, ";" },
        { TOK_EOF, "" },
    };
    check_stream_bytes(src6, sizeof(src6) - 1, block_comment_exp,
                       sizeof(block_comment_exp) / sizeof(*block_comment_exp),
                       "NUL does not hide a block comment terminator");

    // NUL inside a literal is preserved (GCC keeps it too).
    char src7[] = "char s[] = \"a\0b\";";
    Token *tokens7 = tokenize_text(src7, sizeof(src7) - 1);
    Token *t7 = tokens7;
    while (t7 && t7->kind != TOK_STRING)
        t7 = t7->next;
    if (t7 && t7->str_len == 3 && t7->str[0] == 'a' && t7->str[1] == '\0' && t7->str[2] == 'b') {
        printf("ok   : NUL inside a string literal is preserved\n");
    } else {
        printf("FAIL : NUL inside a string literal was not preserved\n");
        failures++;
    }
    token_free(tokens7);

    char src8[] = "'a\0b'";
    Token *tokens8 = tokenize_text(src8, sizeof(src8) - 1);
    if (tokens8 && tokens8->kind == TOK_CHARACTER && tokens8->value.integer == 0x610062) {
        printf("ok   : NUL inside a character constant packs as a byte\n");
    } else {
        printf("FAIL : NUL inside a character constant\n");
        failures++;
    }
    token_free(tokens8);

    // Last diagnostic check: \x without hex digits reports and contributes no
    // bytes. Earlier checks must stay error-free, so error_count() is still 0.
    SourceFile err_file = { 0 };
    Token *bad = literal_token("\"\\x\"", &err_file);
    if (bad && bad->kind == TOK_STRING && bad->str_len == 0 && error_count() > 0) {
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

static void dump_tokens(Token *tok)
{
    for (; tok; tok = tok->next) {
        printf("%-18s", token_kind_name(tok->kind));
        fwrite(string_view_begin(tok->span.text), 1,
               string_view_length(tok->span.text), stdout);
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

    return error_count() > 0 ? 1 : 0;
}

#endif
