// Parser error boundary. Grammar modules select a typed
// handle_parser_error_* operation but do not report, clean up or synchronize
// directly; low-level reporting and synchronization remain private here.

#include "parser_internal.h"

#include "util/common.h"
#include "util/diagnostic.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned parens;
    unsigned brackets;
    unsigned braces;
} DelimiterDepth;

// ---------------------------------------------------------------------------
// Private diagnostic formatting and reporters
// ---------------------------------------------------------------------------

// Private report_* functions select one message and anchor; public handlers
// compose them with ownership cleanup or parser synchronization when needed.

// Clamp a source-backed view to printf's `int` precision interface.
static int view_precision(StringView view)
{
    return string_view_length(view) > INT_MAX ? INT_MAX : (int)string_view_length(view);
}

// gcc anchors a missing ';' at the end of the previous token instead of the
// following token. Snippet tests without a current source keep the token span.
static SourceSpan span_after_previous(const Token *tok)
{
    const SourceFile *file = tok->span.file;
    if (!file || file != current_source())
        return tok->span;
    const char *p = string_view_begin(tok->span.text);
    while (p > file->contents && isspace((unsigned char)p[-1]))
        p--;
    return source_span(p, p);
}

// Render one expected-token diagnostic at either the supplied anchor or the
// offending token, preserving gcc's literal and punctuation wording.
static void report_expected_at(const Token *tok, const char *expected,
                               const SourceSpan *at)
{
    if (token_is(tok, TOK_EOF)) {
        error_at(at ? at : &tok->span, "expected %s at end of input", expected);
    } else if (token_is(tok, TOK_NUMBER)) {
        error_at(at ? at : &tok->span, "expected %s before numeric constant", expected);
    } else if (token_is(tok, TOK_CHARACTER)) {
        error_at(at ? at : &tok->span, "expected %s before character constant", expected);
    } else if (token_is(tok, TOK_STRING)) {
        error_at(at ? at : &tok->span, "expected %s before string constant", expected);
    } else if (token_kind_is_punctuator(tok->kind)) {
        error_at(at ? at : &tok->span, "expected %s before '%.*s' token", expected,
                 view_precision(tok->span.text), string_view_begin(tok->span.text));
    } else {
        error_at(at ? at : &tok->span, "expected %s before '%.*s'", expected,
                 view_precision(tok->span.text), string_view_begin(tok->span.text));
    }
}

// Render a generic expectation without an anchor override.
static void report_expected(const Token *tok, const char *expected)
{
    report_expected_at(tok, expected, NULL);
}

// Report one required expression at the caller-selected grammar boundary.
static void report_expected_expression(const Token *tok)
{
    report_expected(tok, "expression");
}

// Report a translation-unit item that cannot begin with a C11 type.
static void report_missing_type(const Token *tok)
{
    report_expected(tok, "type");
}

// C11 6.9p3 rejects an extra semicolon outside a function.
static void report_extra_semicolon(const Token *tok)
{
    error_at(&tok->span, "ISO C does not allow extra ';' outside of a function");
}

// gcc's wording for a declarator tail it cannot continue.
static void report_declarator_tail(const Token *tok)
{
    report_expected(tok, "'=', ',', ';', 'asm' or '__attribute__'");
}

// Initializers stop at the comma, so a missing `;` is reported with gcc's
// "',' or ';'" wording instead of the generic "';'".
static void report_initializer_tail(const Token *tok)
{
    report_expected(tok, "',' or ';'");
}

// Parameter lists share the declarator tail: anything but ',' or ')' after a
// parameter is gcc's list-tail error.
static void report_parameter_tail(const Token *tok)
{
    report_expected(tok, "';', ',' or ')'");
}

// C11 6.7.2: `signed` and `unsigned` are mutually exclusive.
static void report_both_signed_unsigned(const Token *tok)
{
    error_at(&tok->span, "both 'signed' and 'unsigned' in declaration specifiers");
}

// Report a repeated type keyword at the later occurrence.
static void report_duplicate_type_keyword(const Token *tok)
{
    error_at(&tok->span, "duplicate '%.*s'", view_precision(tok->span.text),
             string_view_begin(tok->span.text));
}

// C11 6.7.6.3p2 permits only register on a parameter declaration; named
// parameters anchor the diagnostic at their identifier.
static void report_parameter_storage_class(const SourceSpan *span,
                                           StringView name)
{
    error_at(span, "storage class specified for parameter '%.*s'",
             view_precision(name), string_view_begin(name));
}

