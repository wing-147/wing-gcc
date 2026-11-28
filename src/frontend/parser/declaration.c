// C11 Annex A.2.2 declaration grammar and A.2.4 function-definition leaf.
// Declarator parsing directly builds the owned recursive shape used by AST.

#include "parser_internal.h"

#include "util/common.h"

typedef enum {
    SPEC_SIGNED,
    SPEC_UNSIGNED,
    SPEC_SHORT,
    SPEC_INT,
    SPEC_LONG,
    SPEC_CHAR,
    SPEC_FLOAT,
    SPEC_DOUBLE,
    SPEC_COMPLEX,
    SPEC_VOID,
    SPEC_BOOL,
    SPEC_COUNT,
} TypeSpecifierSlot;

typedef struct {
    const Token *tok[SPEC_COUNT][3];
    unsigned count[SPEC_COUNT];
    TypeSpecifier *other;
    const Token *other_tok;
    unsigned other_count;
    unsigned qualifiers;
    const Token *last;
} TypeSpecifiers;

typedef struct {
    TypeSpecifiers types;
    StorageClass storage;
    const Token *storage_tok;
    unsigned storage_count;
    const Token *thread_local_tok;
    unsigned thread_local_count;
    unsigned function_specifiers;
    const Token *function_specifier_tok;
    AlignmentSpecifier *alignments;
    AlignmentSpecifier *alignment_tail;
    const Token *alignment_tok;
    const Token *last;
} DeclarationSpecifiersState;

typedef struct {
    TypeSpecifier *type;
    unsigned qualifiers;
    AlignmentSpecifier *alignments;
    const Token *start;
    const Token *end;
} ParsedSpecifierQualifierList;

typedef bool (*SpecifierParseFn)(ParserCursor *cursor, TypeSpecifiers *out);
typedef bool (*DeclarationSpecifierStartsFn)(
    const ParserCursor *cursor, const DeclarationSpecifiersState *state);
typedef bool (*DeclarationSpecifierParseFn)(
    ParserCursor *cursor, DeclarationSpecifiersState *out);
typedef bool (*DeclaratorParseFn)(ParserCursor *cursor,
                                  ParsedDeclarator *out);
typedef bool (*DeclarationParseFn)(ParserCursor *cursor,
                                   DeclarationContext context,
                                   const Token *context_anchor, Node **out);

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, SpecifierParseFn);
} SpecifierAlternative;

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(DeclarationSpecifierStartsFn,
                              DeclarationSpecifierParseFn);
} DeclarationSpecifierAlternative;

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, DeclaratorParseFn);
} DeclaratorAlternative;

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, DeclarationParseFn);
} DeclarationAlternative;

typedef struct {
    ParsedDeclarationSpecifiers specifiers;
    ParsedDeclarator declarator;
} ParsedParameterDeclaration;

typedef struct {
    ParsedDeclarator declarator;
    Node *initializer;
} ParsedInitDeclarator;

typedef struct {
    Node *parameters;
    FunctionParameterForm form;
    bool variadic;
} ParsedParameterClause;

typedef struct {
    AlignmentSpecifier *specifier;
    const Token *end;
} ParsedAlignmentOperand;

typedef bool (*ParameterClauseParseFn)(ParserCursor *cursor,
                                       ParsedParameterClause *out);

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, ParameterClauseParseFn);
} ParameterClauseAlternative;

typedef bool (*AlignmentOperandParseFn)(ParserCursor *cursor,
                                        const Token *keyword,
                                        ParsedAlignmentOperand *out);

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, AlignmentOperandParseFn);
} AlignmentOperandAlternative;

typedef enum {
    DECLARATOR_ENTITY_NONE,
    DECLARATOR_ENTITY_POINTER,
    DECLARATOR_ENTITY_ARRAY,
    DECLARATOR_ENTITY_FUNCTION,
} DeclaratorEntityKind;

typedef struct {
    DeclaratorEntityKind entity;
    FunctionParameterForm function_form;
} DeclaratorSummary;

static bool parse_declarator(ParserCursor *cursor, ParsedDeclarator *out,
                             NameScope **definition_parameters);
static bool parse_abstract_declarator(ParserCursor *cursor,
                                      ParsedDeclarator *out);
static bool parse_direct_abstract_declarator(ParserCursor *cursor,
                                             ParsedDeclarator *out);
static bool starts_abstract_declarator(const ParserCursor *cursor);
static bool starts_direct_abstract_declarator(const ParserCursor *cursor);
static bool parse_type_qualifier_list(ParserCursor *cursor,
                                      TypeSpecifiers *out);
static bool parse_designation(ParserCursor *cursor, Node **out);
static bool parse_designator_list(ParserCursor *cursor, Node **out);
static bool parse_designator(ParserCursor *cursor, Node **out);
static const Token *parse_struct_or_union(ParserCursor *cursor);
static bool parse_struct_declaration_list(ParserCursor *cursor, Node **out);
static bool parse_struct_declaration(ParserCursor *cursor, Node **out);
static bool parse_struct_member_declaration(ParserCursor *cursor, Node **out);
static bool parse_struct_declarator_list(ParserCursor *cursor, Node **out);
static bool parse_struct_declarator(ParserCursor *cursor, Node **out);
static bool starts_struct_declarator(const ParserCursor *cursor);
static bool parse_enumerator_list(ParserCursor *cursor, Node **out);
static bool parse_enumerator(ParserCursor *cursor, Node **out);
static Node *parse_static_assert_declaration(ParserCursor *cursor);
static bool starts_static_assert_declaration(const ParserCursor *cursor);
static bool parse_declaration_list(ParserCursor *cursor, Node **out);

// Exact-terminal-to-slot map for the implemented builtin type specifiers.
static const struct {
    TokenKind kind;
    unsigned index;
} g_type_specifier_map[] = {
    { TOK_KW_SIGNED, SPEC_SIGNED },   { TOK_KW_UNSIGNED, SPEC_UNSIGNED },
    { TOK_KW_SHORT, SPEC_SHORT },     { TOK_KW_INT, SPEC_INT },
    { TOK_KW_LONG, SPEC_LONG },       { TOK_KW_CHAR, SPEC_CHAR },
    { TOK_KW_FLOAT, SPEC_FLOAT },     { TOK_KW_DOUBLE, SPEC_DOUBLE },
    { TOK_KW_COMPLEX, SPEC_COMPLEX }, { TOK_KW_VOID, SPEC_VOID },
    { TOK_KW_BOOL, SPEC_BOOL },
};


// Mask of the collected keywords, keyed with the number of `long` tokens. The
// count is part of the key because `long int` and `long long int` differ only
// there. Only combinations C11 6.7.2 allows appear here; anything else is
// rejected with gcc's "two or more data types" wording.
static const struct {
    unsigned mask;
    unsigned long_count;
    TypeKind type;
} g_type_combinations[] = {
    { 1u << SPEC_VOID, 0, TYPE_VOID },
    { 1u << SPEC_BOOL, 0, TYPE_BOOL },
    { 1u << SPEC_CHAR, 0, TYPE_CHAR },
    { (1u << SPEC_SIGNED) | (1u << SPEC_CHAR), 0, TYPE_SCHAR },
    { (1u << SPEC_UNSIGNED) | (1u << SPEC_CHAR), 0, TYPE_UCHAR },
    { 1u << SPEC_SHORT, 0, TYPE_SHORT },
    { (1u << SPEC_SHORT) | (1u << SPEC_INT), 0, TYPE_SHORT },
    { (1u << SPEC_SIGNED) | (1u << SPEC_SHORT), 0, TYPE_SHORT },
    { (1u << SPEC_SIGNED) | (1u << SPEC_SHORT) | (1u << SPEC_INT), 0, TYPE_SHORT },
    { (1u << SPEC_UNSIGNED) | (1u << SPEC_SHORT), 0, TYPE_USHORT },
    { (1u << SPEC_UNSIGNED) | (1u << SPEC_SHORT) | (1u << SPEC_INT), 0, TYPE_USHORT },
    { 1u << SPEC_INT, 0, TYPE_INT },
    { 1u << SPEC_SIGNED, 0, TYPE_INT },
    { (1u << SPEC_SIGNED) | (1u << SPEC_INT), 0, TYPE_INT },
    { 1u << SPEC_UNSIGNED, 0, TYPE_UINT },
    { (1u << SPEC_UNSIGNED) | (1u << SPEC_INT), 0, TYPE_UINT },
    { 1u << SPEC_FLOAT, 0, TYPE_FLOAT },
    { 1u << SPEC_DOUBLE, 0, TYPE_DOUBLE },
    { (1u << SPEC_FLOAT) | (1u << SPEC_COMPLEX), 0, TYPE_FLOAT_COMPLEX },
    { (1u << SPEC_DOUBLE) | (1u << SPEC_COMPLEX), 0, TYPE_DOUBLE_COMPLEX },
    { 0, 1, TYPE_LONG },
    { 1u << SPEC_INT, 1, TYPE_LONG },
    { 1u << SPEC_SIGNED, 1, TYPE_LONG },
    { (1u << SPEC_SIGNED) | (1u << SPEC_INT), 1, TYPE_LONG },
    { 1u << SPEC_UNSIGNED, 1, TYPE_ULONG },
    { (1u << SPEC_UNSIGNED) | (1u << SPEC_INT), 1, TYPE_ULONG },
    { 1u << SPEC_DOUBLE, 1, TYPE_LDOUBLE },
    { (1u << SPEC_DOUBLE) | (1u << SPEC_COMPLEX), 1,
      TYPE_LDOUBLE_COMPLEX },
    { 0, 2, TYPE_LLONG },
    { 1u << SPEC_INT, 2, TYPE_LLONG },
    { 1u << SPEC_SIGNED, 2, TYPE_LLONG },
    { (1u << SPEC_SIGNED) | (1u << SPEC_INT), 2, TYPE_LLONG },
    { 1u << SPEC_UNSIGNED, 2, TYPE_ULLONG },
    { (1u << SPEC_UNSIGNED) | (1u << SPEC_INT), 2, TYPE_ULLONG },
};

// Builtin keywords are immutable language syntax. Future typedef-names belong
// to a scoped ordinary-identifier table and must not mutate this mapping.
static bool lookup_builtin_type_specifier(const Token *tok, unsigned *slot)
{
    for (size_t i = 0; i < ARRAY_LEN(g_type_specifier_map); i++) {
        if (!token_is(tok, g_type_specifier_map[i].kind))
            continue;
        *slot = g_type_specifier_map[i].index;
        return true;
    }
    return false;
}

// Pure lookup for the implemented C11 6.7.1 subset; it neither consumes the
// token nor mutates declaration state, so lookahead and parsing share it.
static bool lookup_storage_class(const Token *tok, StorageClass *storage)
{
    static const struct {
        TokenKind kind;
        StorageClass storage;
    } map[] = {
        { TOK_KW_TYPEDEF, STORAGE_TYPEDEF },
        { TOK_KW_EXTERN, STORAGE_EXTERN },
        { TOK_KW_STATIC, STORAGE_STATIC },
        { TOK_KW_THREAD_LOCAL, STORAGE_THREAD_LOCAL },
        { TOK_KW_AUTO, STORAGE_AUTO },
        { TOK_KW_REGISTER, STORAGE_REGISTER },
    };

    for (size_t i = 0; i < ARRAY_LEN(map); i++) {
        if (!token_is(tok, map[i].kind))
            continue;
        *storage = map[i].storage;
        return true;
    }
    return false;
}

// Pure lookup shared by type-qualifier parsing and FIRST-set selection.
static bool lookup_type_qualifier(const Token *tok, TypeQualifier *qualifier)
{
    static const struct {
        TokenKind kind;
        TypeQualifier qualifier;
    } map[] = {
        { TOK_KW_CONST, TYPE_QUAL_CONST },
        { TOK_KW_RESTRICT, TYPE_QUAL_RESTRICT },
        { TOK_KW_VOLATILE, TYPE_QUAL_VOLATILE },
        { TOK_KW_ATOMIC, TYPE_QUAL_ATOMIC },
    };

    for (size_t i = 0; i < ARRAY_LEN(map); i++) {
        if (!token_is(tok, map[i].kind))
            continue;
        *qualifier = map[i].qualifier;
        return true;
    }
    return false;
}

// Pure lookup shared by function-specifier parsing and FIRST-set selection.
static bool lookup_function_specifier(const Token *tok, unsigned *specifier)
{
    static const struct {
        TokenKind kind;
        unsigned specifier;
    } map[] = {
        { TOK_KW_INLINE, FUNCTION_SPEC_INLINE },
        { TOK_KW_NORETURN, FUNCTION_SPEC_NORETURN },
    };

    for (size_t i = 0; i < ARRAY_LEN(map); i++) {
        if (!token_is(tok, map[i].kind))
            continue;
        *specifier = map[i].specifier;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// A.2.2 Declarations (C11 6.7)
// ---------------------------------------------------------------------------

static void record_type_specifier(TypeSpecifiers *specs, const Token *tok)
{
    unsigned index;
    if (!lookup_builtin_type_specifier(tok, &index))
        unreachable();
    if (specs->count[index] < 3)
        specs->tok[index][specs->count[index]] = tok;
    specs->count[index]++;
    specs->last = tok;
}

// Record one non-builtin type-specifier in the shared accumulator. The first
// syntax object remains the unique owner; later incompatible specifiers are
// counted and released for resolve_type_specifiers() to diagnose.
static void record_other_type_specifier(TypeSpecifiers *specs,
                                        TypeSpecifier *type,
                                        const Token *anchor,
                                        const Token *last)
{
    specs->other_count++;
    specs->other_tok = anchor;
    specs->last = last;
    if (!specs->other)
        specs->other = type;
    else
        type_specifier_free(type);
}

// Release declaration-specifier state that failed before ownership could move
// into DeclarationSpecifiers.
static void discard_declaration_specifiers_state(
    DeclarationSpecifiersState *state)
{
    type_specifier_free(state->types.other);
    state->types.other = NULL;
    alignment_specifier_free(state->alignments);
    state->alignments = NULL;
    state->alignment_tail = NULL;
}

// Query whether the accumulator already contains a type-specifier, including
// a structured or typedef-backed type held outside the builtin slots.
static bool type_specifiers_have_type(const TypeSpecifiers *specifiers)
{
    if (specifiers->other_count)
        return true;
    for (unsigned i = 0; i < SPEC_COUNT; i++) {
        if (specifiers->count[i])
            return true;
    }
    return false;
}

// C11 6.7.2 constraints: combine the collected type specifiers into one
// syntax object. On success ownership of `other` moves out of the accumulator;
// on failure the caller retains and releases every accumulated object.
static bool resolve_type_specifiers(TypeSpecifiers *specs, const Token *type_tok,
                                    TypeSpecifier **type,
                                    TypeSpecifierError *error)
{
    *error = (TypeSpecifierError){ .kind = TYPE_SPEC_OK };
    unsigned builtin_count = 0;
    for (unsigned i = 0; i < SPEC_COUNT; i++)
        builtin_count += specs->count[i];
    if (specs->other_count) {
        if (specs->other_count != 1 || builtin_count) {
            *error = (TypeSpecifierError){ TYPE_SPEC_INVALID_COMBINATION,
                                          specs->other_tok };
            return false;
        }
        *type = specs->other;
        specs->other = NULL;
        return true;
    }
    for (unsigned i = 0; i < SPEC_COUNT; i++) {
        // `long long` is the one legal repetition; count > 2 is diagnosed below.
        if (i == SPEC_LONG)
            continue;
        if (specs->count[i] > 1) {
            *error = (TypeSpecifierError){ TYPE_SPEC_DUPLICATE,
                                          specs->tok[i][1] };
            return false;
        }
    }
    if (specs->count[SPEC_SIGNED] && specs->count[SPEC_UNSIGNED]) {
        *error = (TypeSpecifierError){ TYPE_SPEC_SIGN_CONFLICT,
                                      specs->last ? specs->last : type_tok };
        return false;
    }
    if (specs->count[SPEC_LONG] > 2) {
        *error = (TypeSpecifierError){ TYPE_SPEC_TOO_MANY_LONG,
                                      specs->tok[SPEC_LONG][2] };
        return false;
    }
    const Token *anchor = specs->last ? specs->last : type_tok;
    if (specs->count[SPEC_LONG] > 1 && specs->count[SPEC_DOUBLE]) {
        *error = (TypeSpecifierError){ TYPE_SPEC_LONG_DOUBLE_CONFLICT, anchor };
        return false;
    }

    unsigned mask = 0;
    for (unsigned i = 0; i < SPEC_COUNT; i++) {
        if (i != SPEC_LONG && specs->count[i])
            mask |= 1u << i;
    }
    for (size_t i = 0; i < ARRAY_LEN(g_type_combinations); i++) {
        if (g_type_combinations[i].mask == mask &&
            g_type_combinations[i].long_count == specs->count[SPEC_LONG]) {
            const Token *end = specs->last ? specs->last : type_tok;
            *type = new_builtin_type_specifier(g_type_combinations[i].type,
                                               type_tok, end);
            return true;
        }
    }
    *error = (TypeSpecifierError){ TYPE_SPEC_INVALID_COMBINATION, anchor };
    return false;
}

// (6.7.2.4) atomic-type-specifier:
//     _Atomic ( type-name )
static bool parse_atomic_type_specifier(ParserCursor *cursor, TypeSpecifiers *out)
{
    Token *keyword = consume(cursor);
    if (!expect(cursor, TOK_LPAREN))
        return false;
    TypeName *type_name = NULL;
    if (!starts_type_name(cursor)) {
        handle_parser_error_expected_syntax(current_token(cursor), "type name");
        return false;
    }
    if (!parse_type_name(cursor, &type_name))
        return false;
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        type_name_free(type_name);
        return false;
    }
    record_other_type_specifier(
        out, new_atomic_type_specifier(keyword, type_name, close), keyword,
        close);
    return true;
}

static bool starts_atomic_type_specifier(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_ATOMIC) &&
           token_is(peek_token(cursor, 1), TOK_LPAREN);
}

// (6.7.2.1) struct-or-union-specifier:
//     struct-or-union identifieropt { struct-declaration-list }
//     struct-or-union identifier
static bool parse_struct_or_union_specifier(ParserCursor *cursor,
                                            TypeSpecifiers *out)
{
    const Token *keyword = parse_struct_or_union(cursor);

    const Token *tag = NULL;
    if (token_is(current_token(cursor), TOK_IDENTIFIER))
        tag = consume(cursor);

    Node *members = NULL;
    Token *close = NULL;
    if (consume_if(cursor, TOK_LBRACE)) {
        if (!parse_struct_declaration_list(cursor, &members))
            return false;
        close = expect(cursor, TOK_RBRACE);
        if (!close) {
            node_free(members);
            return false;
        }
    } else if (!tag) {
        (void)expect_identifier(cursor);
        return false;
    }

    TypeSpecifierKind kind = token_is(keyword, TOK_KW_STRUCT)
                                 ? TYPE_SPECIFIER_STRUCT
                                 : TYPE_SPECIFIER_UNION;
    TypeSpecifier *type =
        new_aggregate_type_specifier(kind, keyword, tag, members, close);
    record_other_type_specifier(out, type, keyword, close ? close : tag);
    return true;
}

static bool starts_struct_or_union_specifier(const ParserCursor *cursor)
{
    const Token *tok = current_token(cursor);
    return token_is(tok, TOK_KW_STRUCT) ||
           token_is(tok, TOK_KW_UNION);
}

// (6.7.2.2) enum-specifier:
//     enum identifieropt { enumerator-list }
//     enum identifieropt { enumerator-list , }
//     enum identifier
static bool parse_enum_specifier(ParserCursor *cursor, TypeSpecifiers *out)
{
    Token *keyword = consume(cursor);
    Token *tag = NULL;
    if (token_is(current_token(cursor), TOK_IDENTIFIER))
        tag = consume(cursor);

    Node *enumerators = NULL;
    Token *close = NULL;
    if (consume_if(cursor, TOK_LBRACE)) {
        if (!parse_enumerator_list(cursor, &enumerators))
            return false;
        (void)consume_if(cursor, TOK_COMMA);
        close = expect(cursor, TOK_RBRACE);
        if (!close) {
            node_free(enumerators);
            return false;
        }
    } else if (!tag) {
        (void)expect_identifier(cursor);
        return false;
    }

    TypeSpecifier *type =
        new_enum_type_specifier(keyword, tag, enumerators, close);
    record_other_type_specifier(out, type, keyword, close ? close : tag);
    return true;
}

static bool starts_enum_specifier(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_ENUM);
}

// (6.7.8) typedef-name:
//     identifier
static bool parse_typedef_name(ParserCursor *cursor, TypeSpecifiers *out)
{
    Token *name = consume(cursor);
    record_other_type_specifier(out, new_typedef_type_specifier(name), name,
                                name);
    return true;
}

static bool starts_typedef_name(const ParserCursor *cursor)
{
    return parser_name_is_typedef(cursor, current_token(cursor));
}

// Branches of (6.7.2) type-specifier:
//     void
//     char
//     short
//     int
//     long
//     float
//     double
//     signed
//     unsigned
//     _Bool
//     _Complex
// Owned only by parse_type_specifier(); this terminal-only branch keeps the
// builtin keyword table separate from structured specifier productions.
static bool parse_builtin_type_specifier(ParserCursor *cursor,
                                         TypeSpecifiers *out)
{
    record_type_specifier(out, consume(cursor));
    return true;
}

static bool starts_builtin_type_specifier(const ParserCursor *cursor)
{
    unsigned slot;
    return lookup_builtin_type_specifier(current_token(cursor), &slot);
}

static const SpecifierAlternative g_type_specifier_alternatives[] = {
    { starts_atomic_type_specifier, parse_atomic_type_specifier },
    { starts_struct_or_union_specifier, parse_struct_or_union_specifier },
    { starts_enum_specifier, parse_enum_specifier },
    { starts_typedef_name, parse_typedef_name },
    { starts_builtin_type_specifier, parse_builtin_type_specifier },
};