// C11 6.9p2 excludes auto and register from external declarations. Keep the
// identifier as the anchor while spelling the rejected specifier in the text.
static void report_external_storage_class(const Declarator *declarator,
                                          const Token *storage_tok)
{
    const SourceSpan *span = declarator_name_span(declarator);
    const StringView *name = declarator_name(declarator);
    StringView displayed = name ? *name : (StringView){ 0 };
    error_at(span ? span : &declarator->span,
             "file-scope declaration of '%.*s' specifies '%.*s'",
             view_precision(displayed), string_view_begin(displayed),
             view_precision(storage_tok->span.text), string_view_begin(storage_tok->span.text));
}

// C11 6.9.1p4 permits only extern or static on a function definition; the
// function identifier is the stable diagnostic anchor.
static void report_function_definition_storage_class(const ParsedDeclarator *decl,
                                                      const Token *storage_tok)
{
    const SourceSpan *span = declarator_name_span(decl->shape);
    error_at(span ? span : &decl->shape->span,
             "function definition declared '%.*s'",
             view_precision(storage_tok->span.text), string_view_begin(storage_tok->span.text));
}

// C11 6.7.1p7 permits only extern on a block-scope function declaration; the
// function identifier is the stable diagnostic anchor.
static void report_block_function_storage_class(const Declarator *declarator)
{
    const SourceSpan *span = declarator_name_span(declarator);
    const StringView *name = declarator_name(declarator);
    error_at(span ? span : &declarator->span,
             "invalid storage class for function '%.*s'",
             view_precision(*name), string_view_begin(*name));
}

// C11 6.8.5p3 restricts a for-init declaration to objects with auto or
// register storage; the declarator name identifies the rejected declaration.
static void report_for_storage_class(const Declarator *declarator)
{
    const SourceSpan *span = declarator_name_span(declarator);
    const StringView *name = declarator_name(declarator);
    error_at(span ? span : &declarator->span,
             "invalid storage class for '%.*s' in 'for' loop initial declaration",
             view_precision(*name), string_view_begin(*name));
}

// gcc asks for at most two `long` in one declaration-specifiers.
static void report_too_many_long(const Token *tok)
{
    error_at(&tok->span, "'long long long' is too long for GCC");
}

// Any other incompatible combination of type-specifier keywords.
static void report_too_many_types(const Token *tok)
{
    error_at(&tok->span, "two or more data types in declaration specifiers");
}

// `long long` cannot be combined with `double`; gcc keeps the combined wording
// and anchors the later keyword, whichever order the two spellings appear in.
static void report_both_long_double(const Token *tok)
{
    error_at(&tok->span, "both 'long long' and 'double' in declaration specifiers");
}

// Render the declarator classifier's structured failure at its stable name or
// offending derived layer without re-deciding the violated constraint.
static void report_invalid_derived_declarator(const Declarator *declarator,
                                              const DeclaratorConstraintError *error)
{
    const SourceSpan *span = declarator_name_span(declarator);
    const StringView *name = declarator_name(declarator);
    const SourceSpan *anchor = span ? span : &error->layer->span;
    switch (error->kind) {
    case DECLARATOR_FUNCTION_RETURNING_FUNCTION:
        if (name)
            error_at(anchor, "'%.*s' declared as function returning a function",
                     view_precision(*name), string_view_begin(*name));
        else
            error_at(anchor, "type name declared as function returning a function");
        break;
    case DECLARATOR_FUNCTION_RETURNING_ARRAY:
        if (name)
            error_at(anchor, "'%.*s' declared as function returning an array",
                     view_precision(*name), string_view_begin(*name));
        else
            error_at(anchor, "type name declared as function returning an array");
        break;
    case DECLARATOR_ARRAY_OF_FUNCTIONS:
        if (name)
            error_at(anchor, "declaration of '%.*s' as array of functions",
                     view_precision(*name), string_view_begin(*name));
        else
            error_at(anchor, "type name declared as array of functions");
        break;
    case DECLARATOR_ARRAY_PARAMETER_ONLY:
        error_at(anchor,
                 "static or type qualifiers in non-parameter array declarator");
        break;
    case DECLARATOR_ARRAY_OUTERMOST_ONLY:
        error_at(anchor,
                 "static or type qualifiers in non-outermost array type derivation");
        break;
    case DECLARATOR_STAR_PROTOTYPE_ONLY:
        error_at(anchor,
                 "'[*]' not allowed in other than function prototype scope");
        break;
    case DECLARATOR_CONSTRAINT_NONE:
        unreachable();
    }
}

// Report an initializer attached to a completed function declarator.
static void report_function_initialized(const Declarator *declarator)
{
    const SourceSpan *span = declarator_name_span(declarator);
    const StringView *name = declarator_name(declarator);
    error_at(span ? span : &declarator->span,
             "function '%.*s' is initialized like a variable",
             view_precision(*name), string_view_begin(*name));
}