static bool starts_type_specifier(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_type_specifier_alternatives); i++) {
        if (g_type_specifier_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.7.2) type-specifier:
//     void
//     char
//     short
//     int
//     long
//     float
//     double
//     signed
//     unsigned
//     _Bool
//     _Complex
//     atomic-type-specifier
//     struct-or-union-specifier
//     enum-specifier
//     typedef-name
// One stable dispatcher owns every C11 type-specifier alternative. Its selected
// leaf is committed and reports only success or syntax error.
static bool parse_type_specifier(ParserCursor *cursor, TypeSpecifiers *out)
{
    for (size_t i = 0; i < ARRAY_LEN(g_type_specifier_alternatives); i++) {
        if (g_type_specifier_alternatives[i].starts(cursor))
            return g_type_specifier_alternatives[i].parse(cursor, out);
    }
    unreachable();
}

// (6.7.1) storage-class-specifier:
//     typedef
//     extern
//     static
//     _Thread_local
//     auto
//     register
static bool parse_storage_class_specifier(
    ParserCursor *cursor, DeclarationSpecifiersState *out)
{
    StorageClass storage;
    if (!lookup_storage_class(current_token(cursor), &storage))
        unreachable();
    const Token *tok = consume(cursor);
    out->storage |= storage;
    out->last = tok;
    if (storage == STORAGE_THREAD_LOCAL) {
        if (!out->thread_local_tok)
            out->thread_local_tok = tok;
        out->thread_local_count++;
        return true;
    }
    if (!out->storage_tok)
        out->storage_tok = tok;
    out->storage_count++;
    return true;
}

// Pure lookahead counterpart of parse_storage_class_specifier(); it recognizes
// exactly the same implemented subset without consuming input.
static bool starts_storage_class_specifier(const ParserCursor *cursor)
{
    StorageClass storage;
    return lookup_storage_class(current_token(cursor), &storage);
}

// (6.7.3) type-qualifier:
//     const
//     restrict
//     volatile
//     _Atomic
static bool parse_type_qualifier(ParserCursor *cursor, TypeSpecifiers *out)
{
    TypeQualifier qualifier;
    if (!lookup_type_qualifier(current_token(cursor), &qualifier))
        unreachable();
    out->qualifiers |= qualifier;
    out->last = consume(cursor);
    return true;
}

static bool starts_type_qualifier(const ParserCursor *cursor)
{
    TypeQualifier qualifier;
    return lookup_type_qualifier(current_token(cursor), &qualifier);
}

// Pure lookahead for the first token of C11 6.7.2.1
// specifier-qualifier-list; type-name consumers share this boundary.
static bool starts_specifier_qualifier_list(const ParserCursor *cursor);

// (6.7.4) function-specifier:
//     inline
//     _Noreturn
static bool parse_function_specifier(ParserCursor *cursor,
                                     DeclarationSpecifiersState *out)
{
    const Token *tok = current_token(cursor);
    unsigned flag;
    if (!lookup_function_specifier(tok, &flag))
        unreachable();
    consume(cursor);
    out->function_specifiers |= flag;
    if (!out->function_specifier_tok)
        out->function_specifier_tok = tok;
    out->last = tok;
    return true;
}

static bool starts_function_specifier(const ParserCursor *cursor)
{
    unsigned specifier;
    return lookup_function_specifier(current_token(cursor), &specifier);
}

// Branch of (6.7.5) alignment-specifier:
//     _Alignas ( type-name )
// Owned only by parse_alignment_specifier(); the parent already owns `_Alignas
// (` and this branch completes the selected type-name operand and close.
static bool parse_type_alignment_operand(ParserCursor *cursor,
                                         const Token *keyword,
                                         ParsedAlignmentOperand *out)
{
    TypeName *type_name = NULL;
    if (!parse_type_name(cursor, &type_name))
        return false;
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        type_name_free(type_name);
        return false;
    }
    out->specifier = new_type_alignment_specifier(keyword, type_name, close);
    out->end = close;
    return true;
}

// Branch of (6.7.5) alignment-specifier:
//     _Alignas ( constant-expression )
// Owned only by parse_alignment_specifier(); the parent already owns `_Alignas
// (` and this branch completes the selected expression operand and close.
static bool parse_expression_alignment_operand(ParserCursor *cursor,
                                               const Token *keyword,
                                               ParsedAlignmentOperand *out)
{
    Node *expression = parse_constant_expression(cursor);
    if (!expression)
        return false;
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        node_free(expression);
        return false;
    }
    out->specifier =
        new_expression_alignment_specifier(keyword, expression, close);
    out->end = close;
    return true;
}

static const AlignmentOperandAlternative g_alignment_operand_alternatives[] = {
    { starts_type_name, parse_type_alignment_operand },
    { starts_assignment_expression, parse_expression_alignment_operand },
};

// (6.7.5) alignment-specifier:
//     _Alignas ( type-name )
//     _Alignas ( constant-expression )
static bool parse_alignment_specifier(ParserCursor *cursor,
                                      DeclarationSpecifiersState *out)
{
    Token *keyword = consume(cursor);
    if (!expect(cursor, TOK_LPAREN))
        return false;

    const AlignmentOperandAlternative *alternative = NULL;
    for (size_t i = 0; i < ARRAY_LEN(g_alignment_operand_alternatives); i++) {
        if (g_alignment_operand_alternatives[i].starts(cursor)) {
            alternative = &g_alignment_operand_alternatives[i];
            break;
        }
    }
    if (!alternative) {
        handle_parser_error_expected_expression(current_token(cursor));
        return false;
    }
    ParsedAlignmentOperand operand = {0};
    if (!alternative->parse(cursor, keyword, &operand))
        return false;
    out->last = operand.end;
    append_alignment_specifier(&out->alignments, &out->alignment_tail,
                               operand.specifier);
    if (!out->alignment_tok)
        out->alignment_tok = keyword;
    return true;
}

static bool starts_alignment_specifier(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_ALIGNAS);
}

// Select a storage-class child for the current declaration-specifiers state.
static bool starts_declaration_storage_class(
    const ParserCursor *cursor, const DeclarationSpecifiersState *state)
{
    (void)state;
    return starts_storage_class_specifier(cursor);
}

// Select a type child while preserving typedef-name termination after a type.
static bool starts_type_specifier_in_state(
    const ParserCursor *cursor, const DeclarationSpecifiersState *state)
{
    return starts_type_specifier(cursor) &&
           !(starts_typedef_name(cursor) &&
             type_specifiers_have_type(&state->types));
}

// Non-grammar adapter shared by the two specifier lists. Retain the
// whole list's endpoint after the type-specifier child succeeds.
static bool parse_type_specifier_in_state(
    ParserCursor *cursor, DeclarationSpecifiersState *state)
{
    if (!parse_type_specifier(cursor, &state->types))
        return false;
    state->last = state->types.last;
    return true;
}

// Select a qualifier child in either specifier list.
static bool starts_type_qualifier_in_state(
    const ParserCursor *cursor, const DeclarationSpecifiersState *state)
{
    (void)state;
    return starts_type_qualifier(cursor);
}

// Non-grammar adapter shared by the two specifier lists. Retain the
// whole list's endpoint after the type-qualifier child succeeds.
static bool parse_type_qualifier_in_state(
    ParserCursor *cursor, DeclarationSpecifiersState *state)
{
    if (!parse_type_qualifier(cursor, &state->types))
        return false;
    state->last = state->types.last;
    return true;
}

// Select a function-specifier child for declaration-specifiers.
static bool starts_declaration_function_specifier(
    const ParserCursor *cursor, const DeclarationSpecifiersState *state)
{
    (void)state;
    return starts_function_specifier(cursor);
}

// Select an alignment-specifier child for declaration-specifiers.
static bool starts_declaration_alignment_specifier(
    const ParserCursor *cursor, const DeclarationSpecifiersState *state)
{
    (void)state;
    return starts_alignment_specifier(cursor);
}

// ---------------------------------------------------------------------------
// declaration-specifiers private region
// ---------------------------------------------------------------------------

static const DeclarationSpecifierAlternative
    g_declaration_specifier_alternatives[] = {
        { starts_declaration_storage_class,
          parse_storage_class_specifier },
        { starts_type_specifier_in_state, parse_type_specifier_in_state },
        { starts_type_qualifier_in_state, parse_type_qualifier_in_state },
        { starts_declaration_function_specifier,
          parse_function_specifier },
        { starts_declaration_alignment_specifier,
          parse_alignment_specifier },
    };

bool starts_declaration_specifiers(const ParserCursor *cursor)
{
    DeclarationSpecifiersState state = {0};
    for (size_t i = 0;
         i < ARRAY_LEN(g_declaration_specifier_alternatives); i++) {
        if (g_declaration_specifier_alternatives[i].starts(cursor, &state))
            return true;
    }
    return false;
}

// (6.7) declaration-specifiers:
//     storage-class-specifier declaration-specifiersopt
//     type-specifier declaration-specifiersopt
//     type-qualifier declaration-specifiersopt
//     function-specifier declaration-specifiersopt
//     alignment-specifier declaration-specifiersopt
// The right recursion is linearized into a loop. Each branch calls the exact
// child production named by Annex A instead of adapting them through a second
// set of pseudo-productions.
static bool parse_declaration_specifiers(
    ParserCursor *cursor, ParsedDeclarationSpecifiers *out)
{
    *out = (ParsedDeclarationSpecifiers){ .start = current_token(cursor) };
    DeclarationSpecifiersState state = {0};
    for (;;) {
        const DeclarationSpecifierAlternative *alternative = NULL;
        for (size_t i = 0;
             i < ARRAY_LEN(g_declaration_specifier_alternatives); i++) {
            if (g_declaration_specifier_alternatives[i].starts(cursor,
                                                               &state)) {
                alternative = &g_declaration_specifier_alternatives[i];
                break;
            }
        }
        if (!alternative)
            break;
        if (!alternative->parse(cursor, &state)) {
            discard_declaration_specifiers_state(&state);
            return false;
        }
    }
    StorageClass base_storage =
        state.storage & (StorageClass)~STORAGE_THREAD_LOCAL;
    bool has_thread_local = state.storage & STORAGE_THREAD_LOCAL;
    if (state.storage_count > 1 || state.thread_local_count > 1 ||
        (has_thread_local && base_storage != STORAGE_NONE &&
         base_storage != STORAGE_EXTERN && base_storage != STORAGE_STATIC)) {
        const Token *anchor = state.storage_count > 1
                                  ? state.storage_tok
                                  : state.thread_local_tok;
        handle_parser_error_multiple_storage_classes(anchor);
        discard_declaration_specifiers_state(&state);
        return false;
    }
    TypeSpecifierError error;
    TypeSpecifier *type = NULL;
    if (!resolve_type_specifiers(&state.types, out->start, &type, &error)) {
        handle_parser_error_type_specifier(&error);
        discard_declaration_specifiers_state(&state);
        return false;
    }
    out->storage_tok = state.storage_tok;
    out->thread_local_tok = state.thread_local_tok;
    out->function_specifier_tok = state.function_specifier_tok;
    out->alignment_tok = state.alignment_tok;
    out->end = state.last ? state.last : out->start;
    out->ast = new_declaration_specifiers(
        type, state.types.qualifiers, state.storage, state.function_specifiers,
        state.alignments, out->start, out->end);
    state.alignments = NULL;
    return true;
}

// ---------------------------------------------------------------------------
// Declarators and type names (C11 6.7.6-6.7.7)
// ---------------------------------------------------------------------------

// Pure lookahead for the declarator alternative of C11 6.7.6
// parameter-declaration. Only the pointer/parenthesized prefix is searched for
// its required identifier; suffix validity belongs to the committed parser.
static bool starts_declarator(const ParserCursor *cursor)
{
    ParserCursor lookahead = *cursor;
    for (;;) {
        while (consume_if(&lookahead, TOK_STAR)) {
            while (starts_type_qualifier(&lookahead))
                consume(&lookahead);
        }
        if (token_is(current_token(&lookahead), TOK_IDENTIFIER))
            return true;
        if (!consume_if(&lookahead, TOK_LPAREN))
            return false;
    }
}

// Pure FIRST-set query owned by pointer.
static bool starts_pointer(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_STAR);
}

// Branch of (6.7.6) parameter-declaration:
//     declaration-specifiers declarator
// Owned only by parse_parameter_declaration(); the parent has already parsed
// declaration-specifiers and this branch binds the completed name.
static bool parse_parameter_named_declarator(ParserCursor *cursor,
                                             ParsedDeclarator *out)
{
    if (!parse_declarator(cursor, out, NULL))
        return false;
    parser_declare_name(cursor,
                        out->name_tok ? &out->name_tok->span.text : NULL,
                        false);
    return true;
}

// Branch of (6.7.6) parameter-declaration:
//     declaration-specifiers abstract-declarator
// Owned only by parse_parameter_declaration(); the parent has already parsed
// declaration-specifiers.
static bool parse_parameter_abstract_declarator(ParserCursor *cursor,
                                                ParsedDeclarator *out)
{
    return parse_abstract_declarator(cursor, out);
}

// Select the empty abstract-declarator alternative after specifiers.
static bool starts_empty_parameter_declarator(const ParserCursor *cursor)
{
    (void)cursor;
    return true;
}

// Branch of (6.7.6) parameter-declaration:
//     declaration-specifiers
// Owned only by parse_parameter_declaration(); it realizes the optional
// abstract-declarator's empty form without consuming input.
static bool parse_empty_parameter_declarator(ParserCursor *cursor,
                                             ParsedDeclarator *out)
{
    (void)cursor;
    *out = (ParsedDeclarator){0};
    return true;
}