// C11 6.7.8p3 forbids an initializer on a typedef declarator.
static void report_typedef_initialized(const Declarator *declarator)
{
    const SourceSpan *span = declarator_name_span(declarator);
    const StringView *name = declarator_name(declarator);
    error_at(span ? span : &declarator->span,
             "typedef '%.*s' is initialized", view_precision(*name),
             string_view_begin(*name));
}

// C11 6.7.2.1p2 requires a member declarator unless the declaration introduces
// an anonymous structure or union; anchor the empty list at its semicolon.
static void report_struct_declaration_without_declarator(const Token *semi)
{
    error_at(&semi->span, "declaration does not declare anything");
}

// C11 6.7.6.3p3 permits a non-definition identifier-list only when it is empty.
static void report_identifier_list_in_declaration(const Declarator *declarator)
{
    const SourceSpan *span = declarator_name_span(declarator);
    error_at(span ? span : &declarator->span,
             "parameter names (without types) in function declaration");
}

// Report an unmatched else at its keyword without consuming parser input.
static void report_else_without_if(const Token *tok)
{
    error_at(&tok->span, "'else' without a previous 'if'");
}

// Report a nested function definition at its retained declarator name.
static void report_function_definition_in_block(const Declarator *declarator)
{
    const SourceSpan *span = declarator_name_span(declarator);
    error_at(span ? span : &declarator->span,
             "function definition is not allowed here");
}

// gcc anchors a truncated block at the statement it was in the middle of; an
// empty block falls back to the opening brace.
static void report_unclosed_block(const Token *open, const Token *stmt_start)
{
    SourceSpan anchor = stmt_start ? stmt_start->span : open->span;
    error_at(&anchor, "expected declaration or statement at end of input");
}

// C11 6.7.6.3p10: unnamed `void` denotes no parameters only when it is the
// sole item in the parameter list.
static void report_void_not_alone_at(const SourceSpan *span)
{
    error_at(span, "'void' must be the only parameter");
}

// A named direct `void` parameter in a function definition has incomplete
// type. Prototype declarations remain valid and may be warned on later.
static void report_void_parameter(const SourceSpan *name, int index)
{
    error_at(name, "parameter %d ('%.*s') has incomplete type", index,
             view_precision(name->text), string_view_begin(name->text));
}

// gcc message for C11 6.8.5.3: the for-init declaration must declare an
// ordinary identifier, not a function. Anchored at the `for` keyword like gcc.
static void report_non_variable_in_for_init(const Node *init, const Token *keyword)
{
    const Declarator *declarator = init->u.init_decl.declarator;
    const StringView *name = declarator_name(declarator);
    error_at(&keyword->span,
             "declaration of non-variable '%.*s' in 'for' loop initial declaration",
             view_precision(*name), string_view_begin(*name));
}

// ---------------------------------------------------------------------------
// Private recovery and validation helpers
// ---------------------------------------------------------------------------

// Find the first `[*]` layer in one parameter declarator without diagnostics
// or mutation; function definitions do not have function prototype scope.
static const Declarator *find_star_array(const Declarator *declarator)
{
    for (const Declarator *layer = declarator; layer; layer = layer->inner) {
        if (layer->kind == DECLARATOR_ARRAY &&
            layer->u.array.form == ARRAY_DECLARATOR_STAR)
            return layer;
    }
    return NULL;
}

// Pure multiplicity lookup for one old-style parameter spelling.
static unsigned old_style_identifier_count(const Node *identifiers,
                                           StringView name)
{
    unsigned count = 0;
    for (const Node *identifier = identifiers; identifier;
         identifier = identifier->next) {
        if (string_view_equal(identifier->u.identifier.name, name))
            count++;
    }
    return count;
}

// Update recovery-only delimiter state for one consumed token. Unmatched
// closing delimiters remain at the caller's baseline rather than underflowing.
static void update_delimiter_depth(DelimiterDepth *depth, const Token *tok)
{
    if (token_is(tok, TOK_LPAREN))
        depth->parens++;
    else if (token_is(tok, TOK_RPAREN) && depth->parens)
        depth->parens--;
    else if (token_is(tok, TOK_LBRACKET))
        depth->brackets++;
    else if (token_is(tok, TOK_RBRACKET) && depth->brackets)
        depth->brackets--;
    else if (token_is(tok, TOK_LBRACE))
        depth->braces++;
    else if (token_is(tok, TOK_RBRACE) && depth->braces)
        depth->braces--;
}

// Reconstruct delimiters consumed since one grammar item began. Both cursors
// remain unchanged; malformed EOF paths return the depth reached so far.
static DelimiterDepth delimiter_depth_between(const ParserCursor *begin,
                                              const ParserCursor *end)
{
    DelimiterDepth depth = {0};
    ParserCursor scan = *begin;
    while (current_token(&scan) != current_token(end) &&
           !at_eof(current_token(&scan))) {
        const Token *tok = consume(&scan);
        update_delimiter_depth(&depth, tok);
    }
    return depth;
}

// Pure recovery-state query for the failed item's original delimiter depth.
static bool at_recovery_baseline(const DelimiterDepth *depth)
{
    return !depth->parens && !depth->brackets && !depth->braces;
}

// Panic-mode recovery at top level: walk to EOF or the first complete
// declaration prefix outside every delimiter opened by the failed item.
// Stopping at its first specifier preserves combinations such as `unsigned
// long` for the formal parser.
static void sync_external_declaration(ParserCursor *cursor,
                                      const ParserCursor *item_start)
{
    DelimiterDepth depth = delimiter_depth_between(item_start, cursor);
    while (!at_eof(current_token(cursor))) {
        if (at_recovery_baseline(&depth)) {
            if (starts_external_declaration_sync(cursor))
                return;
            if (token_is(current_token(cursor), TOK_IDENTIFIER) &&
                token_is(peek_token(cursor, 1), TOK_LPAREN))
                return;
        }
        const Token *tok = consume(cursor);
        update_delimiter_depth(&depth, tok);
    }
}

// Top-level recovery for an unparsed item: consuming one token when sync
// cannot move guarantees the caller's loop always advances.
static void sync_top_level(ParserCursor *cursor,
                           const ParserCursor *item_start)
{
    sync_external_declaration(cursor, item_start);
    if (current_token(cursor) == current_token(item_start))
        consume(cursor);
}

// Block recovery skips delimiters belonging to the failed item, preserves the
// owning compound-statement's `}`, and stops before the next selected block-item.
static void sync_block_item(ParserCursor *cursor,
                            const ParserCursor *item_start)
{
    DelimiterDepth depth = delimiter_depth_between(item_start, cursor);
    while (!at_eof(current_token(cursor))) {
        if (at_recovery_baseline(&depth)) {
            if (token_is(current_token(cursor), TOK_RBRACE))
                return;
            if (token_is(current_token(cursor), TOK_SEMICOLON)) {
                consume(cursor);
                return;
            }
            if (current_token(cursor) != current_token(item_start) &&
                !token_is(current_token(cursor), TOK_LBRACE) &&
                starts_block_item(cursor))
                return;
        }
        const Token *tok = consume(cursor);
        update_delimiter_depth(&depth, tok);
    }
}

// Pure context policy for C11 storage-class constraints. Declarator shape only
// affects block-scope function declarations; for-init object shape is checked
// after the complete declaration has been built.
static bool is_storage_class_allowed(StorageClass storage,
                                     DeclarationContext context,
                                     bool is_function)
{
    StorageClass base_storage =
        storage & (StorageClass)~STORAGE_THREAD_LOCAL;
    if (base_storage == STORAGE_TYPEDEF)
        return context == DECLARATION_FILE || context == DECLARATION_BLOCK;
    switch (context) {
    case DECLARATION_FILE:
        return base_storage == STORAGE_NONE || base_storage == STORAGE_EXTERN ||
               base_storage == STORAGE_STATIC;
    case DECLARATION_BLOCK:
        return !is_function || base_storage == STORAGE_NONE ||
               base_storage == STORAGE_EXTERN;
    case DECLARATION_FOR:
        return base_storage == STORAGE_NONE || base_storage == STORAGE_AUTO ||
               base_storage == STORAGE_REGISTER;
    case DECLARATION_OLD_STYLE_PARAMETER:
        return base_storage == STORAGE_NONE || base_storage == STORAGE_REGISTER;
    }
    unreachable();
}

// Pure declarator-shape query used by storage-class validation without
// discarding constness or mutating the completed AST.
static bool has_function_declarator(const Declarator *declarator)
{
    for (const Declarator *layer = declarator; layer; layer = layer->inner) {
        if (layer->kind == DECLARATOR_FUNCTION)
            return true;
    }
    return false;
}

// Enforce C11 6.7.6.3p2 after the complete parameter declarator is available.
static void validate_parameter_storage_class(
    const ParsedDeclarationSpecifiers *specs, const ParsedDeclarator *decl)
{
    StorageClass base_storage =
        specs->ast->storage & (StorageClass)~STORAGE_THREAD_LOCAL;
    if (base_storage == STORAGE_NONE || base_storage == STORAGE_REGISTER)
        return;
    const Token *anchor = decl && decl->name_tok ? decl->name_tok
                                                 : specs->storage_tok;
    report_parameter_storage_class(&anchor->span, anchor->span.text);
}