static const DeclaratorAlternative g_parameter_declarator_alternatives[] = {
    { starts_declarator, parse_parameter_named_declarator },
    { starts_abstract_declarator, parse_parameter_abstract_declarator },
    { starts_empty_parameter_declarator, parse_empty_parameter_declarator },
};

// (6.7.6) parameter-declaration:
//     declaration-specifiers declarator
//     declaration-specifiers abstract-declaratoropt
// Construct and validate the final parameter before transferring it to
// parameter-list.
static Node *parse_parameter_declaration(ParserCursor *cursor)
{
    ParsedParameterDeclaration parsed = {0};
    if (!parse_declaration_specifiers(cursor, &parsed.specifiers))
        return NULL;

    bool parsed_declarator = false;
    for (size_t i = 0;
         i < ARRAY_LEN(g_parameter_declarator_alternatives); i++) {
        if (g_parameter_declarator_alternatives[i].starts(cursor)) {
            parsed_declarator = g_parameter_declarator_alternatives[i].parse(
                cursor, &parsed.declarator);
            break;
        }
    }
    if (!parsed_declarator) {
        declaration_specifiers_free(parsed.specifiers.ast);
        return NULL;
    }
    validate_parameter_declaration_constraints(
        &parsed.specifiers, &parsed.declarator);
    return new_parameter(&parsed.specifiers, &parsed.declarator);
}

// Pure FIRST-set query owned by parameter-declaration. Its parent list uses
// this boundary instead of reaching into declaration-specifiers directly.
static bool starts_parameter_declaration(const ParserCursor *cursor)
{
    return starts_declaration_specifiers(cursor);
}

// Pure FIRST-set query owned by parameter-type-list and delegated through its
// required parameter-list child.
static bool starts_parameter_type_list(const ParserCursor *cursor)
{
    return starts_parameter_declaration(cursor);
}

// Recognize the two-token variadic suffix without consuming it, so
// parameter-list does not mistake its comma for another parameter.
static bool starts_variadic_parameter_suffix(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_COMMA) &&
           token_is(peek_token(cursor, 1), TOK_ELLIPSIS);
}

// Branch of (6.7.6) parameter-type-list:
//     parameter-list , ...
// Owned only by parse_parameter_type_list(); the parent has already selected
// this tail, so the branch only consumes and records its suffix.
static void parse_variadic_parameter_suffix(ParserCursor *cursor, bool *variadic)
{
    consume(cursor);
    consume(cursor);
    *variadic = true;
}

// Canonicalize only the valid `(void)` representation after constraint
// validation. Invalid lists retain every syntactic parameter in their AST.
static void discard_lone_void_parameter(Node **params)
{
    if (!*params || (*params)->next || !parameter_is_plain_void(*params))
        return;
    node_free(*params);
    *params = NULL;
}

// (6.7.6) parameter-list:
//     parameter-declaration
//     parameter-list , parameter-declaration
// parameter-list (C11 6.7.6): comma-separated parameter-declarations. Errors
// free the partial chain; the caller owns the surrounding tokens.
static bool parse_parameter_list(ParserCursor *cursor, Node **params_out)
{
    *params_out = NULL;
    Node *head = NULL;
    Node *tail = NULL;
    for (;;) {
        if (!starts_parameter_declaration(cursor)) {
            handle_parser_error_parameter_list(current_token(cursor));
            node_free(head);
            return false;
        }
        Node *param = parse_parameter_declaration(cursor);
        if (!param) {
            node_free(head);
            return false;
        }
        append_node_list(&head, &tail, param);
        ParserCursor lookahead = *cursor;
        (void)consume_if(&lookahead, TOK_COMMA);
        if (!token_is(current_token(cursor), TOK_COMMA) ||
            !starts_parameter_declaration(&lookahead))
            break;
        consume_if(cursor, TOK_COMMA);
    }
    *params_out = head;
    return true;
}

// (6.7.6) parameter-type-list:
//     parameter-list
//     parameter-list , ...
// parameter-type-list (C11 6.7.6): a lone `(void)` is normalized to no
// parameters, while `()` is handled by direct-declarator as identifier-listopt.
// The caller owns the parentheses; this production stops before the closing
// ')'.
static bool parse_parameter_type_list(ParserCursor *cursor, Node **params_out, bool *variadic_out)
{
    *params_out = NULL;
    *variadic_out = false;
    if (!parse_parameter_list(cursor, params_out))
        return false;

    if (starts_variadic_parameter_suffix(cursor))
        parse_variadic_parameter_suffix(cursor, variadic_out);
    else if (token_is(current_token(cursor), TOK_COMMA)) {
        handle_parser_error_parameter_expression(cursor);
        node_free(*params_out);
        *params_out = NULL;
        return false;
    }

    validate_parameter_type_list_constraints(*params_out);
    discard_lone_void_parameter(params_out);
    return true;
}

// (6.7.6) identifier-list:
//     identifier
//     identifier-list , identifier
// Pure FIRST-set query owned by identifier-list.
static bool starts_identifier_list(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_IDENTIFIER);
}

// (6.7.6) identifier-list:
//     identifier
//     identifier-list , identifier
static bool parse_identifier_list(ParserCursor *cursor, Node **out)
{
    *out = NULL;

    Node *head = NULL;
    Node *tail = NULL;
    for (;;) {
        Token *name = expect_identifier(cursor);
        if (!name) {
            node_free(head);
            return false;
        }
        append_node_list(&head, &tail, new_identifier(name));
        if (!consume_if(cursor, TOK_COMMA))
            break;
        if (!token_is(current_token(cursor), TOK_IDENTIFIER)) {
            (void)expect_identifier(cursor);
            node_free(head);
            return false;
        }
    }
    *out = head;
    return true;
}

// Branch of (6.7.6) direct-declarator:
//     ( declarator )
// Owned only by parse_direct_declarator(); it is split because the recursive
// form owns delimiters and nested cleanup.
static bool starts_direct_declarator_parenthesized(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LPAREN);
}

// Branch of (6.7.6) direct-declarator:
//     ( declarator )
// Owned only by parse_direct_declarator(); it owns the nested declarator's
// delimiters and failure cleanup.
static bool parse_direct_declarator_parenthesized(ParserCursor *cursor,
                                                  ParsedDeclarator *out)
{
    Declarator *outer_prefix = out->prefix;
    Token *open = consume(cursor);

    ParsedDeclarator nested;
    if (!parse_declarator(cursor, &nested, out->definition_parameters))
        return false;
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        discard_parsed_declarator(&nested);
        return false;
    }
    *out = nested;
    out->prefix = outer_prefix;
    parenthesize_declarator(out->shape, open, close);
    return true;
}

// Branch of (6.7.6) direct-declarator:
//     identifier
// Owned only by parse_direct_declarator(); it is the simple alternative paired
// with the recursively parenthesized base.
static bool starts_direct_declarator_identifier(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_IDENTIFIER);
}

// Branch of (6.7.6) direct-declarator:
//     identifier
// Owned only by parse_direct_declarator(); it constructs the required base
// before the parent attaches suffixes.
static bool parse_direct_declarator_identifier(ParserCursor *cursor,
                                               ParsedDeclarator *out)
{
    Token *name = consume(cursor);
    out->name_tok = name;
    out->shape = new_identifier_declarator(name);
    return true;
}

// Branches of (6.7.6) direct-declarator:
//     direct-declarator [ type-qualifier-listopt assignment-expressionopt ]
//     direct-declarator [ static type-qualifier-listopt assignment-expression ]
//     direct-declarator [ type-qualifier-list static assignment-expression ]
//     direct-declarator [ type-qualifier-listopt * ]
// Owned only by parse_direct_declarator(); they are split because the four
// array alternatives share qualifier and bound ownership.
static bool starts_direct_declarator_array_suffix(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LBRACKET);
}

// Branches of (6.7.6) direct-declarator:
//     direct-declarator [ type-qualifier-listopt assignment-expressionopt ]
//     direct-declarator [ static type-qualifier-listopt assignment-expression ]
//     direct-declarator [ type-qualifier-list static assignment-expression ]
//     direct-declarator [ type-qualifier-listopt * ]
// Owned only by parse_direct_declarator(); this branch replaces the completed
// base with one selected array form.
static bool parse_direct_declarator_array_suffix(ParserCursor *cursor,
                                                 ParsedDeclarator *out)
{
    Token *open = consume(cursor);

    TypeSpecifiers qualifiers = {0};
    ArrayDeclaratorForm form = ARRAY_DECLARATOR_BOUND;
    Node *size = NULL;
    if (consume_if(cursor, TOK_KW_STATIC)) {
        form = ARRAY_DECLARATOR_STATIC;
        if (starts_type_qualifier(cursor) &&
            !parse_type_qualifier_list(cursor, &qualifiers))
            return false;
        size = parse_assignment_expression(cursor);
        if (!size)
            return false;
    } else {
        if (starts_type_qualifier(cursor) &&
            !parse_type_qualifier_list(cursor, &qualifiers))
            return false;
        if (consume_if(cursor, TOK_KW_STATIC)) {
            form = ARRAY_DECLARATOR_STATIC;
            size = parse_assignment_expression(cursor);
            if (!size)
                return false;
        } else if (consume_if(cursor, TOK_STAR)) {
            form = ARRAY_DECLARATOR_STAR;
        } else if (!token_is(current_token(cursor), TOK_RBRACKET)) {
            size = parse_assignment_expression(cursor);
            if (!size)
                return false;
        }
    }
    Token *close = expect(cursor, TOK_RBRACKET);
    if (!close) {
        node_free(size);
        return false;
    }
    out->shape = new_array_declarator(out->shape, size, qualifiers.qualifiers,
                                      form, open, close);
    return true;
}

// Branch of (6.7.6) direct-declarator:
//     direct-declarator ( parameter-type-list )
// Owned only by parse_direct_declarator_function_suffix(); this adapter records
// the prototype representation selected for the enclosing suffix.
static bool parse_prototype_parameter_clause(ParserCursor *cursor,
                                             ParsedParameterClause *out)
{
    out->form = FUNCTION_PARAMS_PROTOTYPE;
    return parse_parameter_type_list(cursor, &out->parameters,
                                     &out->variadic);
}

// Branch of (6.7.6) direct-declarator:
//     direct-declarator ( identifier-list )
// Owned only by parse_direct_declarator_function_suffix(); this adapter records
// the old-style representation selected for the enclosing suffix.
static bool parse_identifier_parameter_clause(ParserCursor *cursor,
                                              ParsedParameterClause *out)
{
    out->form = FUNCTION_PARAMS_IDENTIFIER_LIST;
    return parse_identifier_list(cursor, &out->parameters);
}

static const ParameterClauseAlternative g_parameter_clause_alternatives[] = {
    { starts_parameter_type_list, parse_prototype_parameter_clause },
    { starts_identifier_list, parse_identifier_parameter_clause },
};

// Branches of (6.7.6) direct-declarator:
//     direct-declarator ( parameter-type-list )
//     direct-declarator ( identifier-listopt )
// Owned only by parse_direct_declarator(); they are split because prototype and
// identifier-list alternatives transfer different lists.
static bool starts_direct_declarator_function_suffix(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LPAREN);
}

// Branches of (6.7.6) direct-declarator:
//     direct-declarator ( parameter-type-list )
//     direct-declarator ( identifier-listopt )
// Owned only by parse_direct_declarator(); this branch selects the clause form
// and replaces the completed base with its function layer.
static bool parse_direct_declarator_function_suffix(ParserCursor *cursor,
                                                    ParsedDeclarator *out)
{
    Token *open = consume(cursor);
    Token *close = consume_if(cursor, TOK_RPAREN);
    if (close) {
        out->shape = new_function_declarator(out->shape, NULL,
                                             FUNCTION_PARAMS_IDENTIFIER_LIST,
                                             false,
                                             open, close);
        return true;
    }

    ParsedParameterClause clause = {0};
    parser_push_scope(cursor, NULL);
    const ParameterClauseAlternative *alternative = NULL;
    for (size_t i = 0; i < ARRAY_LEN(g_parameter_clause_alternatives); i++) {
        if (g_parameter_clause_alternatives[i].starts(cursor)) {
            alternative = &g_parameter_clause_alternatives[i];
            break;
        }
    }
    if (!alternative) {
        handle_parser_error_parameter_list(current_token(cursor));
        parser_pop_scope(cursor, NULL);
        return false;
    }
    if (!alternative->parse(cursor, &clause)) {
        parser_pop_scope(cursor, NULL);
        return false;
    }
    close = expect(cursor, TOK_RPAREN);
    if (!close) {
        node_free(clause.parameters);
        parser_pop_scope(cursor, NULL);
        return false;
    }
    NameScope **saved = out->shape->kind == DECLARATOR_IDENT ? out->definition_parameters : NULL;
    parser_pop_scope(cursor, saved);
    out->shape = new_function_declarator(
        out->shape, clause.parameters, clause.form, clause.variadic, open,
        close);
    return true;
}

// (6.7.6) direct-declarator:
//     identifier
//     ( declarator )
//     direct-declarator [ type-qualifier-listopt assignment-expressionopt ]
//     direct-declarator [ static type-qualifier-listopt assignment-expression ]
//     direct-declarator [ type-qualifier-list static assignment-expression ]
//     direct-declarator [ type-qualifier-listopt * ]
//     direct-declarator ( parameter-type-list )
//     direct-declarator ( identifier-listopt )
// Base and suffix tables reserve every C11 direct-declarator shape. A bare `()`
// is the empty identifier-listopt (old-style), distinct from `(void)`.
static bool parse_direct_declarator(ParserCursor *cursor, ParsedDeclarator *out)
{
    static const DeclaratorAlternative bases[] = {
        { starts_direct_declarator_parenthesized,
          parse_direct_declarator_parenthesized },
        { starts_direct_declarator_identifier,
          parse_direct_declarator_identifier },
    };
    static const DeclaratorAlternative suffixes[] = {
        { starts_direct_declarator_array_suffix,
          parse_direct_declarator_array_suffix },
        { starts_direct_declarator_function_suffix,
          parse_direct_declarator_function_suffix },
    };

    const DeclaratorAlternative *base = NULL;
    for (size_t i = 0; i < ARRAY_LEN(bases); i++) {
        if (bases[i].starts(cursor)) {
            base = &bases[i];
            break;
        }
    }
    if (!base) {
        (void)expect_identifier(cursor);
        return false;
    }
    if (!base->parse(cursor, out))
        return false;

    for (;;) {
        const DeclaratorAlternative *suffix = NULL;
        for (size_t i = 0; i < ARRAY_LEN(suffixes); i++) {
            if (suffixes[i].starts(cursor)) {
                suffix = &suffixes[i];
                break;
            }
        }
        if (!suffix)
            break;
        if (!suffix->parse(cursor, out))
            return false;
    }
    return true;
}

// (6.7.6) pointer:
//     * type-qualifier-listopt
//     * type-qualifier-listopt pointer
static bool parse_pointer(ParserCursor *cursor, ParsedDeclarator *out)
{
    Declarator **tail = &out->prefix;
    while (true) {
        Token *star = consume_if(cursor, TOK_STAR);
        if (!star)
            break;
        TypeSpecifiers qualifiers = {0};
        if (starts_type_qualifier(cursor) &&
            !parse_type_qualifier_list(cursor, &qualifiers))
            return false;
        Declarator *pointer = new_pointer_declarator(star, qualifiers.qualifiers);
        *tail = pointer;
        tail = &pointer->inner;
    }
    return true;
}

// (6.7.6) type-qualifier-list:
//     type-qualifier
//     type-qualifier-list type-qualifier
static bool parse_type_qualifier_list(ParserCursor *cursor, TypeSpecifiers *out)
{
    while (starts_type_qualifier(cursor)) {
        if (!parse_type_qualifier(cursor, out))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// specifier-qualifier-list private region
// ---------------------------------------------------------------------------

static const struct {
    DeclarationSpecifierAlternative alternative;
    bool member_only;
} g_specifier_qualifier_alternatives[] = {
    { { starts_type_specifier_in_state, parse_type_specifier_in_state }, false },
    { { starts_type_qualifier_in_state, parse_type_qualifier_in_state }, false },
    { { starts_declaration_alignment_specifier, parse_alignment_specifier }, true },
};

// Select a direct specifier alternative through the shared inventory.
static const DeclarationSpecifierAlternative *lookup_specifier_qualifier(
    const ParserCursor *cursor, const DeclarationSpecifiersState *state, bool member)
{
    for (size_t i = 0; i < ARRAY_LEN(g_specifier_qualifier_alternatives); ++i) {
        const DeclarationSpecifierAlternative *alternative =
            &g_specifier_qualifier_alternatives[i].alternative;
        if ((member || !g_specifier_qualifier_alternatives[i].member_only) &&
            alternative->starts(cursor, state))
            return alternative;
    }
    return NULL;
}

// Select the type-name variant of specifier-qualifier-list.
static bool starts_specifier_qualifier_list(const ParserCursor *cursor)
{
    DeclarationSpecifiersState state = {0};
    return lookup_specifier_qualifier(cursor, &state, false) != NULL;
}

// Select the structure-member variant, including alignment specifiers.
static bool starts_member_specifier_qualifier_list(const ParserCursor *cursor)
{
    DeclarationSpecifiersState state = {0};
    return lookup_specifier_qualifier(cursor, &state, true) != NULL;
}

// (6.7.2.1) specifier-qualifier-list:
//     type-specifier specifier-qualifier-listopt
//     type-qualifier specifier-qualifier-listopt
//     alignment-specifier specifier-qualifier-listopt (member only, DR 444)
// Resolve the completed list and transfer its owned specifiers.
static bool parse_specifier_qualifier_list(
    ParserCursor *cursor, ParsedSpecifierQualifierList *out, bool member)
{
    *out = (ParsedSpecifierQualifierList){ .start = current_token(cursor) };
    DeclarationSpecifiersState state = {0};
    const DeclarationSpecifierAlternative *alternative;
    while ((alternative = lookup_specifier_qualifier(cursor, &state, member))) {
        if (!alternative->parse(cursor, &state)) {
            discard_declaration_specifiers_state(&state);
            return false;
        }
    }
    TypeSpecifierError error;
    if (!resolve_type_specifiers(&state.types, out->start, &out->type, &error)) {
        handle_parser_error_type_specifier(&error);
        discard_declaration_specifiers_state(&state);
        return false;
    }
    out->qualifiers = state.types.qualifiers;
    out->alignments = state.alignments;
    out->end = state.last ? state.last : out->start;
    return true;
}

// (6.7.7) type-name:
//     specifier-qualifier-list abstract-declaratoropt
bool parse_type_name(ParserCursor *cursor, TypeName **out)
{
    *out = NULL;
    ParsedSpecifierQualifierList specs;
    if (!parse_specifier_qualifier_list(cursor, &specs, false)) {
        return false;
    }

    ParsedDeclarator declarator = {0};
    bool has_declarator = starts_abstract_declarator(cursor);
    if (has_declarator) {
        if (!parse_abstract_declarator(cursor, &declarator)) {
            type_specifier_free(specs.type);
            return false;
        }
        validate_declarator_constraints(&declarator, DECLARATOR_ORDINARY);
    }
    const SourceSpan *end = has_declarator ? &declarator.shape->span
                                          : &specs.end->span;
    *out = new_type_name(specs.type, specs.qualifiers, declarator.shape,
                         specs.start, end);
    specs.type = NULL;
    declarator.shape = NULL;
    return true;
}

// Pure FIRST-set query owned by type-name. Expression parents call it before
// committing to the type-name alternative.
bool starts_type_name(const ParserCursor *cursor)
{
    return starts_specifier_qualifier_list(cursor);
}

// (6.7.7) abstract-declarator:
//     pointer
//     pointeropt direct-abstract-declarator
static bool parse_abstract_declarator(ParserCursor *cursor,
                                      ParsedDeclarator *out)
{
    *out = (ParsedDeclarator){0};
    bool has_pointer = starts_pointer(cursor);
    if (has_pointer && !parse_pointer(cursor, out)) {
        discard_parsed_declarator(out);
        return false;
    }
    bool has_direct = starts_direct_abstract_declarator(cursor);
    if (has_direct && !parse_direct_abstract_declarator(cursor, out)) {
        discard_parsed_declarator(out);
        return false;
    }
    if (!has_pointer && !has_direct)
        unreachable();
    out->shape = attach_pointer_prefix(out->prefix, out->shape);
    out->prefix = NULL;
    return true;
}

// Branches of (6.7.7) direct-abstract-declarator:
//     direct-abstract-declaratoropt [ type-qualifier-listopt assignment-expressionopt ]
//     direct-abstract-declaratoropt [ static type-qualifier-listopt assignment-expression ]
//     direct-abstract-declaratoropt [ type-qualifier-list static assignment-expression ]
//     direct-abstract-declaratoropt [ * ]
// Owned only by parse_direct_abstract_declarator(); they remain separate
// because C11 6.7.7 permits `[*]`, but not the named-declarator form
// `[ type-qualifier-listopt * ]`.
static bool starts_direct_abstract_declarator_array_suffix(
    const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LBRACKET);
}

// Branches of (6.7.7) direct-abstract-declarator:
//     direct-abstract-declaratoropt [ type-qualifier-listopt assignment-expressionopt ]
//     direct-abstract-declaratoropt [ static type-qualifier-listopt assignment-expression ]
//     direct-abstract-declaratoropt [ type-qualifier-list static assignment-expression ]
//     direct-abstract-declaratoropt [ * ]
// Owned only by parse_direct_abstract_declarator(); this branch replaces the
// optional completed base with one selected array form.
static bool parse_direct_abstract_declarator_array_suffix(
    ParserCursor *cursor, ParsedDeclarator *out)
{
    Token *open = consume(cursor);

    TypeSpecifiers qualifiers = {0};
    ArrayDeclaratorForm form = ARRAY_DECLARATOR_BOUND;
    Node *size = NULL;
    if (consume_if(cursor, TOK_STAR)) {
        form = ARRAY_DECLARATOR_STAR;
    } else if (consume_if(cursor, TOK_KW_STATIC)) {
        form = ARRAY_DECLARATOR_STATIC;
        if (starts_type_qualifier(cursor) &&
            !parse_type_qualifier_list(cursor, &qualifiers))
            return false;
        size = parse_assignment_expression(cursor);
        if (!size)
            return false;
    } else {
        if (starts_type_qualifier(cursor) &&
            !parse_type_qualifier_list(cursor, &qualifiers))
            return false;
        if (consume_if(cursor, TOK_KW_STATIC)) {
            form = ARRAY_DECLARATOR_STATIC;
            size = parse_assignment_expression(cursor);
            if (!size)
                return false;
        } else if (!token_is(current_token(cursor), TOK_RBRACKET)) {
            size = parse_assignment_expression(cursor);
            if (!size)
                return false;
        }
    }
    Token *close = expect(cursor, TOK_RBRACKET);
    if (!close) {
        node_free(size);
        return false;
    }
    out->shape = new_array_declarator(out->shape, size, qualifiers.qualifiers,
                                      form, open, close);
    return true;
}

// Branch of (6.7.7) direct-abstract-declarator:
//     direct-abstract-declaratoropt ( parameter-type-listopt )
// Owned only by parse_direct_abstract_declarator(); unlike a direct-declarator
// suffix, this branch must never reinterpret an identifier-list as a type-name.
static bool starts_direct_abstract_declarator_function_suffix(
    const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LPAREN);
}