// ---------------------------------------------------------------------------
// Parser diagnostic and recovery boundaries
// ---------------------------------------------------------------------------

// Handle one failed exact-token expectation while matching and cursor movement
// remain in parser.c. Token metadata is the sole source of display spelling.
void handle_parser_error_expected_token(const Token *tok, TokenKind kind)
{
    char expected[64];
    const char *name = token_kind_name(kind);
    if (token_kind_is_punctuator(kind))
        snprintf(expected, sizeof(expected), "'%s'", name);
    else
        snprintf(expected, sizeof(expected), "%s", name);
    if (kind == TOK_SEMICOLON) {
        SourceSpan anchor = span_after_previous(tok);
        report_expected_at(tok, expected, &anchor);
    } else {
        report_expected(tok, expected);
    }
}

// Handle a grammar-category expectation that cannot be represented by one
// exact TokenKind, such as "type name" or "declaration or statement".
void handle_parser_error_expected_syntax(const Token *tok, const char *what)
{
    report_expected(tok, what);
}

// Diagnose a translation-unit token that cannot begin external-declaration
// and guarantee progress before returning control to the parent boundary.
void handle_parser_error_unexpected_external_declaration(
    ParserCursor *cursor, const ParserCursor *item_start)
{
    if (token_is(current_token(cursor), TOK_SEMICOLON)) {
        report_extra_semicolon(current_token(cursor));
        consume(cursor);
        return;
    }
    report_missing_type(current_token(cursor));
    sync_top_level(cursor, item_start);
}

// Handle an already-reported external-declaration failure by synchronizing
// without selecting or reparsing another production.
void handle_parser_error_external_declaration(
    ParserCursor *cursor, const ParserCursor *item_start)
{
    sync_top_level(cursor, item_start);
}

// Handle a required expression at a caller-selected grammar boundary.
void handle_parser_error_expected_expression(const Token *tok)
{
    report_expected_expression(tok);
}

// Handle the expression form excluded by the C11 _Alignof grammar.
void handle_parser_error_alignof_expression(const Token *keyword)
{
    error_at(&keyword->span, "ISO C does not allow '_Alignof (expression)'");
}

// Handle a `return` at EOF using gcc's keyword anchor.
void handle_parser_error_return_missing_operand(const Token *eof_tok,
                                                const Token *keyword)
{
    report_expected_at(eof_tok, "expression", &keyword->span);
}

// Render one already-classified type-specifier constraint failure.
void handle_parser_error_type_specifier(const TypeSpecifierError *error)
{
    switch (error->kind) {
    case TYPE_SPEC_DUPLICATE:
        report_duplicate_type_keyword(error->anchor);
        break;
    case TYPE_SPEC_SIGN_CONFLICT:
        report_both_signed_unsigned(error->anchor);
        break;
    case TYPE_SPEC_TOO_MANY_LONG:
        report_too_many_long(error->anchor);
        break;
    case TYPE_SPEC_LONG_DOUBLE_CONFLICT:
        report_both_long_double(error->anchor);
        break;
    case TYPE_SPEC_INVALID_COMBINATION:
        report_too_many_types(error->anchor);
        break;
    case TYPE_SPEC_OK:
        unreachable();
    }
}

// Handle the C11 6.7.1p2 storage-class constraint at the first specifier.
void handle_parser_error_multiple_storage_classes(const Token *tok)
{
    error_at(&tok->span, "multiple storage classes in declaration specifiers");
}

// Classify, report and recover from the token following a completed borrowed
// init-declarator-list. A true result means recovery consumed the rejected
// nested body; ownership remains with the caller on every path.
bool handle_parser_error_declaration_tail(
    ParserCursor *cursor, const Node *declarations, DeclarationContext context)
{
    const Node *last = declarations;
    while (last->next)
        last = last->next;

    if (context == DECLARATION_BLOCK && !declarations->next &&
        declarator_function(declarations->u.init_decl.declarator) &&
        token_is(current_token(cursor), TOK_LBRACE)) {
        report_function_definition_in_block(
            declarations->u.init_decl.declarator);
        int depth = 0;
        while (!at_eof(current_token(cursor))) {
            Token *tok = consume(cursor);
            if (token_is(tok, TOK_LBRACE)) {
                depth++;
            } else if (token_is(tok, TOK_RBRACE) && --depth == 0) {
                return true;
            }
        }
        return true;
    }

    if (last->u.init_decl.init)
        report_initializer_tail(current_token(cursor));
    else
        report_declarator_tail(current_token(cursor));
    return false;
}

// Handle the C11 6.7p2 constraint that a declaration must declare something.
void handle_parser_error_useless_declaration(const Token *tok)
{
    error_at(&tok->span, "useless type name in empty declaration");
}

// Report a block truncated by EOF at the statement in progress or opening
// brace. The compound-statement parser retains its partial statement chain.
void handle_parser_error_unclosed_block(const Token *open,
                                        const Token *stmt_start)
{
    report_unclosed_block(open, stmt_start);
}

// Handle and consume an `else` that cannot begin any statement production.
void handle_parser_error_else_without_if(ParserCursor *cursor)
{
    report_else_without_if(current_token(cursor));
    consume(cursor);
}

// Report a missing parameter after entering parameter-list. The list parser
// retains and releases its partial chain.
void handle_parser_error_parameter_list(const Token *tok)
{
    report_parameter_tail(tok);
}

// Consume a classified dangling parameter-list comma and report the required
// expression at the following token; the caller retains its partial chain.
void handle_parser_error_parameter_expression(ParserCursor *cursor)
{
    if (!consume_if(cursor, TOK_COMMA))
        unreachable();
    report_expected_expression(current_token(cursor));
}

// Handle an already-reported block-item failure by synchronizing to the next
// statement boundary without emitting a second diagnostic.
void handle_parser_error_block_item(ParserCursor *cursor,
                                    const ParserCursor *item_start)
{
    sync_block_item(cursor, item_start);
}

// Diagnose a token that cannot begin block-item, then synchronize with the same
// delimiter-aware policy used after an already-reported child failure.
void handle_parser_error_expected_block_item(
    ParserCursor *cursor, const ParserCursor *item_start)
{
    report_expected(current_token(cursor), "declaration or statement");
    sync_block_item(cursor, item_start);
}

// ---------------------------------------------------------------------------
// Parser constraint validation
// ---------------------------------------------------------------------------

// Diagnose the first invalid derived layer after a complete declarator exists;
// the caller retains the full invalid shape for deterministic recovery.
void validate_declarator_constraints(const ParsedDeclarator *decl,
                                     DeclaratorContext context)
{
    DeclaratorConstraintError error;
    if (find_declarator_constraint(decl->shape, context, &error))
        report_invalid_derived_declarator(decl->shape, &error);
}

// Diagnose constraints wholly owned by one completed init-declarator without
// inspecting declaration-specifiers or its containing declaration context.
void validate_init_declarator_constraints(const Node *decl)
{
    if (decl->type != ND_INIT_DECL)
        return;
    Declarator *declarator = decl->u.init_decl.declarator;
    const Declarator *function = declarator_function(declarator);
    if (!function)
        return;
    if (decl->u.init_decl.init)
        report_function_initialized(declarator);
    if (function->u.function.form == FUNCTION_PARAMS_IDENTIFIER_LIST &&
        function->u.function.clause.old_style.identifiers)
        report_identifier_list_in_declaration(declarator);
}

// Validate only constraints shared by parameter declarations in prototypes and
// definitions after the parameter syntax parser has completed.
void validate_parameter_declaration_constraints(
    const ParsedDeclarationSpecifiers *specs, const ParsedDeclarator *decl)
{
    validate_declarator_constraints(decl, DECLARATOR_PARAMETER);
    validate_parameter_storage_class(specs, decl);
    const Token *anchor = decl->name_tok ? decl->name_tok : specs->start;
    if (specs->ast->storage & STORAGE_THREAD_LOCAL)
        error_at(&anchor->span,
                 "'_Thread_local' specified for function parameter");
    if (specs->ast->function_specifiers)
        error_at(&anchor->span,
                 "function specifier specified for function parameter");
    if (specs->ast->alignments)
        error_at(&anchor->span,
                 "alignment specified for function parameter");
}

// Enforce C11's special unadorned `void` parameter-list constraint after the
// complete syntactic list exists; ownership and canonicalization stay with
// the declaration parser.
void validate_parameter_type_list_constraints(const Node *params)
{
    const Node *void_param = NULL;
    for (const Node *param = params; param; param = param->next) {
        if (!parameter_is_plain_void(param))
            continue;
        if (void_param) {
            report_void_not_alone_at(&param->span);
            return;
        }
        void_param = param;
    }
    if (void_param && (void_param != params || void_param->next))
        report_void_not_alone_at(&void_param->span);
}