// Branch of (6.7.7) direct-abstract-declarator:
//     direct-abstract-declaratoropt ( parameter-type-listopt )
// Owned only by parse_direct_abstract_declarator(); this branch replaces the
// optional completed base with its function layer.
static bool parse_direct_abstract_declarator_function_suffix(
    ParserCursor *cursor, ParsedDeclarator *out)
{
    Token *open = consume(cursor);

    Token *close = consume_if(cursor, TOK_RPAREN);
    if (close) {
        out->shape = new_function_declarator(out->shape, NULL,
                                             FUNCTION_PARAMS_IDENTIFIER_LIST,
                                             false, open, close);
        return true;
    }

    Node *params = NULL;
    bool variadic = false;
    parser_push_scope(cursor, NULL);
    if (!starts_parameter_type_list(cursor)) {
        handle_parser_error_parameter_list(current_token(cursor));
        parser_pop_scope(cursor, NULL);
        return false;
    }
    if (!parse_parameter_type_list(cursor, &params, &variadic)) {
        parser_pop_scope(cursor, NULL);
        return false;
    }
    close = expect(cursor, TOK_RPAREN);
    if (!close) {
        node_free(params);
        parser_pop_scope(cursor, NULL);
        return false;
    }
    parser_pop_scope(cursor, NULL);
    out->shape = new_function_declarator(out->shape, params,
                                         FUNCTION_PARAMS_PROTOTYPE,
                                         variadic, open, close);
    return true;
}

// Pure FIRST-set query owned by abstract-declarator. Callers use this boundary
// instead of duplicating the child's pointer and direct-declarator starters.
static bool starts_abstract_declarator(const ParserCursor *cursor)
{
    return starts_pointer(cursor) || starts_direct_abstract_declarator(cursor);
}

// Pure FIRST-set query owned by direct-abstract-declarator.
static bool starts_direct_abstract_declarator(const ParserCursor *cursor)
{
    const Token *tok = current_token(cursor);
    return token_is(tok, TOK_LBRACKET) || token_is(tok, TOK_LPAREN);
}

// (6.7.7) direct-abstract-declarator:
//     ( abstract-declarator )
//     direct-abstract-declaratoropt [ type-qualifier-listopt assignment-expressionopt ]
//     direct-abstract-declaratoropt [ static type-qualifier-listopt assignment-expression ]
//     direct-abstract-declaratoropt [ type-qualifier-list static assignment-expression ]
//     direct-abstract-declaratoropt [ * ]
//     direct-abstract-declaratoropt ( parameter-type-listopt )
// Select the parenthesized abstract-declarator base through local lookahead.
static bool starts_direct_abstract_declarator_parenthesized(
    const ParserCursor *cursor)
{
    ParserCursor lookahead = *cursor;
    return consume_if(&lookahead, TOK_LPAREN) &&
           starts_abstract_declarator(&lookahead);
}

// Branch of (6.7.7) direct-abstract-declarator:
//     ( abstract-declarator )
// Owned only by parse_direct_abstract_declarator(); it owns the nested
// declarator's delimiters and failure cleanup.
static bool parse_direct_abstract_declarator_parenthesized(
    ParserCursor *cursor, ParsedDeclarator *out)
{
    Declarator *outer_prefix = out->prefix;
    Token *open = consume(cursor);
    ParsedDeclarator nested;
    if (!parse_abstract_declarator(cursor, &nested))
        return false;
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        discard_parsed_declarator(&nested);
        return false;
    }
    *out = nested;
    out->prefix = outer_prefix;
    parenthesize_declarator(out->shape, open, close);
    return true;
}

// (6.7.7) direct-abstract-declarator:
//     ( abstract-declarator )
//     direct-abstract-declaratoropt [ type-qualifier-listopt assignment-expressionopt ]
//     direct-abstract-declaratoropt [ static type-qualifier-listopt assignment-expression ]
//     direct-abstract-declaratoropt [ type-qualifier-list static assignment-expression ]
//     direct-abstract-declaratoropt [ * ]
//     direct-abstract-declaratoropt ( parameter-type-listopt )
static bool parse_direct_abstract_declarator(ParserCursor *cursor,
                                             ParsedDeclarator *out)
{
    bool matched = false;
    if (starts_direct_abstract_declarator_parenthesized(cursor)) {
        if (!parse_direct_abstract_declarator_parenthesized(cursor, out))
            return false;
        matched = true;
    }

    static const DeclaratorAlternative suffixes[] = {
        { starts_direct_abstract_declarator_array_suffix,
          parse_direct_abstract_declarator_array_suffix },
        { starts_direct_abstract_declarator_function_suffix,
          parse_direct_abstract_declarator_function_suffix },
    };
    for (;;) {
        const DeclaratorAlternative *suffix = NULL;
        for (size_t i = 0; i < ARRAY_LEN(suffixes); i++) {
            if (suffixes[i].starts(cursor)) {
                suffix = &suffixes[i];
                break;
            }
        }
        if (!suffix)
            break;
        if (!suffix->parse(cursor, out))
            return false;
        matched = true;
    }
    if (!matched)
        unreachable();
    return true;
}

// (6.7.6) declarator:
//     pointeropt direct-declarator
// Pointer prefixes are parsed separately and attached after the required
// direct-declarator so parentheses preserve C's binding rules.
static bool parse_declarator(ParserCursor *cursor, ParsedDeclarator *out,
                             NameScope **definition_parameters)
{
    *out = (ParsedDeclarator){ .definition_parameters = definition_parameters };
    if (starts_pointer(cursor) && !parse_pointer(cursor, out))
        return false;
    if (!parse_direct_declarator(cursor, out)) {
        discard_parsed_declarator(out);
        return false;
    }
    out->shape = attach_pointer_prefix(out->prefix, out->shape);
    out->prefix = NULL;
    return true;
}

// ---------------------------------------------------------------------------
// Initializers (C11 6.7.9)
// ---------------------------------------------------------------------------

// Pure FIRST-set query owned by designator.
static bool starts_designator(const ParserCursor *cursor);

// Pure FIRST-set query owned by designation and delegated to its required
// designator-list child.
static bool starts_designation(const ParserCursor *cursor)
{
    return starts_designator(cursor);
}

// Pure FIRST-set query owned by initializer.
static bool starts_initializer(const ParserCursor *cursor);

// Compose the direct children of one initializer-list item without copying
// either child's token-level FIRST set into the list production.
static bool starts_initializer_list_item(const ParserCursor *cursor)
{
    return starts_designation(cursor) || starts_initializer(cursor);
}

// (6.7.9) initializer-list:
//     designationopt initializer
//     initializer-list , designationopt initializer
bool parse_initializer_list(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    Node *head = NULL;
    Node *tail = NULL;
    for (;;) {
        if (!starts_initializer_list_item(cursor)) {
            handle_parser_error_expected_expression(current_token(cursor));
            node_free(head);
            return false;
        }
        Node *designators = NULL;
        if (starts_designation(cursor) &&
            !parse_designation(cursor, &designators)) {
            node_free(head);
            return false;
        }
        Node *value = parse_initializer(cursor);
        if (!value) {
            node_free(designators);
            node_free(head);
            return false;
        }
        Node *item = designators
                         ? new_designated_initializer(designators, value)
                         : value;
        append_node_list(&head, &tail, item);
        ParserCursor next = *cursor;
        if (consume_if(&next, TOK_COMMA) &&
            !starts_initializer_list_item(&next))
            break;
        if (!consume_if(cursor, TOK_COMMA))
            break;
    }
    *out = head;
    return true;
}

// Branches of (6.7.9) initializer:
//     { initializer-list }
//     { initializer-list , }
// Owned only by parse_initializer(); this boundary owns braces and the optional
// trailing comma, then wraps the final ordered item chain. It is split because
// braced lists own delimiters and a chain unlike assignment expressions.
static bool parse_braced_initializer(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    Token *open = consume(cursor);
    Node *items = NULL;
    if (!parse_initializer_list(cursor, &items))
        return false;
    consume_if(cursor, TOK_COMMA);
    Token *close = expect(cursor, TOK_RBRACE);
    if (!close) {
        node_free(items);
        return false;
    }
    *out = new_initializer_list(items, open, close);
    return true;
}

// Select the braced initializer alternatives without consuming input.
static bool starts_braced_initializer(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LBRACE);
}

// Branch of (6.7.9) initializer:
//     assignment-expression
// Owned only by parse_initializer(); this adapter presents the expression
// child's result through the initializer alternative protocol.
static bool parse_assignment_initializer(ParserCursor *cursor, Node **out)
{
    *out = parse_assignment_expression(cursor);
    return *out != NULL;
}

static const NodeOutAlternative g_initializer_alternatives[] = {
    { starts_braced_initializer, parse_braced_initializer },
    { starts_assignment_expression, parse_assignment_initializer },
};

// Select initializer through the same inventory used by its parser.
static bool starts_initializer(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_initializer_alternatives); i++) {
        if (g_initializer_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.7.9) designation:
//     designator-list =
static bool parse_designation(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    if (!parse_designator_list(cursor, out))
        return false;
    if (!expect(cursor, TOK_ASSIGN)) {
        node_free(*out);
        *out = NULL;
        return false;
    }
    return true;
}

// (6.7.9) designator-list:
//     designator
//     designator-list designator
static bool parse_designator_list(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    Node *head = NULL;
    Node *tail = NULL;
    while (starts_designator(cursor)) {
        Node *designator = NULL;
        if (!parse_designator(cursor, &designator)) {
            node_free(head);
            return false;
        }
        append_node_list(&head, &tail, designator);
    }
    *out = head;
    return true;
}

// (6.7.9) designator:
//     [ constant-expression ]
//     . identifier
static bool starts_index_designator(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LBRACKET);
}

// Branch of (6.7.9) designator:
//     [ constant-expression ]
// Owned only by parse_designator(); it owns the index expression and brackets.
static bool parse_index_designator(ParserCursor *cursor, Node **out)
{
    Token *open = consume(cursor);
    Node *index = parse_constant_expression(cursor);
    if (!index)
        return false;
    Token *close = expect(cursor, TOK_RBRACKET);
    if (!close) {
        node_free(index);
        return false;
    }
    *out = new_index_designator(open, index, close);
    return true;
}

// Select a member designator without consuming parser state.
static bool starts_member_designator(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_DOT);
}

// Branch of (6.7.9) designator:
//     . identifier
// Owned only by parse_designator(); it owns the member token pair.
static bool parse_member_designator(ParserCursor *cursor, Node **out)
{
    Token *dot = consume(cursor);
    Token *name = expect_identifier(cursor);
    if (!name)
        return false;
    *out = new_member_designator(dot, name);
    return true;
}

static const NodeOutAlternative g_designator_alternatives[] = {
    { starts_index_designator, parse_index_designator },
    { starts_member_designator, parse_member_designator },
};

// Select designator through the same inventory used by its parser.
static bool starts_designator(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_designator_alternatives); i++) {
        if (g_designator_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.7.9) designator:
//     [ constant-expression ]
//     . identifier
// Dispatch one already-selected child through the production's typed inventory.
static bool parse_designator(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    for (size_t i = 0; i < ARRAY_LEN(g_designator_alternatives); i++) {
        if (g_designator_alternatives[i].starts(cursor))
            return g_designator_alternatives[i].parse(cursor, out);
    }
    unreachable();
}

// ---------------------------------------------------------------------------
// Aggregate members and static assertions (C11 6.7.2, 6.7.10)
// ---------------------------------------------------------------------------

// (6.7.2.1) struct-or-union:
//     struct
//     union
static const Token *parse_struct_or_union(ParserCursor *cursor)
{
    return consume(cursor);
}

// Branch of (6.7.2.1) struct-declaration:
//     static_assert-declaration
// Owned only by parse_struct_declaration(); this adapter presents the child
// result through the struct-declaration alternative protocol.
static bool parse_struct_static_assertion(ParserCursor *cursor, Node **out)
{
    *out = parse_static_assert_declaration(cursor);
    return *out != NULL;
}

static const NodeOutAlternative g_struct_declaration_alternatives[] = {
    { starts_static_assert_declaration, parse_struct_static_assertion },
    { starts_member_specifier_qualifier_list, parse_struct_member_declaration },
};

static bool starts_struct_declaration(const ParserCursor *cursor)
{
    for (size_t i = 0;
         i < ARRAY_LEN(g_struct_declaration_alternatives); i++) {
        if (g_struct_declaration_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.7.2.1) struct-declaration-list:
//     struct-declaration
//     struct-declaration-list struct-declaration
static bool parse_struct_declaration_list(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    Node *head = NULL;
    Node *tail = NULL;
    if (!starts_struct_declaration(cursor)) {
        handle_parser_error_expected_syntax(current_token(cursor),
                                            "member declaration");
        return false;
    }
    while (starts_struct_declaration(cursor)) {
        Node *declaration = NULL;
        if (!parse_struct_declaration(cursor, &declaration)) {
            node_free(head);
            return false;
        }
        append_node_list(&head, &tail, declaration);
    }
    *out = head;
    return true;
}

// Branch of (6.7.2.1) struct-declaration:
//     specifier-qualifier-list struct-declarator-listopt ;
// Owned only by parse_struct_declaration(); this branch owns the optional
// declarator list and semicolon after the completed specifier list.
static bool parse_struct_member_declaration(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    ParsedSpecifierQualifierList parsed;
    if (!parse_specifier_qualifier_list(cursor, &parsed, true)) {
        return false;
    }
    DeclarationSpecifiers *specifiers = new_declaration_specifiers(
        parsed.type, parsed.qualifiers, STORAGE_NONE, 0, parsed.alignments, parsed.start,
        parsed.end);
    parsed.type = NULL;
    parsed.alignments = NULL;

    Node *declarators = NULL;
    if (starts_struct_declarator(cursor)) {
        if (!parse_struct_declarator_list(cursor, &declarators)) {
            declaration_specifiers_free(specifiers);
            return false;
        }
    }
    Token *semi = expect(cursor, TOK_SEMICOLON);
    if (!semi) {
        declaration_specifiers_free(specifiers);
        node_free(declarators);
        return false;
    }
    validate_struct_declaration_constraints(specifiers, declarators, semi);
    *out = new_struct_declaration(specifiers, declarators, semi);
    return true;
}

// (6.7.2.1) struct-declaration:
//     specifier-qualifier-list struct-declarator-listopt ;
//     static_assert-declaration
// Dispatch one selected child through the production's typed inventory.
static bool parse_struct_declaration(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    for (size_t i = 0;
         i < ARRAY_LEN(g_struct_declaration_alternatives); i++) {
        if (g_struct_declaration_alternatives[i].starts(cursor))
            return g_struct_declaration_alternatives[i].parse(cursor, out);
    }
    unreachable();
}

// (6.7.2.1) struct-declarator-list:
//     struct-declarator
//     struct-declarator-list , struct-declarator
static bool parse_struct_declarator_list(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    Node *head = NULL;
    Node *tail = NULL;
    for (;;) {
        Node *declarator = NULL;
        if (!parse_struct_declarator(cursor, &declarator)) {
            node_free(head);
            return false;
        }
        append_node_list(&head, &tail, declarator);
        if (!consume_if(cursor, TOK_COMMA))
            break;
    }
    *out = head;
    return true;
}

// Pure FIRST-set query owned by struct-declarator. A colon selects its
// declarator-less branch; every other start is delegated to declarator.
static bool starts_struct_declarator(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_COLON) ||
           starts_declarator(cursor);
}

// (6.7.2.1) struct-declarator:
//     declarator
//     declaratoropt : constant-expression
static bool parse_struct_declarator(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    ParsedDeclarator parsed = {0};
    if (!token_is(current_token(cursor), TOK_COLON)) {
        if (!parse_declarator(cursor, &parsed, NULL))
            return false;
        validate_declarator_constraints(&parsed, DECLARATOR_ORDINARY);
    }

    Node *width = NULL;
    if (consume_if(cursor, TOK_COLON)) {
        width = parse_constant_expression(cursor);
        if (!width) {
            discard_parsed_declarator(&parsed);
            return false;
        }
    }
    if (!parsed.shape && !width)
        unreachable();
    *out = new_struct_declarator(parsed.shape, width);
    parsed.shape = NULL;
    return true;
}

// Pure FIRST-set query owned by enumerator. The parent list uses it to decide
// whether a comma repeats the child or remains its own trailing delimiter.
static bool starts_enumerator(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_IDENTIFIER);
}

// (6.7.2.2) enumerator-list:
//     enumerator
//     enumerator-list , enumerator
static bool parse_enumerator_list(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    Node *head = NULL;
    Node *tail = NULL;
    for (;;) {
        Node *enumerator = NULL;
        if (!starts_enumerator(cursor)) {
            handle_parser_error_expected_syntax(current_token(cursor),
                                                "enumerator");
            node_free(head);
            return false;
        }
        if (!parse_enumerator(cursor, &enumerator)) {
            node_free(head);
            return false;
        }
        append_node_list(&head, &tail, enumerator);
        ParserCursor next = *cursor;
        if (!consume_if(&next, TOK_COMMA) ||
            !starts_enumerator(&next))
            break;
        consume_if(cursor, TOK_COMMA);
    }
    *out = head;
    return true;
}

// (6.7.2.2) enumerator:
//     enumeration-constant
//     enumeration-constant = constant-expression
static bool parse_enumerator(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    Token *name = consume(cursor);
    Node *value = NULL;
    if (consume_if(cursor, TOK_ASSIGN)) {
        value = parse_constant_expression(cursor);
        if (!value)
            return false;
    }
    *out = new_enumerator(name, value);
    parser_declare_name(cursor, &name->span.text, false);
    return true;
}

// (6.7.10) static_assert-declaration:
//     _Static_assert ( constant-expression , string-literal ) ;
static Node *parse_static_assert_declaration(ParserCursor *cursor)
{
    Token *keyword = consume_if(cursor, TOK_KW_STATIC_ASSERT);
    if (!keyword)
        unreachable();
    if (!expect(cursor, TOK_LPAREN))
        return NULL;
    Node *expression = parse_constant_expression(cursor);
    if (!expression)
        return NULL;
    if (!expect(cursor, TOK_COMMA)) {
        node_free(expression);
        return NULL;
    }
    Token *message_tok = consume_if(cursor, TOK_STRING);
    if (!message_tok) {
        handle_parser_error_expected_token(current_token(cursor), TOK_STRING);
        node_free(expression);
        return NULL;
    }
    Node *message = new_string(message_tok);
    if (!expect(cursor, TOK_RPAREN)) {
        node_free(expression);
        node_free(message);
        return NULL;
    }
    Token *semi = expect(cursor, TOK_SEMICOLON);
    if (!semi) {
        node_free(expression);
        node_free(message);
        return NULL;
    }
    return new_static_assertion(keyword, expression, message, semi);
}

static bool starts_static_assert_declaration(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_STATIC_ASSERT);
}

// (6.7.9) initializer:
//     assignment-expression
//     { initializer-list }
//     { initializer-list , }
// The braced alternative is registered before assignment-expression, so its
// implementation does not change this production.
Node *parse_initializer(ParserCursor *cursor)
{
    Node *initializer = NULL;
    for (size_t i = 0; i < ARRAY_LEN(g_initializer_alternatives); i++) {
        if (g_initializer_alternatives[i].starts(cursor)) {
            return g_initializer_alternatives[i].parse(cursor, &initializer)
                       ? initializer
                       : NULL;
        }
    }
    handle_parser_error_expected_expression(current_token(cursor));
    return NULL;
}

// Pure FIRST-set query owned by init-declarator and delegated to its required
// declarator child.
static bool starts_init_declarator(const ParserCursor *cursor)
{
    return starts_declarator(cursor);
}

// Pure FIRST-set query owned by init-declarator-list and delegated to its
// required first init-declarator.
static bool starts_init_declarator_list(const ParserCursor *cursor)
{
    return starts_init_declarator(cursor);
}

// (6.7) init-declarator:
//     declarator
//     declarator = initializer
// This production owns its declarator, optional initializer, final node and
// constraints requiring only that node. The typedef flag is the minimum state
// needed to make the declared name visible before parsing its initializer.
static Node *parse_init_declarator(ParserCursor *cursor, bool is_typedef)
{
    ParsedInitDeclarator parsed = {0};
    if (!parse_declarator(cursor, &parsed.declarator, NULL))
        return NULL;
    parser_declare_name(cursor, parsed.declarator.name_tok
                                    ? &parsed.declarator.name_tok->span.text
                                    : NULL,
                        is_typedef);
    if (consume_if(cursor, TOK_ASSIGN)) {
        parsed.initializer = parse_initializer(cursor);
        if (!parsed.initializer) {
            discard_parsed_declarator(&parsed.declarator);
            return NULL;
        }
    }
    validate_declarator_constraints(&parsed.declarator, DECLARATOR_ORDINARY);
    Node *node = new_init_declarator(parsed.initializer,
                                     parsed.declarator.shape);
    parsed.initializer = NULL;
    parsed.declarator.shape = NULL;
    validate_init_declarator_constraints(node);
    return node;
}

// (6.7) init-declarator-list:
//     init-declarator
//     init-declarator-list , init-declarator
// init-declarator-list owns only comma-separated repetition. The typedef flag
// is forwarded solely for C11's declarator-point name visibility rule.
static Node *parse_init_declarator_list(ParserCursor *cursor, bool is_typedef)
{
    Node *head = NULL;
    Node *tail = NULL;
    for (;;) {
        if (!starts_init_declarator(cursor)) {
            handle_parser_error_expected_syntax(current_token(cursor),
                                                "declarator");
            node_free(head);
            return NULL;
        }
        Node *node = parse_init_declarator(cursor, is_typedef);
        if (!node) {
            node_free(head);
            return NULL;
        }
        append_node_list(&head, &tail, node);
        if (!consume_if(cursor, TOK_COMMA))
            break;
    }
    return head;
}

// Branch of (6.7) declaration:
//     declaration-specifiers init-declarator-listopt ;
// Owned only by parse_declaration(); this branch owns the completed specifiers,
// optional declarator list, declaration constraints and semicolon.
static bool parse_specified_declaration(ParserCursor *cursor,
                                        DeclarationContext context,
                                        const Token *context_anchor,
                                        Node **out)
{
    *out = NULL;
    ParsedDeclarationSpecifiers specs;
    if (!parse_declaration_specifiers(cursor, &specs))
        return false;
    Token *empty_semi = consume_if(cursor, TOK_SEMICOLON);
    if (empty_semi) {
        TypeSpecifierKind kind = specs.ast->type->kind;
        if (kind != TYPE_SPECIFIER_STRUCT && kind != TYPE_SPECIFIER_UNION &&
            kind != TYPE_SPECIFIER_ENUM)
            handle_parser_error_useless_declaration(specs.start);
        *out = new_declaration(specs.ast, NULL, empty_semi);
        specs.ast = NULL;
        return true;
    }

    if (!starts_init_declarator_list(cursor)) {
        (void)expect(cursor, TOK_SEMICOLON);
        declaration_specifiers_free(specs.ast);
        return false;
    }

    bool is_typedef = specs.ast->storage & STORAGE_TYPEDEF;
    Node *declarators = parse_init_declarator_list(cursor, is_typedef);
    if (!declarators) {
        declaration_specifiers_free(specs.ast);
        return false;
    }

    for (const Node *decl = declarators; decl; decl = decl->next)
        validate_declaration_constraints(&specs, context, context_anchor,
                                         decl);

    if (!token_is(current_token(cursor), TOK_SEMICOLON)) {
        bool handled = handle_parser_error_declaration_tail(
            cursor, declarators, context);
        node_free(declarators);
        declaration_specifiers_free(specs.ast);
        return handled;
    }
    Token *semi = consume_if(cursor, TOK_SEMICOLON);
    *out = new_declaration(specs.ast, declarators, semi);
    specs.ast = NULL;
    return true;
}