// Enforce the completed struct-declaration constraint while retaining its AST.
// A tagless aggregate definition is the sole no-declarator member form.
void validate_struct_declaration_constraints(
    const DeclarationSpecifiers *specifiers, const Node *declarators,
    const Token *semi)
{
    if (!declarators &&
        !type_specifier_is_anonymous_aggregate(specifiers->type))
        report_struct_declaration_without_declarator(semi);
}

// Enforce storage-class constraints after declaration has completed. This
// validator reports but retains the syntactically valid declaration AST.
static void validate_declaration_storage_class(
    const ParsedDeclarationSpecifiers *specs, DeclarationContext context,
    const Node *decl)
{
    const Declarator *declarator = decl->u.init_decl.declarator;
    if ((specs->ast->storage & STORAGE_THREAD_LOCAL) &&
        (context == DECLARATION_FOR ||
         context == DECLARATION_OLD_STYLE_PARAMETER ||
         (context == DECLARATION_BLOCK &&
          !(specs->ast->storage & STORAGE_EXTERN) &&
          !(specs->ast->storage & STORAGE_STATIC)))) {
        const SourceSpan *span = declarator_name_span(declarator);
        error_at(span ? span : &specs->thread_local_tok->span,
                 "invalid use of '_Thread_local'");
    }
    if (is_storage_class_allowed(specs->ast->storage, context,
                                 has_function_declarator(declarator)))
        return;
    if (context == DECLARATION_OLD_STYLE_PARAMETER) {
        const SourceSpan *span = declarator_name_span(declarator);
        const StringView *name = declarator_name(declarator);
        if (span && name)
            report_parameter_storage_class(span, *name);
        else
            report_parameter_storage_class(&specs->storage_tok->span,
                                           specs->storage_tok->span.text);
    } else if (context == DECLARATION_FILE) {
        report_external_storage_class(declarator, specs->storage_tok);
    } else if (context == DECLARATION_BLOCK) {
        report_block_function_storage_class(declarator);
    } else {
        report_for_storage_class(declarator);
    }
}

// Enforce C11 6.9.1p4 at the function-definition boundary without consuming
// input or taking ownership of the completed declarator.
void validate_function_definition_storage_class(
    const ParsedDeclarationSpecifiers *specs, const ParsedDeclarator *decl)
{
    const SourceSpan *span = declarator_name_span(decl->shape);
    if (specs->ast->storage & STORAGE_THREAD_LOCAL)
        error_at(span ? span : &specs->thread_local_tok->span,
                 "function definition declared '_Thread_local'");
    if (specs->ast->alignments)
        error_at(span ? span : &specs->alignment_tok->span,
                 "alignment specified for function definition");
    StorageClass base_storage =
        specs->ast->storage & (StorageClass)~STORAGE_THREAD_LOCAL;
    if (base_storage == STORAGE_NONE || base_storage == STORAGE_EXTERN ||
        base_storage == STORAGE_STATIC)
        return;
    report_function_definition_storage_class(decl, specs->storage_tok);
}

// Enforce the variable-only for-init constraint on one completed borrowed
// init-declarator.
static void validate_for_init_declarator(const Node *declarator,
                                         const Token *keyword)
{
    if (has_function_declarator(declarator->u.init_decl.declarator))
        report_non_variable_in_for_init(declarator, keyword);
}

// Diagnose constraints that cross declaration-specifiers,
// init-declarator-list or the containing declaration context. The declaration
// parent retains every syntactically complete AST object after diagnostics.
void validate_declaration_constraints(
    const ParsedDeclarationSpecifiers *specs, DeclarationContext context,
    const Token *context_anchor, const Node *decl)
{
    Declarator *declarator = decl->u.init_decl.declarator;
    bool is_function = declarator_function(declarator) != NULL;
    const SourceSpan *name_span = declarator_name_span(declarator);
    if ((specs->ast->storage & STORAGE_THREAD_LOCAL) && is_function)
        error_at(name_span ? name_span : &specs->thread_local_tok->span,
                 "function declared '_Thread_local'");
    if (specs->ast->function_specifiers && !is_function)
        error_at(name_span ? name_span : &specs->function_specifier_tok->span,
                 "function specifier applied to non-function declaration");
    if (specs->ast->alignments &&
        (is_function || (specs->ast->storage & STORAGE_TYPEDEF) ||
         (specs->ast->storage & STORAGE_REGISTER)))
        error_at(name_span ? name_span : &specs->alignment_tok->span,
                 "alignment specified for invalid declaration");
    if ((specs->ast->storage & STORAGE_TYPEDEF) && decl->u.init_decl.init)
        report_typedef_initialized(declarator);
    validate_declaration_storage_class(specs, context, decl);
    if (context == DECLARATION_FOR)
        validate_for_init_declarator(decl, context_anchor);
}

// Diagnose a direct named `void` parameter only after its parent has selected
// function-definition. Array and function parameters adjust to pointer types.
static void validate_named_void_definition_parameter(const Node *parameter,
                                                      int index)
{
    const DeclarationSpecifiers *specs = parameter->u.parameter.specifiers;
    const Declarator *declarator = parameter->u.parameter.declarator;
    if (specs->type->kind != TYPE_SPECIFIER_BUILTIN ||
        declaration_specifiers_builtin_type(specs) != TYPE_VOID ||
        !declarator || declarator_entity_layer(declarator))
        return;
    const SourceSpan *name = declarator_name_span(declarator);
    if (name)
        report_void_parameter(name, index);
}

// Enforce definition-only parameter constraints after external-declaration has
// selected the function-definition alternative and retained its parameter AST.
void validate_function_definition_constraints(const Declarator *function)
{
    if (function->u.function.form != FUNCTION_PARAMS_PROTOTYPE)
        return;
    int index = 1;
    for (const Node *param =
             function->u.function.clause.prototype.parameters;
         param;
         param = param->next, index++) {
        validate_named_void_definition_parameter(param, index);
        const Declarator *array = find_star_array(param->u.parameter.declarator);
        if (array)
            error_at(&array->span,
                     "'[*]' not allowed in other than function prototype scope");
    }
}

// Enforce the declaration-list constraints after the complete syntactic child
// has been retained by its function-definition owner.
void validate_function_definition_declarations(const Declarator *function,
                                                const Node *declarations)
{
    if (!declarations)
        return;
    if (function->u.function.form != FUNCTION_PARAMS_IDENTIFIER_LIST) {
        error_at(&declarations->span,
                 "declaration list requires an identifier-list function definition");
        return;
    }

    const Node *identifiers =
        function->u.function.clause.old_style.identifiers;
    for (const Node *group = declarations; group; group = group->next) {
        for (const Node *decl = group->u.decl.declarators; decl;
             decl = decl->next) {
            const StringView *name =
                declarator_name(decl->u.init_decl.declarator);
            const SourceSpan *span =
                declarator_name_span(decl->u.init_decl.declarator);
            unsigned identifier_count =
                name ? old_style_identifier_count(identifiers, *name) : 0;
            if (identifier_count == 0) {
                StringView displayed = name ? *name : (StringView){ 0 };
                error_at(
                    span ? span : &decl->span,
                    "declaration for parameter '%.*s' but no such parameter",
                    view_precision(displayed), string_view_begin(displayed));
                continue;
            }
            if (identifier_count > 1)
                error_at(span, "multiple parameters named '%.*s'",
                         view_precision(*name), string_view_begin(*name));
            if (decl->u.init_decl.init)
                error_at(span, "parameter '%.*s' is initialized",
                         view_precision(*name), string_view_begin(*name));
            bool reached = false;
            for (const Node *prior_group = declarations;
                 prior_group && !reached;
                 prior_group = prior_group->next) {
                for (const Node *prior = prior_group->u.decl.declarators;
                     prior; prior = prior->next) {
                    if (prior == decl) {
                        reached = true;
                        break;
                    }
                    const StringView *prior_name =
                        declarator_name(prior->u.init_decl.declarator);
                    if (prior_name && string_view_equal(*prior_name, *name)) {
                        error_at(span, "redefinition of parameter '%.*s'",
                                 view_precision(*name),
                                 string_view_begin(*name));
                        reached = true;
                        break;
                    }
                }
            }
        }
    }

    for (const Node *identifier = identifiers; identifier;
         identifier = identifier->next) {
        bool declared = false;
        for (const Node *group = declarations; group && !declared;
             group = group->next) {
            for (const Node *decl = group->u.decl.declarators; decl;
                 decl = decl->next) {
                const StringView *name =
                    declarator_name(decl->u.init_decl.declarator);
                if (name &&
                    string_view_equal(*name,
                                      identifier->u.identifier.name)) {
                    declared = true;
                    break;
                }
            }
        }
        if (!declared)
            error_at(&identifier->span,
                     "declaration for parameter '%.*s' is missing",
                     view_precision(identifier->u.identifier.name),
                     string_view_begin(identifier->u.identifier.name));
    }
}

// C11 6.5.1.1 permits at most one default generic association. Type
// compatibility and controlling-expression selection remain semantic work.
void validate_generic_selection_constraints(
    const GenericAssociation *associations)
{
    bool found_default = false;
    for (const GenericAssociation *association = associations; association;
         association = association->next) {
        if (association->type_name)
            continue;
        if (found_default)
            error_at(&association->span,
                     "multiple default generic associations");
        found_default = true;
    }
}