// Branch of (6.7) declaration:
//     static_assert-declaration
// Owned only by parse_declaration(); this adapter presents the child result
// through the declaration alternative protocol.
static bool parse_static_assert_alternative(
    ParserCursor *cursor, DeclarationContext context,
    const Token *context_anchor, Node **out)
{
    (void)context;
    (void)context_anchor;
    *out = parse_static_assert_declaration(cursor);
    return *out != NULL;
}

static const DeclarationAlternative g_declaration_alternatives[] = {
    { starts_static_assert_declaration, parse_static_assert_alternative },
    { starts_declaration_specifiers, parse_specified_declaration },
};

// Select declaration through the same inventory used by its parser.
bool starts_declaration(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_declaration_alternatives); i++) {
        if (g_declaration_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.7) declaration:
//     declaration-specifiers init-declarator-listopt ;
//     static_assert-declaration
// Dispatch one selected child and return its optional final AST node.
bool parse_declaration(ParserCursor *cursor, DeclarationContext context,
                       const Token *context_anchor, Node **out)
{
    *out = NULL;
    for (size_t i = 0; i < ARRAY_LEN(g_declaration_alternatives); i++) {
        if (g_declaration_alternatives[i].starts(cursor)) {
            return g_declaration_alternatives[i].parse(
                cursor, context, context_anchor, out);
        }
    }
    unreachable();
}

// (6.9.1) declaration-list:
//     declaration
//     declaration-list declaration
// Parse the nonempty declaration sequence directly into its final AST chain;
// a completed function-definition becomes its owner. Whether its declarator
// permits this child is a later function-definition constraint.
static bool parse_declaration_list(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    Node *head = NULL;
    Node *tail = NULL;
    do {
        Node *declaration = NULL;
        if (!parse_declaration(cursor, DECLARATION_OLD_STYLE_PARAMETER,
                               NULL, &declaration)) {
            node_free(head);
            return false;
        }
        if (declaration)
            append_node_list(&head, &tail, declaration);
    } while (starts_declaration(cursor));
    *out = head;
    return true;
}

// Pure FIRST-set query owned by declaration-list. Function-definition uses it
// to decide whether its optional direct child is present.
static bool starts_declaration_list(const ParserCursor *cursor)
{
    return starts_declaration(cursor);
}

// Probe the complete declaration-specifiers prefix without allocating or
// diagnosing. Structured specifiers are skipped as one grammar unit so the
// following tag or body token cannot be mistaken for a declarator.
static bool skip_declaration_specifiers(ParserCursor *cursor)
{
    bool matched = false;
    bool has_type = false;
    for (;;) {
        const Token *tok = current_token(cursor);
        unsigned slot;
        if (starts_storage_class_specifier(cursor) ||
            starts_function_specifier(cursor)) {
            consume(cursor);
            matched = true;
            continue;
        }
        if (starts_alignment_specifier(cursor)) {
            consume(cursor);
            if (!skip_balanced_group(cursor, TOK_LPAREN, TOK_RPAREN))
                return false;
            matched = true;
            continue;
        }
        if (starts_atomic_type_specifier(cursor)) {
            if (has_type)
                break;
            consume(cursor);
            if (!skip_balanced_group(cursor, TOK_LPAREN, TOK_RPAREN))
                return false;
            matched = true;
            has_type = true;
            continue;
        }
        if (starts_type_qualifier(cursor)) {
            consume(cursor);
            matched = true;
            continue;
        }
        if (starts_struct_or_union_specifier(cursor) ||
            starts_enum_specifier(cursor)) {
            if (has_type)
                break;
            consume(cursor);
            if (token_is(current_token(cursor), TOK_IDENTIFIER))
                consume(cursor);
            if (token_is(current_token(cursor), TOK_LBRACE) &&
                !skip_balanced_group(cursor, TOK_LBRACE, TOK_RBRACE))
                return false;
            matched = true;
            has_type = true;
            continue;
        }
        if (lookup_builtin_type_specifier(tok, &slot)) {
            (void)slot;
            consume(cursor);
            matched = true;
            has_type = true;
            continue;
        }
        if (starts_typedef_name(cursor) && !has_type) {
            consume(cursor);
            matched = true;
            has_type = true;
            continue;
        }
        break;
    }
    return matched && has_type;
}

// Silently skip one declarator and summarize the derived layer nearest its
// identifier. Recursion is necessary to preserve arbitrary parenthesized
// binding; every recursive frame first consumes one opening parenthesis and
// terminates only after consuming its matching close.
static bool skip_declarator(ParserCursor *cursor, DeclaratorSummary *out)
{
    bool has_pointer = false;
    while (consume_if(cursor, TOK_STAR)) {
        has_pointer = true;
        while (starts_type_qualifier(cursor))
            consume(cursor);
    }

    if (token_is(current_token(cursor), TOK_IDENTIFIER)) {
        consume(cursor);
    } else if (token_is(current_token(cursor), TOK_LPAREN)) {
        consume_if(cursor, TOK_LPAREN);
        if (!skip_declarator(cursor, out) ||
            !consume_if(cursor, TOK_RPAREN))
            return false;
    } else {
        return false;
    }

    for (;;) {
        if (token_is(current_token(cursor), TOK_LBRACKET)) {
            if (out->entity == DECLARATOR_ENTITY_NONE)
                out->entity = DECLARATOR_ENTITY_ARRAY;
            if (!skip_balanced_group(cursor, TOK_LBRACKET, TOK_RBRACKET))
                return false;
            continue;
        }
        if (token_is(current_token(cursor), TOK_LPAREN)) {
            ParserCursor contents = *cursor;
            consume(&contents);
            FunctionParameterForm form =
                token_is(current_token(&contents), TOK_RPAREN) ||
                        !starts_declaration_specifiers(&contents)
                    ? FUNCTION_PARAMS_IDENTIFIER_LIST
                    : FUNCTION_PARAMS_PROTOTYPE;
            if (out->entity == DECLARATOR_ENTITY_NONE) {
                out->entity = DECLARATOR_ENTITY_FUNCTION;
                out->function_form = form;
            }
            if (!skip_balanced_group(cursor, TOK_LPAREN, TOK_RPAREN))
                return false;
            continue;
        }
        break;
    }
    if (out->entity == DECLARATOR_ENTITY_NONE && has_pointer)
        out->entity = DECLARATOR_ENTITY_POINTER;
    return true;
}

// Pure synchronization probe for a complete external-declaration prefix. It
// accepts the optional init-declarator-list's empty form only before `;`; every
// other declaration-specifiers prefix must lead to a declarator.
bool starts_external_declaration_sync(const ParserCursor *cursor)
{
    if (starts_static_assert_declaration(cursor))
        return true;
    ParserCursor lookahead = *cursor;
    if (!skip_declaration_specifiers(&lookahead))
        return false;
    if (token_is(current_token(&lookahead), TOK_SEMICOLON))
        return true;
    DeclaratorSummary declarator = {0};
    return skip_declarator(&lookahead, &declarator);
}

// Pure selector for C11 6.9 external-declaration. It follows only the shared
// declaration-specifiers/declarator prefix through a local cursor; a false
// result never consumes input or emits diagnostics.
static bool starts_function_definition(const ParserCursor *cursor)
{
    ParserCursor lookahead = *cursor;
    if (!skip_declaration_specifiers(&lookahead))
        return false;

    DeclaratorSummary declarator = {0};
    if (!skip_declarator(&lookahead, &declarator) ||
        declarator.entity != DECLARATOR_ENTITY_FUNCTION)
        return false;
    if (token_is(current_token(&lookahead), TOK_LBRACE))
        return true;
    return starts_declaration_list(&lookahead);
}

// Register old-style identifiers after entering the function body scope.
static void declare_function_parameter_names(ParserCursor *cursor,
                                             const Declarator *function)
{
    if (function->u.function.form != FUNCTION_PARAMS_IDENTIFIER_LIST)
        return;
    for (const Node *identifier = function->u.function.clause.old_style.identifiers;
         identifier; identifier = identifier->next)
        parser_declare_name(cursor, &identifier->span.text, false);
}

// (6.9.1) function-definition:
//     declaration-specifiers declarator declaration-listopt compound-statement
// This parser is entered only after starts_function_definition() selects the
// alternative; every subsequent failure is a committed syntax error.
static bool parse_function_definition(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    ParsedDeclarationSpecifiers specs;
    if (!parse_declaration_specifiers(cursor, &specs))
        return false;
    ParsedDeclarator decl = {0};
    NameScope *parameters = NULL;
    if (!parse_declarator(cursor, &decl, &parameters)) {
        free_parser_scope(parameters);
        declaration_specifiers_free(specs.ast);
        return false;
    }
    validate_declarator_constraints(&decl, DECLARATOR_ORDINARY);

    const Declarator *function_decl = declarator_function(decl.shape);
    if (!function_decl)
        unreachable();
    validate_function_definition_storage_class(&specs, &decl);

    validate_function_definition_constraints(function_decl);
    parser_declare_name(cursor,
                        decl.name_tok ? &decl.name_tok->span.text : NULL,
                        false);
    parser_push_scope(cursor, &parameters);
    declare_function_parameter_names(cursor, function_decl);

    Node *declarations = NULL;
    if (starts_declaration_list(cursor)) {
        if (!parse_declaration_list(cursor, &declarations)) {
            discard_parsed_declarator(&decl);
            declaration_specifiers_free(specs.ast);
            parser_pop_scope(cursor, NULL);
            return false;
        }
        validate_function_definition_declarations(function_decl, declarations);
    }

    Node *body = parse_compound_statement(cursor);
    if (!body) {
        discard_parsed_declarator(&decl);
        declaration_specifiers_free(specs.ast);
        node_free(declarations);
        parser_pop_scope(cursor, NULL);
        return false;
    }
    parser_pop_scope(cursor, NULL);
    *out = new_function(specs.ast, decl.shape, declarations, body);
    specs.ast = NULL;
    decl.shape = NULL;
    declarations = NULL;
    return true;
}

// Branch of (6.9) external-declaration:
//     declaration
// Owned only by parse_external_declaration(); this adapter supplies the
// file-scope context required by the declaration child.
static bool parse_file_declaration(ParserCursor *cursor, Node **out)
{
    return parse_declaration(cursor, DECLARATION_FILE, NULL, out);
}

static const NodeOutAlternative g_external_declaration_alternatives[] = {
    { starts_function_definition, parse_function_definition },
    { starts_declaration, parse_file_declaration },
};

// (6.9) external-declaration:
//     function-definition
//     declaration
// The parent production only selects and delegates to its two C11 children.
bool parse_external_declaration(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    for (size_t i = 0;
         i < ARRAY_LEN(g_external_declaration_alternatives); i++) {
        if (g_external_declaration_alternatives[i].starts(cursor))
            return g_external_declaration_alternatives[i].parse(cursor, out);
    }
    unreachable();
}

// Pure FIRST-set query owned by external-declaration. Translation-unit uses
// this boundary instead of reaching through it to declaration.
bool starts_external_declaration(const ParserCursor *cursor)
{
    for (size_t i = 0;
         i < ARRAY_LEN(g_external_declaration_alternatives); i++) {
        if (g_external_declaration_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

#ifdef TEST_PARSER

// Regression probe for the grammar boundary: initializer-list must stop before
// the trailing comma owned by its parent initializer production.
bool parser_test_initializer_list_leaves_trailing_comma(void)
{
    static char source[] = "1,}";
    static SourceFile file = {
        .name = (char *)"<initializer-list-test>",
        .contents = source,
        .size = sizeof(source) - 1,
    };
    Token *tokens = tokenize(&file);
    ParserCursor cursor;
    init_parser_cursor(&cursor, tokens);
    Node *items = NULL;
    bool ok = parse_initializer_list(&cursor, &items) &&
              token_is(current_token(&cursor), TOK_COMMA);
    node_free(items);
    token_free(tokens);
    return ok;
}

#endif // TEST_PARSER
