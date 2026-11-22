// Recursive-descent parser subsystem entry point: cursor/expectation
// primitives and external-definition dispatch. The cursor is passed
// explicitly instead of living in a file-scope variable: it changes on almost
// every token, so threading it is the normal, cheap design. Truly global state
// such as the current source file is accessed through helpers (see source.c)
// rather than passed everywhere; span positions are resolved against that
// current source (the tokenize_file contract). Syntax errors are reported and
// recovered from, so one run can surface every independent error in the file;
// control flow, recursive declarators, type names, casts and initializers are
// retained for later semantic analysis.

#include "parser_internal.h"

#include "config.h"
#include "util/common.h"
#include "util/diagnostic.h"
#include "util/hashmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct NameScope {
    HashMap names;
    NameScope *parent;
};

static char g_ordinary_binding;
static char g_typedef_binding;

// ---------------------------------------------------------------------------
// Parser primitives
// ---------------------------------------------------------------------------

// The primitives match exact TokenKind values. Their contract differs only in
// reporting: consume/consume_if are silent, while expect reports once through
// diagnostic.c.

// Unconditional advance: returns the consumed token, or NULL at EOF without
// moving the cursor. Callers that need the token keep the result.
Token *current_token(const ParserCursor *cursor)
{
    return cursor->token;
}

Token *peek_token(const ParserCursor *cursor, size_t distance)
{
    Token *tok = current_token(cursor);
    while (distance-- && tok)
        tok = tok->next;
    return tok;
}

Token *consume(ParserCursor *cursor)
{
    Token *tok = current_token(cursor);
    if (token_is(tok, TOK_EOF))
        return NULL;
    cursor->token = tok->next;
    return tok;
}

// Compare a token against one exact lexical terminal.
bool token_is(const Token *tok, TokenKind kind)
{
    return tok && tok->kind == kind;
}

bool at_eof(const Token *tok)
{
    return token_is(tok, TOK_EOF);
}

// Silent matching consume: on a hit advance and return the token; otherwise
// leave the cursor alone and return NULL. EOF is never consumed.
Token *consume_if(ParserCursor *cursor, TokenKind kind)
{
    if (kind == TOK_EOF || !token_is(current_token(cursor), kind))
        return NULL;
    return consume(cursor);
}

// Required matching consume: a miss leaves the cursor unchanged and reports
// the stable spelling/name associated with the exact kind.
Token *expect(ParserCursor *cursor, TokenKind kind)
{
    Token *tok = consume_if(cursor, kind);
    if (!tok)
        handle_parser_error_expected_token(current_token(cursor), kind);
    return tok;
}

// Silently advance through one balanced delimiter group. Callers performing
// lookahead pass a copied cursor; malformed crossings or EOF leave the supplied
// cursor advanced and return false without diagnostics or allocation.
bool skip_balanced_group(ParserCursor *cursor, TokenKind open, TokenKind close)
{
    if (!consume_if(cursor, open))
        return false;
    int parens = open == TOK_LPAREN;
    int brackets = open == TOK_LBRACKET;
    int braces = open == TOK_LBRACE;
    while (!at_eof(current_token(cursor))) {
        Token *tok = consume(cursor);
        if (token_is(tok, TOK_LPAREN))
            parens++;
        else if (token_is(tok, TOK_RPAREN))
            parens--;
        else if (token_is(tok, TOK_LBRACKET))
            brackets++;
        else if (token_is(tok, TOK_RBRACKET))
            brackets--;
        else if (token_is(tok, TOK_LBRACE))
            braces++;
        else if (token_is(tok, TOK_RBRACE))
            braces--;
        if (parens < 0 || brackets < 0 || braces < 0)
            return false;
        if (parens == 0 && brackets == 0 && braces == 0)
            return token_is(tok, close);
    }
    return false;
}

// Identifier expectation, built from the generic primitives so it does not
// repeat the matching logic. The spelling matches gcc's declarator message
// (`identifier or '('`, since C11 6.7 allows a parenthesized declarator).
Token *expect_identifier(ParserCursor *cursor)
{
    Token *tok = consume_if(cursor, TOK_IDENTIFIER);
    if (!tok)
        handle_parser_error_expected_syntax(current_token(cursor),
                                            "identifier or '('");
    return tok;
}

// Enter one lexical ordinary-identifier scope. The scope owns only its hash
// buckets; token-backed spelling remains owned by the lexer.
void parser_push_scope(ParserCursor *cursor)
{
    NameScope *scope = xcalloc(1, sizeof(*scope));
    scope->parent = cursor->scope;
    cursor->scope = scope;
}

// Leave the current lexical scope and restore its parent. Every parser entry
// that pushes a scope must pop it on both success and failure paths.
void parser_pop_scope(ParserCursor *cursor)
{
    if (!cursor->scope)
        unreachable();
    NameScope *scope = cursor->scope;
    cursor->scope = scope->parent;
    hashmap_free(&scope->names);
    free(scope);
}

// Resolve an identifier in the nearest ordinary namespace. An ordinary
// declaration is a positive result that hides any outer typedef spelling.
bool parser_name_is_typedef(const ParserCursor *cursor, const Token *name)
{
    if (!token_is(name, TOK_IDENTIFIER))
        return false;
    for (const NameScope *scope = cursor->scope; scope; scope = scope->parent) {
        void *value = hashmap_get(&scope->names, name->span.text);
        if (value)
            return value == &g_typedef_binding;
    }
    return false;
}

// Bind one borrowed spelling in the current ordinary namespace. Rebinding
// replaces the classification used by subsequent grammar disambiguation.
void parser_declare_name(ParserCursor *cursor, const StringView *name,
                         bool is_typedef)
{
    if (!cursor->scope || !name || !string_view_length(*name))
        return;
    hashmap_put(&cursor->scope->names, *name,
                is_typedef ? &g_typedef_binding : &g_ordinary_binding);
}

// The current subset covers expressions, statements (including if/else, the
// three loop forms, break/continue), simple declarations (local and global) and
// functions, prototypes, calls and recursive declarators. Errors are reported
// and recovered from at statement/function boundaries so one run surfaces
// every independent error; the first error does not abort.
// ponytail: '#'-directive lines are skipped wholesale because the preprocessor
// is not implemented yet; delete skip_directives() when it lands (TODO.md).
void skip_directives(ParserCursor *cursor)
{
    while (token_is(current_token(cursor), TOK_HASH)) {
        unsigned line = current_token(cursor)->span.line;
        while (!at_eof(current_token(cursor)) && current_token(cursor)->span.line == line)
            consume(cursor);
    }
}
// ---------------------------------------------------------------------------
// A.2.4 External definitions (C11 6.9)
// ---------------------------------------------------------------------------

// (6.9) translation-unit:
//     external-declaration
//     translation-unit external-declaration
// translation-unit (C11 6.9): external declarations chained through `next`.
static Node *parse_translation_unit(ParserCursor *cursor)
{
    Node *head = NULL;
    Node *tail = NULL;

    while (true) {
        skip_directives(cursor);
        if (at_eof(current_token(cursor)))
            break;

        ParserCursor item_start = *cursor;
        if (!starts_external_declaration(cursor)) {
            handle_parser_error_unexpected_external_declaration(
                cursor, &item_start);
            continue;
        }
        Node *declaration = NULL;
        if (!parse_external_declaration(cursor, &declaration)) {
            handle_parser_error_external_declaration(cursor, &item_start);
            continue;
        }
        if (declaration)
            append_node_list(&head, &tail, declaration);
    }

    return new_program(head);
}

// Non-grammar parser API: parse a token list through translation-unit.
// Recoverable syntax errors are reported and parsing continues, so the returned
// tree may be partial; NULL is only returned for a NULL token list. The caller
// decides validity with error_count() and keeps ownership of the token list.
Node *parse(Token *tokens)
{
    if (!tokens)
        return NULL;
    ParserCursor cursor = { .token = tokens };
    parser_push_scope(&cursor);
    Node *tree = parse_translation_unit(&cursor);
    parser_pop_scope(&cursor);
    return tree;
}

// Non-grammar lifecycle API: release the complete tree returned by parse().
void parse_free(Node *tree)
{
    node_free(tree);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#ifdef TEST_PARSER

static int failures = 0;

typedef struct TestSourceOwner TestSourceOwner;

struct TestSourceOwner {
    SourceFile *file;
    TestSourceOwner *next;
};

static TestSourceOwner *g_test_sources;

bool parser_test_initializer_list_leaves_trailing_comma(void);
static bool declarator_is_named(const Declarator *declarator,
                                const char *name);

// Tokenize+parse a source string without a SourceFile fixture (tests below).
static Node *parse_snippet(const char *src);

static void check(bool ok, const char *msg)
{
    if (ok) {
        printf("ok   : %s\n", msg);
    } else {
        printf("FAIL : %s\n", msg);
        failures++;
    }
}

static Token *load_basic(void)
{
    Token *tokens = tokenize_file("tests/tests/basic.c");
    if (!tokens) {
        fprintf(stderr, "parser_test: cannot load tests/tests/basic.c\n");
        exit(1);
    }
    return tokens;
}

static void release(Token *tokens)
{
    token_free(tokens);
    source_free(current_source());
}

// Walk the whole stream of `tests/tests/basic.c` with the consumption primitives.
static void test_consumption(void)
{
    Token *tokens = load_basic();
    ParserCursor cursor = { .token = tokens };

    check(consume_if(&cursor, TOK_KW_INT) != NULL, "consume_if(int)");

    Token *name_tok = expect_identifier(&cursor);
    check(name_tok != NULL, "expect_identifier() finds the name");
    if (name_tok)
        check(token_is(name_tok, TOK_IDENTIFIER) &&
                  string_view_equal_cstr(name_tok->span.text, "main"),
              "expect_identifier() returns main");

    check(expect(&cursor, TOK_LPAREN) != NULL, "expect(()");
    check(expect(&cursor, TOK_RPAREN) != NULL, "expect())");
    check(expect(&cursor, TOK_LBRACE) != NULL, "expect({)");
    check(consume_if(&cursor, TOK_KW_RETURN) != NULL, "consume_if(return)");
    check(consume_if(&cursor, TOK_NUMBER) != NULL, "consume_if(NUMBER)");
    check(expect(&cursor, TOK_SEMICOLON) != NULL, "expect(;)");
    check(expect(&cursor, TOK_RBRACE) != NULL, "expect(})");
    check(at_eof(current_token(&cursor)), "cursor reaches EOF");

    release(tokens);
}

// Two cursors over the same stream must not disturb each other.
static void test_independent_cursors(void)
{
    Token *tokens = load_basic();
    ParserCursor a = { .token = tokens };
    ParserCursor b = { .token = tokens };

    check(consume_if(&a, TOK_KW_INT) != NULL, "cursor a consumes int");
    check(token_is(current_token(&b), TOK_KW_INT),
          "cursor b stays on int");
    check(consume_if(&b, TOK_KW_INT) != NULL,
          "cursor b consumes int independently");

    release(tokens);
}

// Token spelling never overrides its lexical category: an identifier whose
// bytes happen to spell a keyword must remain an identifier to the parser.
static void test_token_type_precedes_spelling(void)
{
    static char spelling[] = "int";
    Token ident = {
        .kind = TOK_IDENTIFIER,
        .span = { .text = string_view(spelling, 3) },
    };

    check(token_is(&ident, TOK_IDENTIFIER),
          "identifier keeps its exact lexical kind");
    check(!token_is(&ident, TOK_KW_INT),
          "identifier spelling does not match a keyword");
    ParserCursor cursor = { .token = &ident };
    check(!starts_declaration(&cursor),
          "identifier spelling does not start a declaration as a keyword");
}

static void test_function_definition_dispatch(void)
{
    Node *root = parse_snippet("unsigned long inner(void) { return 0; }");
    Node *function = root ? root->u.program.external_declarations : NULL;

    check(function && function->type == ND_FUNCTION,
          "external-declaration dispatches a function definition");
    check(function &&
              declarator_is_named(function->u.func.declarator, "inner"),
          "function dispatch preserves the declarator name");
    node_free(root);
}

static void test_expected_failures(void)
{
    int before = error_count();
    Token *tokens = load_basic();
    ParserCursor cursor = { .token = tokens };

    // A failed expect reports once, returns NULL and leaves the cursor in place.
    check(expect(&cursor, TOK_LBRACE) == NULL, "expect({) fails on int");
    check(error_count() == before + 1, "failure was reported once");
    check(consume_if(&cursor, TOK_KW_INT) != NULL,
          "cursor did not advance after failure");

    // expect() matches the type only; expect_identifier() is its identifier form.
    Token *name_tok = expect_identifier(&cursor);
    check(name_tok != NULL, "expect_identifier() recovers after failure");
    if (name_tok)
        check(token_is(name_tok, TOK_IDENTIFIER) &&
                  string_view_equal_cstr(name_tok->span.text, "main"),
              "expect_identifier() returns main");
    check(expect(&cursor, TOK_NUMBER) == NULL, "expect(number) fails on ()");
    check(error_count() == before + 2, "type-only failure was reported once");

    // consume() never reports and never consumes EOF.
    ParserCursor eof = cursor;
    while (!at_eof(current_token(&eof)))
        consume(&eof);
    check(current_token(&eof) != NULL, "stream has EOF");
    check(consume(&eof) == NULL, "consume(EOF) returns NULL");
    check(at_eof(current_token(&eof)), "cursor stays on EOF");

    release(tokens);
}

// Build every node shape and tear the tree down; ASan reports any leak.
static void test_constructors(void)
{
    static char snippet_source[] = "";
    static SourceFile snippet_file = {
        .name = (char *)"<snippet>",
        .contents = snippet_source,
    };
    SourceSpan span = {
        .file = &snippet_file,
        .text = string_view(snippet_source, 0),
    };
    Token op_tok = { .span = span };
    Token semi_tok = { .span = span };
    Token open_tok = { .span = span };
    Token close_tok = { .span = span };
    Token type_tok = { .span = span };

    Node *blank = new_empty(span);
    check(blank->type == ND_EMPTY && blank->next == NULL,
          "new_empty zeroes the node");
    node_free(blank);

    Token tok = { 0 };
    tok.span = span;
    tok.kind = TOK_NUMBER;
    tok.value.integer = 42;
    tok.literal = LITERAL_INT;
    Node *num = new_num(&tok);
    check(num->type == ND_NUM && num->u.num.value.integer == 42 &&
              num->u.num.literal == LITERAL_INT &&
              num->next == NULL,
          "new_num copies an integer literal");

    Token ftok = { 0 };
    ftok.span = span;
    ftok.kind = TOK_NUMBER;
    ftok.value.floating = 1.5L;
    ftok.literal = LITERAL_DOUBLE;
    Node *fnum = new_num(&ftok);
    check(fnum->u.num.value.floating == 1.5L &&
              fnum->u.num.literal == LITERAL_DOUBLE,
          "new_num copies a floating literal");

    Token string_tok = { .kind = TOK_STRING, .span = span };
    string_tok.str = xmalloc(4);
    memcpy(string_tok.str, "a\0b", 3);
    string_tok.str[3] = '\0';
    string_tok.str_len = 3;
    char *decoded_string = string_tok.str;
    Node *string = new_string(&string_tok);
    check(string->u.string.value == decoded_string &&
              string->u.string.length == 3 && string_tok.str == NULL &&
              string_tok.str_len == 0,
          "new_string transfers the decoded token buffer");
    node_free(string);
    free(string_tok.str);

    Token name_tok = { .kind = TOK_IDENTIFIER, .span = span };
    Node *identifier = new_identifier(&name_tok);
    check(identifier->type == ND_IDENTIFIER &&
              string_view_begin(identifier->u.identifier.name) == snippet_source &&
              string_view_length(identifier->u.identifier.name) == 0,
          "new_identifier borrows the identifier spelling");

    DeclarationSpecifiers *decl_specs = new_declaration_specifiers(
        new_builtin_type_specifier(TYPE_INT, &type_tok, &type_tok), 0,
        STORAGE_NONE, 0, NULL, &type_tok, &type_tok);
    Node *decl_item = new_init_declarator(
        identifier, new_identifier_declarator(&name_tok));
    Node *decl = new_declaration(decl_specs, decl_item, &semi_tok);
    check(decl->type == ND_DECL &&
              declaration_specifiers_builtin_type(decl->u.decl.specifiers) ==
                  TYPE_INT &&
              declarator_is_named(decl_item->u.init_decl.declarator, "") &&
              decl_item->u.init_decl.init == identifier,
          "new_declaration takes ownership of specifiers and init-declarator");

    DeclarationSpecifiers *empty_specs = new_declaration_specifiers(
        new_builtin_type_specifier(TYPE_INT, &type_tok, &type_tok), 0,
        STORAGE_NONE, 0, NULL, &type_tok, &type_tok);
    Node *empty_item = new_init_declarator(
        NULL, new_identifier_declarator(&name_tok));
    Node *empty_decl = new_declaration(empty_specs, empty_item, &semi_tok);
    decl->next = empty_decl;
    Node *decl_block = new_block(decl, &open_tok, &close_tok);
    check(empty_item->u.init_decl.init == NULL,
          "init-declarator accepts a missing initializer");
    node_free(decl_block);

    Node *un = new_unary(UNARY_NEG, &op_tok, num);
    check(un->type == ND_UNARY && un->u.unary.op == UNARY_NEG && un->u.unary.operand == num,
          "new_unary links its operand");

    Node *bin = new_binary(BINARY_ADD, un, fnum);
    check(bin->type == ND_BINARY && bin->u.binary.op == BINARY_ADD && bin->u.binary.lhs == un &&
              bin->u.binary.rhs == fnum,
          "new_binary links both operands");

    Node *stmt = new_expr_stmt(bin, &semi_tok);
    check(stmt->type == ND_EXPR_STMT && stmt->u.expr.expr == bin,
          "new_expr_stmt links its expression");

    Node *empty_ret = new_return(NULL, &op_tok, &semi_tok);
    check(empty_ret->type == ND_RETURN && empty_ret->u.expr.expr == NULL,
          "new_return accepts an empty expression");

    Node *ret = new_return(stmt, &op_tok, &semi_tok);
    ret->next = empty_ret;
    Node *block = new_block(ret, &open_tok, &close_tok);
    check(block->type == ND_BLOCK && block->u.block.items == ret &&
              ret->next == empty_ret,
          "new_block threads a block-item list");

    char function_name[] = "main";
    Token function_name_tok = {
        .kind = TOK_IDENTIFIER,
        .span = {
            .file = &snippet_file,
            .text = string_view(function_name, 4),
        },
    };
    Declarator *fn_shape = new_function_declarator(
        new_identifier_declarator(&function_name_tok), NULL,
        FUNCTION_PARAMS_IDENTIFIER_LIST, false,
        &open_tok, &close_tok);
    DeclarationSpecifiers *fn_specs = new_declaration_specifiers(
        new_builtin_type_specifier(TYPE_INT, &type_tok, &type_tok), 0,
        STORAGE_NONE, 0, NULL, &type_tok, &type_tok);
    Node *fn = new_function(fn_specs, fn_shape, NULL, block);
    check(fn->type == ND_FUNCTION &&
              declaration_specifiers_builtin_type(fn->u.func.specifiers) ==
                  TYPE_INT &&
              declarator_is_named(fn->u.func.declarator, "main") &&
              string_view_begin(*declarator_name(fn->u.func.declarator)) == function_name &&
              fn->u.func.body == block &&
              declarator_function(fn->u.func.declarator)
                      ->u.function.clause.old_style.identifiers == NULL,
          "new_function takes ownership of the recursive declarator");
    char other_name[] = "other";
    Token other_name_tok = {
        .kind = TOK_IDENTIFIER,
        .span = {
            .file = &snippet_file,
            .text = string_view(other_name, 5),
        },
    };
    Declarator *other_shape = new_function_declarator(
        new_identifier_declarator(&other_name_tok), NULL,
        FUNCTION_PARAMS_IDENTIFIER_LIST, false,
        &open_tok, &close_tok);
    Node *other_block = new_block(NULL, &open_tok, &close_tok);
    DeclarationSpecifiers *other_specs = new_declaration_specifiers(
        new_builtin_type_specifier(TYPE_INT, &type_tok, &type_tok), 0,
        STORAGE_NONE, 0, NULL, &type_tok, &type_tok);
    fn->next = new_function(other_specs, other_shape, NULL, other_block);
    Node *program = new_program(fn);
    check(program->type == ND_PROGRAM &&
              program->u.program.external_declarations == fn &&
              fn->next != NULL,
          "new_program threads an external-declaration list");

    Node *i1 = new_num(&tok);
    Node *i2 = new_num(&tok);
    Node *if_node = new_if(i1, i2, NULL, &open_tok);
    check(if_node->type == ND_IF && if_node->u.if_stmt.cond == i1 &&
              if_node->u.if_stmt.then_stmt == i2 && if_node->u.if_stmt.else_stmt == NULL,
          "new_if links the missing else as NULL");
    node_free(if_node);

    Node *w1 = new_num(&tok);
    Node *w2 = new_num(&tok);
    Node *while_node = new_while(w1, w2, &open_tok);
    check(while_node->type == ND_WHILE && while_node->u.while_stmt.cond == w1 &&
              while_node->u.while_stmt.body == w2,
          "new_while links condition and body");
    node_free(while_node);

    Node *dw_body = new_num(&tok);
    Node *dw_cond = new_num(&tok);
    Node *do_node = new_do_while(dw_body, dw_cond, &open_tok, &semi_tok);
    check(do_node->type == ND_DO_WHILE && do_node->u.do_while.body == dw_body &&
              do_node->u.do_while.cond == dw_cond,
          "new_do_while links body and condition");
    node_free(do_node);

    Node *f_init = new_num(&tok);
    Node *f_cond = new_num(&tok);
    Node *f_post = new_num(&tok);
    Node *f_body = new_num(&tok);
    Node *for_node = new_for(f_init, f_cond, f_post, f_body, &open_tok);
    check(for_node->type == ND_FOR && for_node->u.for_stmt.init == f_init &&
              for_node->u.for_stmt.cond == f_cond && for_node->u.for_stmt.post == f_post &&
              for_node->u.for_stmt.body == f_body,
          "new_for links all four parts");
    node_free(for_node);

    Node *jump = new_jump(ND_BREAK, &op_tok, &semi_tok);
    check(jump->type == ND_BREAK, "new_jump builds a jump node");
    node_free(jump);

    Node *callee = new_identifier(&name_tok);
    Node *arg = new_num(&tok);
    Node *call_node = new_call(callee, arg, &close_tok);
    check(call_node->type == ND_CALL && call_node->u.call.callee == callee &&
              call_node->u.call.args == arg,
          "new_call links callee and arguments");
    node_free(call_node);

    parse_free(program);
    node_free(NULL);
    check(true, "node_free handles NULL and a full tree");
}

static void test_deep_tree_release(void)
{
    Node *root = NULL;
    Token op_tok = {0};
    for (int i = 0; i < 100000; i++) {
        Node *node = new_unary_shell(UNARY_NEG, &op_tok);
        node->u.unary.operand = root;
        root = node;
    }
    node_free(root);
    check(true, "node_free releases a deep tree iteratively");

    static char generic_source[] = "";
    static SourceFile generic_file = {
        .name = (char *)"<generic-release>",
        .contents = generic_source,
    };
    Token generic_tok = {
        .kind = TOK_NUMBER,
        .span = {
            .file = &generic_file,
            .text = string_view(generic_source, 0),
        },
    };
    root = new_num(&generic_tok);
    for (int i = 0; i < 100000; i++) {
        GenericAssociation *association =
            new_generic_association(NULL, root, &generic_tok);
        root = new_generic_selection(&generic_tok, new_num(&generic_tok),
                                     association, &generic_tok);
    }
    node_free(root);
    check(true, "node_free releases deep generic selections iteratively");
}

// Non-grammar test helper: parse a whole file and retain its source until every
// returned AST has been inspected and released. Tokens can be freed immediately.
static Node *parse_file(const char *path)
{
    Token *tokens = tokenize_file(path);
    if (!tokens) {
        fprintf(stderr, "parser_test: cannot load %s\n", path);
        exit(1);
    }
    Node *root = parse(tokens);
    TestSourceOwner *owner = xmalloc(sizeof(*owner));
    owner->file = current_source();
    owner->next = g_test_sources;
    g_test_sources = owner;
    token_free(tokens);
    return root;
}

// Release source fixtures after all source-backed AST views have expired.
static void release_test_sources(void)
{
    while (g_test_sources) {
        TestSourceOwner *owner = g_test_sources;
        g_test_sources = owner->next;
        source_free(owner->file);
        free(owner);
    }
}

#define TP_TESTS "tests/third_party/writing-a-c-compiler-tests/tests"

static Node *first_function(Node *program)
{
    if (!program || program->type != ND_PROGRAM)
        return NULL;
    return program->u.program.external_declarations;
}

static Node *first_statement(Node *function)
{
    if (!function || function->type != ND_FUNCTION || !function->u.func.body)
        return NULL;
    return function->u.func.body->u.block.items;
}

// Return the first init-declarator from either its declaration owner or an
// already selected init-declarator test value.
static Node *first_init_declarator(Node *node)
{
    if (!node)
        return NULL;
    return node->type == ND_DECL ? node->u.decl.declarators : node;
}

// Test-facing access to the declaration-specifiers owner for declarations,
// parameters and function definitions.
static DeclarationSpecifiers *node_declaration_specifiers(Node *node)
{
    if (!node)
        return NULL;
    if (node->type == ND_FUNCTION)
        return node->u.func.specifiers;
    if (node->type == ND_PARAMETER)
        return node->u.parameter.specifiers;
    return node->type == ND_DECL ? node->u.decl.specifiers : NULL;
}

// Test-facing declarator access follows the grammar owner instead of exposing
// compatibility fields on declaration nodes.
static Declarator *node_declarator(Node *node)
{
    if (!node)
        return NULL;
    if (node->type == ND_FUNCTION)
        return node->u.func.declarator;
    if (node->type == ND_PARAMETER)
        return node->u.parameter.declarator;
    Node *item = first_init_declarator(node);
    return item && item->type == ND_INIT_DECL
               ? item->u.init_decl.declarator
               : NULL;
}

static Node *node_initializer(Node *node)
{
    Node *item = first_init_declarator(node);
    return item && item->type == ND_INIT_DECL ? item->u.init_decl.init : NULL;
}

// Test-facing accessors use the same recursive declarator source of truth as
// production code instead of reconstructing removed compatibility fields.
static const Declarator *node_function_declarator(Node *node)
{
    if (!node)
        return NULL;
    return declarator_function(node_declarator(node));
}

// Return the typed prototype parameters or the declaration-list owned by an
// old-style function definition; other declarators intentionally yield NULL.
static Node *node_function_params(Node *node)
{
    const Declarator *function = node_function_declarator(node);
    if (!function)
        return NULL;
    if (function->u.function.form == FUNCTION_PARAMS_PROTOTYPE)
        return function->u.function.clause.prototype.parameters;
    return node->type == ND_FUNCTION ? node->u.func.declarations : NULL;
}

static Node *first_return_expr(Node *function)
{
    Node *stmt = first_statement(function);
    if (!stmt || stmt->type != ND_RETURN)
        return NULL;
    return stmt->u.expr.expr;
}

static bool is_num(Node *node, int64_t val)
{
    return node && node->type == ND_NUM && node->u.num.value.integer == val;
}

static bool is_binary(Node *node, Operator op)
{
    return node && node->type == ND_BINARY && node->u.binary.op == op;
}

static bool is_identifier(Node *node, const char *name)
{
    return node && node->type == ND_IDENTIFIER &&
           string_view_equal_cstr(node->u.identifier.name, name);
}

static bool declarator_is_named(const Declarator *declarator,
                                const char *name)
{
    const StringView *actual = declarator_name(declarator);
    return actual && string_view_equal_cstr(*actual, name);
}

static bool is_decl(Node *node, TypeKind type, const char *name)
{
    DeclarationSpecifiers *specifiers = node_declaration_specifiers(node);
    Declarator *declarator = node_declarator(node);
    return node && (node->type == ND_DECL || node->type == ND_INIT_DECL ||
                    node->type == ND_PARAMETER) &&
           (node->type == ND_INIT_DECL ||
            (specifiers &&
             declaration_specifiers_builtin_type(specifiers) == type)) &&
           declarator_is_named(declarator, name);
}

// Local/global simple declarations: name, optional assignment-expression
// initializer, and identifier uses as ND_IDENTIFIER.
static void test_declarations(void)
{
    int before = error_count();

    Node *root = parse_snippet("int main(void) { int a; a = 1; return a; }");
    Node *decl = first_statement(first_function(root));
    check(error_count() == before, "simple declarations are accepted");
    check(is_decl(decl, TYPE_INT, "a"), "local declaration keeps its name");
    Node *assign = decl ? decl->next : NULL;
    check(assign && assign->type == ND_EXPR_STMT && assign->u.expr.expr->type == ND_ASSIGN &&
              is_identifier(assign->u.expr.expr->u.assign.lhs, "a"),
          "assignment lhs is an identifier");
    Node *ret = assign ? assign->next : NULL;
    check(ret && ret->type == ND_RETURN &&
              is_identifier(ret->u.expr.expr, "a"),
          "return operand is an identifier");
    node_free(root);

    root = parse_snippet("int main(void) { int a = 2 + 3; return a; }");
    decl = first_statement(first_function(root));
    check(is_decl(decl, TYPE_INT, "a") && is_binary(node_initializer(decl), BINARY_ADD),
          "declaration initializer is an assignment-expression");
    node_free(root);

    root = parse_snippet("int g; int h = 3; int main(void) { return h; }");
    Node *g = first_function(root);
    Node *h = g ? g->next : NULL;
    Node *main_fn = h ? h->next : NULL;
    check(is_decl(g, TYPE_INT, "g") && is_decl(h, TYPE_INT, "h") &&
              node_initializer(h) && node_initializer(h)->type == ND_NUM && main_fn &&
              main_fn->type == ND_FUNCTION &&
              declarator_is_named(main_fn->u.func.declarator, "main"),
          "global declarations mix with function definitions");
    node_free(root);

    root = parse_snippet("int a = 1, b = 2; int f(void), g;");
    Node *da = root->u.program.external_declarations;
    Node *db = da && da->u.decl.declarators
                   ? da->u.decl.declarators->next
                   : NULL;
    Node *df = da ? da->next : NULL;
    Node *dg = df && df->u.decl.declarators
                   ? df->u.decl.declarators->next
                   : NULL;
    check(is_decl(da, TYPE_INT, "a") && node_initializer(da) &&
              is_decl(db, TYPE_INT, "b") && is_decl(df, TYPE_INT, "f") &&
              is_decl(dg, TYPE_INT, "g") && dg->next == NULL,
          "one declaration owns its comma-separated init-declarators");
    node_free(root);

    root = parse_snippet("int main(void) { int a = 0 && a; int b = a; return b; }");
    decl = first_statement(first_function(root));
    check(is_decl(decl, TYPE_INT, "a") &&
              is_binary(node_initializer(decl), BINARY_LOGAND),
          "initializer may reference a variable holding an indeterminate value");
    node_free(root);

    root = parse_snippet("int main(void) { int; return 0; }");
    decl = first_statement(first_function(root));
    check(error_count() == before + 1, "empty declaration is rejected");
    check(decl && decl->type == ND_DECL && !decl->u.decl.declarators &&
              decl->next && decl->next->type == ND_RETURN,
          "constraint-invalid empty declaration retains its complete AST");
    node_free(root);

    int top_level_before = error_count();
    root = parse_snippet("; ; int retained;");
    Node *retained = root ? root->u.program.external_declarations : NULL;
    check(error_count() == top_level_before + 2,
          "top-level empty declarations are rejected");
    check(retained && retained->type == ND_DECL &&
              declarator_is_named(node_declarator(retained), "retained"),
          "top-level recovery retains the following declaration");
    node_free(root);

    int combos = error_count();
    root = parse_snippet("unsigned long long a;");
    check(error_count() == combos && root->u.program.external_declarations &&
              is_decl(root->u.program.external_declarations, TYPE_ULLONG, "a"),
          "unsigned long long resolves");
    node_free(root);

    combos = error_count();
    root = parse_snippet("unsigned char b; signed char c; unsigned short d; long double e;");
    check(error_count() == combos, "prefix type-specifier combinations resolve");
    node_free(root);

    combos = error_count();
    root = parse_snippet("short int a; signed short int b; unsigned short int c; "
                         "long int d; long long int e;");
    Node *n = root->u.program.external_declarations;
    check(error_count() == combos && is_decl(n, TYPE_SHORT, "a") && n->next &&
              is_decl(n->next, TYPE_SHORT, "b") && n->next->next &&
              is_decl(n->next->next, TYPE_USHORT, "c") && n->next->next->next &&
              is_decl(n->next->next->next, TYPE_LONG, "d") &&
              is_decl(n->next->next->next->next, TYPE_LLONG, "e"),
          "suffix type-specifier combinations resolve");
    node_free(root);

    combos = error_count();
    root = parse_snippet("signed unsigned a;");
    check(error_count() == combos + 1, "signed with unsigned is rejected");
    node_free(root);

    combos = error_count();
    root = parse_snippet("double float a;");
    check(error_count() == combos + 1, "two base types are rejected");
    node_free(root);

    combos = error_count();
    root = parse_snippet("long long long a;");
    check(error_count() == combos + 1, "three long is rejected");
    node_free(root);

    combos = error_count();
    root = parse_snippet("long long double a;");
    check(error_count() == combos + 1, "long long with double is rejected");
    node_free(root);

    combos = error_count();
    root = parse_snippet("long char a;");
    check(error_count() == combos + 1, "long with char is rejected");
    node_free(root);

    combos = error_count();
    root = parse_snippet("long short a;");
    check(error_count() == combos + 1, "long with short is rejected");
    node_free(root);
}

// Typedef names participate in declaration/type-name selection, while an
// ordinary declaration of the same spelling hides them only in its scope.
static void test_typedef_names(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "typedef int T; T value; "
        "int cast_value(T value) { return (T)value; } "
        "int shadow(int T) { return sizeof(T); } "
        "int block(void) { { int T; T = 1; } return (T)1; } "
        "int loop(void) { for (int T = 0; T < 1; T = T + 1) ; "
        "T after; return (T)after; }");
    Node *typedef_decl = root ? root->u.program.external_declarations : NULL;
    Node *value_decl = typedef_decl ? typedef_decl->next : NULL;
    Node *cast_fn = value_decl ? value_decl->next : NULL;
    Node *shadow_fn = cast_fn ? cast_fn->next : NULL;
    Node *block_fn = shadow_fn ? shadow_fn->next : NULL;
    Node *loop_fn = block_fn ? block_fn->next : NULL;
    Node *shadow_expr = first_return_expr(shadow_fn);
    Node *block_stmt = first_statement(block_fn);
    Node *block_return = block_stmt ? block_stmt->next : NULL;
    Node *loop_stmt = first_statement(loop_fn);
    Node *after_decl = loop_stmt ? loop_stmt->next : NULL;

    check(error_count() == before,
          "typedef names parse in declarations, parameters and casts");
    check(typedef_decl && typedef_decl->type == ND_DECL &&
              typedef_decl->u.decl.specifiers->storage == STORAGE_TYPEDEF &&
              value_decl && value_decl->type == ND_DECL &&
              value_decl->u.decl.specifiers->type->kind ==
                  TYPE_SPECIFIER_TYPEDEF_NAME,
          "typedef declarations establish a reusable type specifier");
    check(shadow_expr && shadow_expr->type == ND_UNARY &&
              shadow_expr->u.unary.op == UNARY_SIZEOF,
          "a parameter name hides a typedef in the function body");
    check(block_stmt && block_stmt->type == ND_BLOCK &&
              block_stmt->u.block.items &&
              block_stmt->u.block.items->next &&
              block_stmt->u.block.items->next->type == ND_EXPR_STMT &&
              block_return && block_return->type == ND_RETURN &&
              block_return->u.expr.expr &&
              block_return->u.expr.expr->type == ND_CAST,
          "block shadowing ends when the block scope closes");
    check(loop_stmt && loop_stmt->type == ND_FOR && after_decl &&
              after_decl->type == ND_DECL &&
              after_decl->u.decl.specifiers->type->kind ==
                  TYPE_SPECIFIER_TYPEDEF_NAME,
          "for-init declarations remain confined to the loop scope");
    node_free(root);

    before = error_count();
    root = parse_snippet("typedef int Invalid = 1;");
    check(error_count() == before + 1,
          "typedef declarators reject initializers");
    node_free(root);
}

// Cover storage-class representation, every implemented declaration context,
// combination constraints, and recovery that preserves later declarators.
static void test_storage_classes(void)
{
    int before = error_count();
    Node *root = parse_file("tests/tests/valid/storage_class.c");
    Node *file_value = root ? root->u.program.external_declarations : NULL;
    Node *external_value = file_value ? file_value->next : NULL;
    Node *accumulate = external_value ? external_value->next : NULL;
    Node *main_fn = accumulate ? accumulate->next : NULL;
    const Declarator *accumulate_decl =
        accumulate && accumulate->type == ND_FUNCTION
            ? declarator_function(accumulate->u.func.declarator)
            : NULL;
    Node *param = accumulate_decl
                      ? accumulate_decl->u.function.clause.prototype.parameters
                      : NULL;
    Node *local = accumulate ? first_statement(accumulate) : NULL;
    Node *result = local ? local->next : NULL;
    Node *loop = result ? result->next : NULL;

    check(error_count() == before, "all four storage classes parse in valid contexts");
    check(file_value && file_value->type == ND_DECL &&
              node_declaration_specifiers(file_value)->storage == STORAGE_STATIC &&
              external_value &&
              external_value->type == ND_DECL &&
              node_declaration_specifiers(external_value)->storage == STORAGE_EXTERN,
          "file-scope declarations retain static and extern");
    check(accumulate && accumulate->type == ND_FUNCTION &&
              node_declaration_specifiers(accumulate)->storage == STORAGE_STATIC &&
              param && node_declaration_specifiers(param)->storage == STORAGE_REGISTER,
          "function and parameter declarations retain their storage classes");
    check(local && local->type == ND_DECL &&
              node_declaration_specifiers(local)->storage == STORAGE_AUTO && result &&
              result->type == ND_DECL &&
              node_declaration_specifiers(result)->storage == STORAGE_REGISTER &&
              loop && loop->type == ND_FOR && loop->u.for_stmt.init &&
              node_declaration_specifiers(loop->u.for_stmt.init)->storage == STORAGE_AUTO &&
              main_fn && main_fn->type == ND_FUNCTION &&
              node_declaration_specifiers(main_fn)->storage == STORAGE_NONE,
          "local, for-init and implicit storage classes remain distinct");
    node_free(root);

    static const struct {
        const char *source;
        const char *message;
    } invalid[] = {
        { "static extern int value;", "multiple storage classes are rejected" },
        { "auto int value;", "auto is rejected in an external declaration" },
        { "register int value;", "register is rejected in an external declaration" },
        { "int f(extern int value) { return value; }",
          "extern is rejected in a parameter declaration" },
        { "auto int f(void) { return 0; }", "auto is rejected on a function definition" },
        { "int main(void) { static int f(void); return 0; }",
          "static is rejected on a block-scope function declaration" },
        { "int main(void) { for (extern int i = 0; i < 1; i = i + 1) ; return 0; }",
          "extern is rejected in a for-init declaration" },
    };

    for (size_t i = 0; i < ARRAY_LEN(invalid); i++) {
        before = error_count();
        root = parse_snippet(invalid[i].source);
        check(error_count() > before, invalid[i].message);
        node_free(root);
    }
}

// Control flow: labels/goto, selections, loops, break and continue.
static void test_control_flow(void)
{
    int before = error_count();

    Node *root = parse_snippet("int main(void) { if (1) return 0; else return 1; }");
    Node *stmt = first_statement(first_function(root));
    check(error_count() == before, "if/else parses");
    check(stmt && stmt->type == ND_IF && stmt->u.if_stmt.cond && stmt->u.if_stmt.then_stmt &&
              stmt->u.if_stmt.else_stmt,
          "if keeps both branches");
    node_free(root);

    root = parse_snippet("int main(void) { if (1) return 0; return 2; }");
    stmt = first_statement(first_function(root));
    check(stmt && stmt->type == ND_IF && stmt->u.if_stmt.else_stmt == NULL,
          "if without else keeps a NULL else branch");
    node_free(root);

    root = parse_snippet("int main(void) { goto done; done: return 0; }");
    stmt = first_statement(first_function(root));
    check(error_count() == before, "goto and a named label parse");
    check(stmt && stmt->type == ND_GOTO &&
              string_view_equal_cstr(stmt->u.goto_stmt.name, "done") &&
              stmt->next && stmt->next->type == ND_LABEL &&
              string_view_equal_cstr(stmt->next->u.label.name, "done") &&
              stmt->next->u.label.stmt && stmt->next->u.label.stmt->type == ND_RETURN,
          "goto and label retain the target name and labeled statement");
    node_free(root);

    root = parse_file("tests/tests/valid/switch_statement.c");
    stmt = first_statement(first_function(root));
    Node *switch_body = stmt && stmt->type == ND_SWITCH ? stmt->u.switch_stmt.body : NULL;
    Node *first_case = switch_body && switch_body->type == ND_BLOCK
                           ? switch_body->u.block.items
                           : NULL;
    Node *second_case = first_case && first_case->type == ND_CASE
                            ? first_case->u.case_stmt.stmt
                            : NULL;
    check(error_count() == before, "switch with case and default labels parses");
    check(stmt && stmt->type == ND_SWITCH &&
              is_identifier(stmt->u.switch_stmt.cond, "value"),
          "switch retains its controlling expression");
    check(first_case && first_case->type == ND_CASE &&
              is_num(first_case->u.case_stmt.expr, 0),
          "first case retains its constant expression");
    check(second_case && second_case->type == ND_CASE,
          "consecutive case labels remain nested statements");
    check(second_case && second_case->type == ND_CASE &&
              is_num(second_case->u.case_stmt.expr, 1),
          "nested case retains its constant expression");
    check(first_case && first_case->next && first_case->next->type == ND_DEFAULT,
          "default remains the next statement in the switch body");
    node_free(root);

    root = parse_snippet("int main(void) { int i = 0; while (i < 3) i = i + 1; return i; }");
    stmt = first_statement(first_function(root))->next;
    check(stmt && stmt->type == ND_WHILE && stmt->u.while_stmt.cond && stmt->u.while_stmt.body,
          "while parses");
    node_free(root);

    root = parse_snippet("int main(void) { int i = 0; do i = i + 1; while (i < 3); return i; }");
    stmt = first_statement(first_function(root))->next;
    check(stmt && stmt->type == ND_DO_WHILE && stmt->u.do_while.body && stmt->u.do_while.cond,
          "do-while parses");
    node_free(root);

    root = parse_snippet("int main(void) { int i; for (i = 0; i < 3; i = i + 1) ; return i; }");
    stmt = first_statement(first_function(root))->next;
    check(stmt && stmt->type == ND_FOR && stmt->u.for_stmt.init && stmt->u.for_stmt.cond &&
              stmt->u.for_stmt.post && stmt->u.for_stmt.body,
          "for parses");
    node_free(root);

    root = parse_snippet(
        "int main(void) { for (int j = 0; j < 3; j = j + 1) { if (j) break; else continue; } "
        "return 0; }");
    stmt = first_statement(first_function(root));
    Node *init = stmt ? stmt->u.for_stmt.init : NULL;
    Node *body = stmt ? stmt->u.for_stmt.body : NULL;
    Node *branch = body && body->type == ND_BLOCK ? body->u.block.items : NULL;
    check(stmt && stmt->type == ND_FOR && init && init->type == ND_DECL && branch &&
              branch->type == ND_IF && branch->u.if_stmt.then_stmt->type == ND_BREAK &&
              branch->u.if_stmt.else_stmt->type == ND_CONTINUE,
          "for with declaration init and break/continue parses");
    node_free(root);
}

// Distinguish pointer binding across direct-declarator parentheses and retain
// the variadic marker on the function layer that consumes the ellipsis.
static void test_recursive_declarators(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "const int *const volatile object; int *factory(void) { return 0; } "
        "int (*callback)(int value, ...);");
    Node *object = root ? root->u.program.external_declarations : NULL;
    Node *factory = object ? object->next : NULL;
    Node *callback = factory ? factory->next : NULL;

    Declarator *object_decl = node_declarator(object);
    check(object && object->type == ND_DECL &&
              (node_declaration_specifiers(object)->qualifiers & TYPE_QUAL_CONST) &&
              object_decl &&
              object_decl->kind == DECLARATOR_POINTER && object_decl->inner &&
              object_decl->inner->kind == DECLARATOR_IDENT &&
              object_decl->u.pointer.qualifiers ==
                  (TYPE_QUAL_CONST | TYPE_QUAL_VOLATILE),
          "pointer declarator wraps its identifier");

    Declarator *factory_decl = factory ? factory->u.func.declarator : NULL;
    check(factory && factory->type == ND_FUNCTION && factory_decl &&
              factory_decl->kind == DECLARATOR_POINTER && factory_decl->inner &&
              factory_decl->inner->kind == DECLARATOR_FUNCTION && factory->u.func.body,
          "function returning pointer preserves declarator binding");

    Declarator *callback_decl = node_declarator(callback);
    check(callback && callback->type == ND_DECL && callback_decl &&
              callback_decl->kind == DECLARATOR_FUNCTION &&
              callback_decl->u.function.clause.prototype.variadic &&
              callback_decl->inner &&
              callback_decl->inner->kind == DECLARATOR_POINTER,
          "parenthesized variadic function pointer preserves declarator binding");
    check(error_count() == before, "pointer and variadic declarators parse without errors");
    node_free(root);
}

// Type-name consumers must retain the abstract declarator rather than
// confusing parenthesized types with ordinary expressions.
static void test_type_name_expressions(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "int cast_value(void) { return (const int *)0; } "
        "int size_value(void) { return sizeof(int *); } "
        "int align_value(void) { return _Alignof(int (*)(void)); } "
        "int nested_value(void) { return _Alignof(int *(*)(void)); }");
    Node *cast_fn = root ? root->u.program.external_declarations : NULL;
    Node *size_fn = cast_fn ? cast_fn->next : NULL;
    Node *align_fn = size_fn ? size_fn->next : NULL;
    Node *nested_fn = align_fn ? align_fn->next : NULL;
    Node *cast = first_return_expr(cast_fn);
    Node *size = first_return_expr(size_fn);
    Node *align = first_return_expr(align_fn);
    Node *nested = first_return_expr(nested_fn);

    check(cast && cast->type == ND_CAST && cast->u.cast.type_name &&
              (cast->u.cast.type_name->qualifiers & TYPE_QUAL_CONST) &&
              cast->u.cast.expr &&
              string_view_begin(cast->u.cast.type_name->span.text)[0] == '(' &&
              string_view_end(cast->u.cast.type_name->span.text)[-1] == ')',
          "cast retains its type-name and operand");
    check(size && size->type == ND_SIZEOF_TYPE && size->u.type_query.type_name &&
              size->u.type_query.type_name->declarator &&
              size->u.type_query.type_name->declarator->kind == DECLARATOR_POINTER,
          "sizeof(type) retains an abstract pointer declarator");
    check(align && align->type == ND_ALIGNOF_TYPE && align->u.type_query.type_name,
          "_Alignof(type) parses a parenthesized abstract declarator");
    Declarator *nested_decl =
        nested && nested->type == ND_ALIGNOF_TYPE && nested->u.type_query.type_name
            ? nested->u.type_query.type_name->declarator
            : NULL;
    check(nested_decl && nested_decl->kind == DECLARATOR_POINTER && nested_decl->inner &&
              nested_decl->inner->kind == DECLARATOR_FUNCTION &&
              nested_decl->inner->inner &&
              nested_decl->inner->inner->kind == DECLARATOR_POINTER,
          "parenthesized abstract declarator preserves its outer pointer prefix");
    check(error_count() == before, "type-name expression forms parse without errors");
    node_free(root);
}

// Declaration grammar must retain derived parameter syntax, typedef-shadowing
// member names, parenthesized abstract arrays and exact production extents.
static void test_declaration_regressions(void)
{
    int before = error_count();
    Node *root = parse_snippet("int f(void *);");
    Node *function = first_function(root);
    Node *parameter = node_function_params(function);
    check(error_count() == before && parameter && !parameter->next &&
              node_declarator(parameter) &&
              node_declarator(parameter)->kind == DECLARATOR_POINTER,
          "unnamed void pointer remains a parameter");
    node_free(root);

    before = error_count();
    root = parse_snippet("int f(int, void (*)(void));");
    function = first_function(root);
    parameter = node_function_params(function);
    check(error_count() == before && parameter && parameter->next &&
              node_declarator(parameter->next),
          "unnamed derived void parameter is not a lone void parameter");
    node_free(root);

    before = error_count();
    root = parse_snippet("typedef int T; struct S { int T; };");
    Node *aggregate = root ? root->u.program.external_declarations : NULL;
    aggregate = aggregate ? aggregate->next : NULL;
    TypeSpecifier *type = aggregate && aggregate->type == ND_DECL
                              ? aggregate->u.decl.specifiers->type
                              : NULL;
    Node *member = type && type->kind == TYPE_SPECIFIER_STRUCT
                       ? type->u.aggregate.members
                       : NULL;
    Node *member_declarator = member && member->type == ND_STRUCT_DECL
                                  ? member->u.struct_decl.declarators
                                  : NULL;
    check(error_count() == before && member_declarator &&
              declarator_is_named(member_declarator->u.struct_declarator.declarator,
                                  "T"),
          "typedef spelling can be a member declarator after another type");
    node_free(root);

    before = error_count();
    root = parse_snippet(
        "int probe(void) { return sizeof(int ([3])); }");
    Node *query = first_return_expr(first_function(root));
    Declarator *abstract = query && query->type == ND_SIZEOF_TYPE
                               ? query->u.type_query.type_name->declarator
                               : NULL;
    check(error_count() == before && abstract &&
              abstract->kind == DECLARATOR_ARRAY,
          "parenthesized abstract array declarator parses");
    node_free(root);

    const char *source = "int f(int a[3], void *); int x = 1, y;";
    before = error_count();
    root = parse_snippet(source);
    function = first_function(root);
    parameter = node_function_params(function);
    Node *declaration = function ? function->next : NULL;
    Node *x = declaration && declaration->type == ND_DECL
                  ? declaration->u.decl.declarators
                  : NULL;
    Node *y = x ? x->next : NULL;
    check(error_count() == before && parameter && parameter->next &&
              string_view_length(parameter->span.text) == 8 &&
              string_view_length(parameter->next->span.text) == 6,
          "parameter spans include complete derived declarators");
    check(x && y && string_view_length(x->span.text) == 5 &&
              string_view_length(y->span.text) == 1,
          "init-declarator spans exclude list separators");
    node_free(root);

    before = error_count();
    root = parse_snippet("struct Bad { ; }; int retained;");
    check(error_count() > before,
          "invalid struct member emits a parser diagnostic");
    node_free(root);

    before = error_count();
    root = parse_snippet(
        "int f(void) int x; { return 0; } int retained;");
    function = first_function(root);
    Node *retained = function ? function->next : NULL;
    check(error_count() > before && function &&
              function->u.func.declarations && retained &&
              retained->type == ND_DECL &&
              declarator_is_named(node_declarator(retained), "retained"),
          "prototype definition retains and diagnoses declaration-list");
    node_free(root);
}

// Structured type specifiers retain their tag and body syntax independently
// of later tag resolution, layout and type checking.
static void test_structured_type_specifiers(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "struct Pair { int x; int y; }; "
        "union Value { int i; long l; }; "
        "enum Color { RED, GREEN = 2, BLUE, }; "
        "struct Bits { unsigned value : 3; _Static_assert(1, \"ok\"); }; ");
    Node *pair = root ? root->u.program.external_declarations : NULL;
    Node *value = pair ? pair->next : NULL;
    Node *color = value ? value->next : NULL;
    Node *bits = color ? color->next : NULL;

    check(error_count() == before, "structured type specifiers parse without errors");
    check(pair && pair->type == ND_DECL && pair->u.decl.specifiers->type->kind ==
                                                TYPE_SPECIFIER_STRUCT &&
              pair->u.decl.specifiers->type->u.aggregate.has_body &&
              pair->u.decl.specifiers->type->u.aggregate.members,
          "struct definition retains its member declarations");
    check(value && value->u.decl.specifiers->type->kind == TYPE_SPECIFIER_UNION &&
              value->u.decl.specifiers->type->u.aggregate.members,
          "union definition retains its member declarations");
    check(color && color->u.decl.specifiers->type->kind == TYPE_SPECIFIER_ENUM &&
              color->u.decl.specifiers->type->u.enumeration.enumerators,
          "enum definition retains enumerators and trailing comma");
    check(bits && bits->u.decl.specifiers->type->u.aggregate.members,
          "struct body retains bit-fields and static assertions");
    node_free(root);

    before = error_count();
    root = parse_snippet(
        "struct Pair { int value; }; "
        "struct Pair make(void) { struct Pair result; return result; }");
    Node *make = root && root->u.program.external_declarations
                     ? root->u.program.external_declarations->next
                     : NULL;
    check(error_count() == before && make && make->type == ND_FUNCTION &&
              make->u.func.specifiers->type->kind == TYPE_SPECIFIER_STRUCT,
          "structured type specifiers select function-definition correctly");
    node_free(root);

    before = error_count();
    root = parse_snippet("enum Empty {};");
    check(error_count() == before + 1,
          "an empty C11 enumerator-list is rejected with a diagnostic");
    node_free(root);

    before = error_count();
    root = parse_snippet(
        "struct Outer { struct { int x; }; union { int y; }; };");
    Node *outer = root ? root->u.program.external_declarations : NULL;
    TypeSpecifier *outer_type = outer && outer->type == ND_DECL
                                    ? outer->u.decl.specifiers->type
                                    : NULL;
    Node *anonymous_struct =
        outer_type && outer_type->kind == TYPE_SPECIFIER_STRUCT
            ? outer_type->u.aggregate.members
            : NULL;
    Node *anonymous_union = anonymous_struct ? anonymous_struct->next : NULL;
    check(error_count() == before && anonymous_struct &&
              anonymous_struct->type == ND_STRUCT_DECL && anonymous_union &&
              anonymous_union->type == ND_STRUCT_DECL &&
              type_specifier_is_anonymous_aggregate(
                  anonymous_struct->u.struct_decl.specifiers->type) &&
              type_specifier_is_anonymous_aggregate(
                  anonymous_union->u.struct_decl.specifiers->type),
          "anonymous struct and union members need no declarator");
    node_free(root);

    before = error_count();
    root = parse_snippet(
        "struct Outer { int; union Tagged { int x; }; int retained; }; "
        "int after;");
    outer = root ? root->u.program.external_declarations : NULL;
    outer_type = outer && outer->type == ND_DECL
                     ? outer->u.decl.specifiers->type
                     : NULL;
    Node *missing_builtin =
        outer_type && outer_type->kind == TYPE_SPECIFIER_STRUCT
            ? outer_type->u.aggregate.members
            : NULL;
    Node *missing_tagged = missing_builtin ? missing_builtin->next : NULL;
    Node *retained_member = missing_tagged ? missing_tagged->next : NULL;
    Node *after = outer ? outer->next : NULL;
    check(error_count() == before + 2,
          "member declarations without declarators are diagnosed");
    check(missing_builtin && missing_builtin->type == ND_STRUCT_DECL &&
              !missing_builtin->u.struct_decl.declarators && missing_tagged &&
              missing_tagged->type == ND_STRUCT_DECL &&
              !missing_tagged->u.struct_decl.declarators && retained_member &&
              retained_member->type == ND_STRUCT_DECL &&
              retained_member->u.struct_decl.declarators &&
              after && declarator_is_named(node_declarator(after), "after"),
          "invalid empty members retain following member and declaration");
    node_free(root);
}

// Removing any remaining C11 declaration-specifier leaf must make this valid
// translation unit report a syntax error. Later assertions inspect the typed
// AST once the complete syntax is accepted.
static void test_remaining_declaration_specifiers(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "_Thread_local int tls; "
        "static _Thread_local int static_tls; "
        "extern _Thread_local int extern_tls; "
        "float _Complex fc; double _Complex dc; long double _Complex ldc; "
        "_Atomic(int) atomic_value; _Atomic int qualified_value; "
        "inline int inlined(void) { return 0; } "
        "_Noreturn void stop(void); "
        "_Alignas(16) int aligned_value; "
        "_Alignas(long double) int typed_alignment; "
        "_Alignas(8) _Alignas(16) int repeated_alignment; "
        "int local_tls(void) { static _Thread_local int x; "
        "extern _Thread_local int y; return 0; }");

    check(error_count() == before,
          "remaining C11 declaration specifiers parse without errors");
    Node *item = root ? root->u.program.external_declarations : NULL;
    check(item && item->u.decl.specifiers->storage == STORAGE_THREAD_LOCAL,
          "_Thread_local remains independent of the base storage class");
    check(item->next &&
              item->next->u.decl.specifiers->storage ==
                  (STORAGE_STATIC | STORAGE_THREAD_LOCAL) &&
              item->next->next &&
              item->next->next->u.decl.specifiers->storage ==
                  (STORAGE_EXTERN | STORAGE_THREAD_LOCAL),
          "_Thread_local combines with static and extern in one flag set");
    for (int i = 0; item && i < 3; i++)
        item = item->next;
    check(item && declaration_specifiers_builtin_type(item->u.decl.specifiers) ==
                      TYPE_FLOAT_COMPLEX &&
              item->next &&
              declaration_specifiers_builtin_type(
                  item->next->u.decl.specifiers) ==
                  TYPE_DOUBLE_COMPLEX &&
              item->next->next &&
              declaration_specifiers_builtin_type(
                  item->next->next->u.decl.specifiers) ==
                  TYPE_LDOUBLE_COMPLEX,
          "complex specifiers retain their resolved builtin variants");
    Node *atomic = item && item->next ? item->next->next->next : NULL;
    Node *inline_definition = atomic && atomic->next
                                  ? atomic->next->next
                                  : NULL;
    Node *aligned = inline_definition && inline_definition->next
                        ? inline_definition->next->next
                        : NULL;
    check(atomic && atomic->u.decl.specifiers->type->kind ==
                        TYPE_SPECIFIER_ATOMIC &&
              atomic->u.decl.specifiers->type->u.atomic_type,
          "atomic type specifier owns its type-name");
    check(inline_definition && inline_definition->type == ND_FUNCTION &&
              (inline_definition->u.func.specifiers->function_specifiers &
               FUNCTION_SPEC_INLINE),
          "function specifier is retained on the function definition");
    check(aligned && aligned->u.decl.specifiers->alignments &&
              aligned->u.decl.specifiers->alignments->kind ==
                  ALIGNMENT_EXPRESSION,
          "alignment specifier owns its constant-expression");
    node_free(root);
}

// A type-specifier dispatcher that speculatively consumes `_Atomic` rejects its
// valid type-qualifier use in specifier-qualifier-list.
static void test_atomic_qualifier_dispatch(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "struct Item { _Atomic int value; }; "
        "int size(void) { return sizeof(_Atomic int); }");
    Node *item = root ? root->u.program.external_declarations : NULL;

    check(error_count() == before && item && item->type == ND_DECL &&
              item->next && item->next->type == ND_FUNCTION,
          "_Atomic qualifier remains selectable in specifier-qualifier-list");
    node_free(root);
}

// Compound literals remain postfix-expressions and generic selections remain
// primary-expressions: member, subscript and call suffixes must bind to them.
static void test_remaining_primary_and_postfix_expressions(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "struct Pair { int value; }; "
        "int identity(int value) { return value; } "
        "int fallback(int value) { return value + 1; } "
        "int use(int value) { "
        "return ((struct Pair){ .value = value }).value + "
        "(int[]){ 1, 2 }[1] + "
        "_Generic(value, int: identity, default: fallback)(value); }");

    check(error_count() == before,
          "compound literals and generic selections accept postfix suffixes");
    Node *use = root && root->u.program.external_declarations
                    ? root->u.program.external_declarations->next->next->next
                    : NULL;
    Node *expression = first_return_expr(use);
    Node *generic_call = expression && expression->type == ND_BINARY
                             ? expression->u.binary.rhs
                             : NULL;
    check(generic_call && generic_call->type == ND_CALL &&
              generic_call->u.call.callee->type == ND_GENERIC_SELECTION,
          "generic selection remains the base of its postfix call");
    Node *left = expression && expression->type == ND_BINARY
                     ? expression->u.binary.lhs
                     : NULL;
    Node *member = left && left->type == ND_BINARY ? left->u.binary.lhs : NULL;
    check(member && member->type == ND_MEMBER &&
              member->u.member_access.base->type == ND_COMPOUND_LITERAL,
          "compound literal remains the base of its postfix member access");
    node_free(root);
}

// Removing parser-side lvalue classification must leave the syntactically valid
// compound-literal assignment for semantic analysis.
static void test_assignment_semantic_boundary(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "int assign(void) { (int){ 0 } = 1; return 0; }");
    Node *statement = first_statement(first_function(root));

    check(error_count() == before && statement &&
              statement->type == ND_EXPR_STMT && statement->u.expr.expr &&
              statement->u.expr.expr->type == ND_ASSIGN &&
              statement->u.expr.expr->u.assign.lhs->type == ND_COMPOUND_LITERAL,
          "compound-literal assignment reaches semantic analysis");
    node_free(root);
}

// Parser-level declaration and generic constraints must be diagnosed only
// after their complete productions have been selected and constructed.
static void test_remaining_specifier_constraints(void)
{
    static const char *invalid_declarations[] = {
        "_Thread_local int function(void);",
        "void function(void) { _Thread_local int local; }",
        "typedef _Alignas(16) int Aligned;",
        "_Alignas(16) int function(void);",
        "inline int object;",
        "int function(_Alignas(8) int parameter);",
    };
    for (size_t i = 0; i < ARRAY_LEN(invalid_declarations); i++) {
        int before = error_count();
        Node *root = parse_snippet(invalid_declarations[i]);
        check(error_count() > before,
              "invalid C11 declaration specifier constraint is rejected");
        node_free(root);
    }

    int before = error_count();
    Node *root = parse_snippet(
        "int value; int use(void) { return _Generic(value, default: 1, "
        "default: 2); }");
    check(error_count() > before,
          "generic selection rejects more than one default association");
    node_free(root);

    static const char *malformed_features[] = {
        "struct Item { int value; }; int use(void) { "
        "return (struct Item){ .value = }; }",
        "int value; int use(void) { return _Generic(value, int:); }",
        "struct Item { int value; }; struct Item _Atomic(;",
        "struct Item { int value; }; struct Item _Alignas(;",
    };
    for (size_t i = 0; i < ARRAY_LEN(malformed_features); i++) {
        before = error_count();
        root = parse_snippet(malformed_features[i]);
        check(error_count() > before,
              "malformed remaining C11 feature is rejected");
        node_free(root);
    }
}

// Array suffixes retain their bound expression and prototype-only static/VLA
// markers on the exact recursive declarator layer that introduced them.
static void test_array_declarators(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "int values[3]; int matrix[2][4]; "
        "int consume(int a[const static 4], int b[*]);");
    Node *values = root ? root->u.program.external_declarations : NULL;
    Node *matrix = values ? values->next : NULL;
    Node *consume_fn = matrix ? matrix->next : NULL;
    const Declarator *consume_decl = node_function_declarator(consume_fn);
    Node *a = consume_decl
                  ? consume_decl->u.function.clause.prototype.parameters
                  : NULL;
    Node *b = a ? a->next : NULL;

    Declarator *values_decl = node_declarator(values);
    check(values_decl && values_decl->kind == DECLARATOR_ARRAY &&
              values_decl->u.array.size && values_decl->u.array.size->type == ND_NUM &&
              string_view_length(values_decl->u.array.size->span.text) == 1 &&
              string_view_begin(values_decl->u.array.size->span.text)[0] == '3' &&
              values_decl->inner &&
              values_decl->inner->kind == DECLARATOR_IDENT,
          "array declarator retains its bound");
    Declarator *matrix_decl = node_declarator(matrix);
    check(matrix_decl && matrix_decl->kind == DECLARATOR_ARRAY &&
              matrix_decl->u.array.size &&
              string_view_begin(matrix_decl->u.array.size->span.text)[0] == '4' &&
              matrix_decl->inner &&
              matrix_decl->inner->kind == DECLARATOR_ARRAY &&
              matrix_decl->inner->u.array.size &&
              string_view_begin(matrix_decl->inner->u.array.size->span.text)[0] == '2',
          "multidimensional array retains suffix order");
    check(a && node_declarator(a) &&
              node_declarator(a)->kind == DECLARATOR_ARRAY &&
              node_declarator(a)->u.array.form == ARRAY_DECLARATOR_STATIC &&
              node_declarator(a)->u.array.qualifiers == TYPE_QUAL_CONST && b &&
              node_declarator(b) &&
              node_declarator(b)->u.array.form == ARRAY_DECLARATOR_STAR,
          "array parameters retain static and star bounds");
    check(error_count() == before, "array declarators parse without errors");
    node_free(root);

    root = parse_snippet("int invalid[static];");
    check(error_count() > before, "static array declarator requires a bound");
    node_free(root);

    int valid_errors = error_count();
    root = parse_snippet("int valid(int values[static 3]) { return 0; }");
    check(error_count() == valid_errors,
          "static array parameter remains valid in a function definition");
    node_free(root);

    static const char *invalid[] = {
        "int values[static 3];",
        "int values[const 3];",
        "int values[*];",
        "int consume(int values[const static volatile 3]);",
        "int consume(int values[*]) { return 0; }",
    };
    for (size_t i = 0; i < ARRAY_LEN(invalid); i++) {
        int errors = error_count();
        root = parse_snippet(invalid[i]);
        check(error_count() > errors, "invalid array declarator context is rejected");
        node_free(root);
    }
}

// Braced initializers retain item order, while each designation owns its
// complete designator chain and the initializer selected by that chain.
static void test_initializer_lists(void)
{
    check(parser_test_initializer_list_leaves_trailing_comma(),
          "initializer-list leaves its parent's trailing comma");

    int before = error_count();
    Node *root = parse_snippet(
        "int values[3] = {1, 2, 3}; "
        "int sparse[4] = {[2] = 7, [0] = 1}; "
        "int member = {.field = 5}; "
        "int trailing[1] = {1,};");
    Node *values = root ? root->u.program.external_declarations : NULL;
    Node *sparse = values ? values->next : NULL;
    Node *member = sparse ? sparse->next : NULL;

    Node *values_init = node_initializer(values);
    check(values_init && values_init->type == ND_INIT_LIST &&
              values_init->u.initializer_list.initializers &&
              values_init->u.initializer_list.initializers->next &&
              values_init->u.initializer_list.initializers->next->next,
          "braced initializer retains item order");
    Node *sparse_item = sparse && node_initializer(sparse)
                            ? node_initializer(sparse)
                                  ->u.initializer_list.initializers
                            : NULL;
    check(sparse_item && sparse_item->type == ND_DESIGNATED_INIT &&
              sparse_item->u.designated.designators &&
              sparse_item->u.designated.designators->type == ND_INDEX_DESIGNATOR &&
              sparse_item->u.designated.value,
          "index designator binds its initializer");
    Node *member_item = member && node_initializer(member)
                            ? node_initializer(member)
                                  ->u.initializer_list.initializers
                            : NULL;
    check(member_item && member_item->type == ND_DESIGNATED_INIT &&
              member_item->u.designated.designators &&
              member_item->u.designated.designators->type == ND_MEMBER_DESIGNATOR &&
              string_view_equal_cstr(
                  member_item->u.designated.designators->u.member.name,
                  "field"),
          "member designator retains its identifier");
    Node *trailing = member ? member->next : NULL;
    check(trailing && node_initializer(trailing) &&
              node_initializer(trailing)->type == ND_INIT_LIST,
          "initializer owns and consumes its optional trailing comma");
    check(error_count() == before, "initializer lists and designators parse without errors");
    node_free(root);
}

// Function calls, parameters, prototypes and the rejected forms.
static void test_functions(void)
{
    int before = error_count();

    Node *root =
        parse_snippet("int add(int a, int b) { return a + b; } int main(void) { return add(1, 2); }");
    Node *fn = first_function(root);
    Node *params = node_function_params(fn);
    check(error_count() == before, "parameters and calls parse");
    check(fn && fn->type == ND_FUNCTION && params && params->next &&
              is_decl(params, TYPE_INT, "a") &&
              is_decl(params->next, TYPE_INT, "b"),
          "parameter list keeps name and type");
    Node *call_stmt = fn ? first_statement(fn->next) : NULL;
    Node *call = call_stmt ? call_stmt->u.expr.expr : NULL;
    check(call && call->type == ND_CALL &&
              is_identifier(call->u.call.callee, "add") &&
              call->u.call.args && call->u.call.args->type == ND_NUM &&
              call->u.call.args->next && call->u.call.args->next->type == ND_NUM,
          "call links callee and arguments");
    node_free(root);

    int errs = error_count();
    root = parse_snippet("int f(int); int main(void) { return f(1); }");
    fn = first_function(root);
    params = node_function_params(fn);
    check(error_count() == errs, "prototype parses");
    check(fn && fn->type == ND_DECL && params && node_initializer(fn) == NULL,
          "prototype remains a declaration rather than a function definition");
    check(fn && fn->next && fn->next->type == ND_FUNCTION && fn->next->u.func.body,
          "definition after a prototype still parses");
    node_free(root);

    errs = error_count();
    root = parse_snippet("int main(void) { return f(1, 2, 3) + g(); }");
    check(error_count() == errs, "argument lists accept several and zero arguments");
    node_free(root);

    int errors = error_count();
    root = parse_snippet("int main(void) { 1(); return 0; }");
    check(error_count() == errors, "calling a non-variable parses");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int foo(int value)(void);");
    check(error_count() > errors, "function returning a function is rejected");
    Node *invalid_return = root ? root->u.program.external_declarations : NULL;
    check(invalid_return && invalid_return->type == ND_DECL &&
              node_declarator(invalid_return),
          "function return-type constraint keeps the complete declaration AST");
    node_free(root);

    static const char *invalid_declarators[] = {
        "int returns_array(void)[3];",
        "int array_of_functions[3](void);",
        "int main(void) { return sizeof(int ()()); }",
        "int main(void) { return sizeof(int ()[3]); }",
        "int main(void) { return sizeof(int (name)); }",
        "struct S { int returns_array(void)[3]; };",
        "struct S { int array_of_functions[3](void); };",
    };
    for (size_t i = 0; i < ARRAY_LEN(invalid_declarators); i++) {
        errors = error_count();
        root = parse_snippet(invalid_declarators[i]);
        check(error_count() > errors, "invalid named or abstract declarator is rejected");
        node_free(root);
    }

    errors = error_count();
    root = parse_snippet("int foo(int value) = 3;");
    check(error_count() > errors, "function initialized like a variable is rejected");
    Node *invalid_function_decl =
        root ? root->u.program.external_declarations : NULL;
    check(invalid_function_decl && invalid_function_decl->type == ND_DECL &&
              node_initializer(invalid_function_decl),
          "function initializer constraint keeps the complete declaration AST");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int (value) { return 0; }");
    check(error_count() > errors, "parenthesized object cannot define a function body");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int f(int a,) { return a; }");
    check(error_count() > errors, "trailing comma in a parameter list is rejected");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int main(void) { return f(1,); }");
    check(error_count() > errors, "trailing comma in a call is rejected");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int f(void); int g(int a, void);");
    check(error_count() > errors, "void outside a lone (void) parameter list is rejected");
    node_free(root);

    errs = error_count();
    root = parse_snippet("void f(void callback(int x)); int g(int h(int a));");
    Node *fp = first_function(root);
    Node *callback = node_function_params(fp);
    const Declarator *callback_function = node_function_declarator(callback);
    check(error_count() == errs, "function type parameters parse");
    check(callback && callback->type == ND_PARAMETER && callback_function &&
              callback_function->u.function.clause.prototype.parameters &&
              is_decl(callback_function->u.function.clause.prototype.parameters,
                      TYPE_INT, "x"),
          "function parameter keeps its declarator shape");
    node_free(root);

    errs = error_count();
    root = parse_snippet("int f(unsigned long, int);");
    fp = first_function(root);
    Node *unnamed = node_function_params(fp);
    check(error_count() == errs && unnamed && unnamed->type == ND_PARAMETER &&
              declarator_name(node_declarator(unnamed)) == NULL &&
              string_view_length(unnamed->span.text) == 13,
          "unnamed parameter span covers the whole specifier list");
    node_free(root);

    errors = error_count();
    root = parse_snippet("void foo(void x);");
    check(error_count() == errors,
          "a named void parameter remains valid in a prototype");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int f(void x) { return 0; }");
    check(error_count() > errors,
          "a named void parameter is rejected in a definition");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int f(void) { return 0; } int g(void); int main(void) { return 0; }");
    check(error_count() == errors, "(void) parameter lists stay accepted");
    node_free(root);

    errs = error_count();
    root = parse_snippet("int f() { return 0; } int g(void) { return 0; }");
    fn = first_function(root);
    check(error_count() == errs, "old-style and (void) definitions parse");
    check(fn && node_function_declarator(fn)->u.function.form ==
                    FUNCTION_PARAMS_IDENTIFIER_LIST,
          "empty identifier-list records its parameter form");
    check(fn && fn->next && node_function_declarator(fn->next)->u.function.form ==
                               FUNCTION_PARAMS_PROTOTYPE,
          "(void) records prototype parameter form");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int f() int missing; { return 0; }");
    check(error_count() > errors,
          "old-style declaration-list cannot declare an absent parameter");
    node_free(root);

    errors = error_count();
    root = parse_snippet(
        "int add(left, right) int left; int right; { return left + right; }");
    fn = first_function(root);
    const Declarator *old_style_function =
        fn ? node_function_declarator(fn) : NULL;
    Node *identifiers = old_style_function
                            ? old_style_function->u.function.clause.old_style.identifiers
                            : NULL;
    check(fn && fn->u.func.declarations &&
              identifiers,
          "old-style declaration-list is owned by function-definition");
    check(identifiers && identifiers->type == ND_IDENTIFIER &&
              identifiers->next && identifiers->next->type == ND_IDENTIFIER,
          "old-style identifier-list retains identifier syntax nodes");
    params = node_function_params(fn);
    check(error_count() == errors && fn && params && params->next &&
              is_decl(params, TYPE_INT, "left") &&
              is_decl(params->next, TYPE_INT, "right"),
          "old-style identifier-list binds its parameter declarations");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int duplicate(value, value) int value; { return value; }");
    check(error_count() > errors,
          "old-style identifier-list rejects duplicate parameter names");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int f(void) int;");
    check(error_count() > errors, "a malformed prototype tail is rejected");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int main(void) { for (int f(void); ; ) { return 0; } }");
    check(error_count() > errors, "a function declaration in for-init is rejected");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int main(void) { for (int f(void), i; ; ) { return 0; } }");
    check(error_count() == errors + 1,
          "for-init declaration chain rejects only the function declarator");
    node_free(root);
}

static void test_parse_valid(void)
{
    int before = error_count();
    Node *basic = parse_file("tests/tests/basic.c");
    Node *fn = first_function(basic);
    Node *ret = first_statement(fn);
    check(fn && fn->type == ND_FUNCTION &&
              declarator_is_named(fn->u.func.declarator, "main"),
          "parse basic.c: main function");
    Declarator *shape = fn ? fn->u.func.declarator : NULL;
    check(shape && shape->kind == DECLARATOR_FUNCTION && shape->inner &&
              shape->inner->kind == DECLARATOR_IDENT &&
              string_view_equal_cstr(shape->inner->u.ident.name, "main"),
          "simple function retains a recursive declarator shape");
    check(ret && ret->type == ND_RETURN && is_num(ret->u.expr.expr, 0), "parse basic.c: return 0");
    check(error_count() == before, "parse basic.c: no syntax errors");
    node_free(basic);

    Node *p = parse_file(TP_TESTS "/chapter_1/valid/return_2.c");
    check(is_num(first_return_expr(first_function(p)), 2), "parse return_2.c: return 2");
    node_free(p);

    p = parse_file(TP_TESTS "/chapter_2/valid/nested_ops.c");
    Node *expr = first_return_expr(first_function(p));
    check(expr && expr->type == ND_UNARY && expr->u.unary.op == UNARY_BITNOT &&
              expr->u.unary.operand && expr->u.unary.operand->type == ND_UNARY &&
              expr->u.unary.operand->u.unary.op == UNARY_NEG,
          "parse nested_ops.c: ~-3");
    node_free(p);

    p = parse_file(TP_TESTS "/chapter_3/valid/precedence.c");
    expr = first_return_expr(first_function(p));
    check(is_binary(expr, BINARY_ADD) && is_num(expr->u.binary.lhs, 2) &&
              is_binary(expr->u.binary.rhs, BINARY_MUL),
          "parse precedence.c: 2 + 3 * 4");
    node_free(p);

    p = parse_file(TP_TESTS "/chapter_3/valid/extra_credit/bitwise_precedence.c");
    expr = first_return_expr(first_function(p));
    check(is_binary(expr, BINARY_BITOR) && is_binary(expr->u.binary.lhs, BINARY_SHR) &&
              is_binary(expr->u.binary.rhs, BINARY_BITXOR),
          "parse bitwise_precedence.c: >> below |, ^ below &");
    check(error_count() == before, "parse bitwise_precedence.c: #ifdef lines skipped");
    node_free(p);

    p = parse_file(TP_TESTS "/chapter_5/valid/add_variables.c");
    check(error_count() == before, "parse add_variables.c: declarations accepted");
    node_free(p);

    p = parse_file(TP_TESTS "/chapter_5/valid/exp_then_declaration.c");
    check(error_count() == before, "parse exp_then_declaration.c: declaration after statement");
    node_free(p);

    p = parse_file(TP_TESTS "/chapter_5/valid/assignment_in_initializer.c");
    check(error_count() == before, "parse assignment_in_initializer.c");
    node_free(p);

    p = parse_file(TP_TESTS "/chapter_5/valid/use_val_in_own_initializer.c");
    check(error_count() == before, "parse use_val_in_own_initializer.c");
    node_free(p);

    p = parse_file(TP_TESTS "/chapter_5/valid/extra_credit/compound_assignment_chained.c");
    check(error_count() == before, "parse compound_assignment_chained.c");
    node_free(p);
}

// Every broken file must report at least one syntax error; the node tree is
// allowed to be partial.
static void test_parse_invalid(void)
{
    static const char *const cases[] = {
        "/chapter_1/invalid_parse/no_semicolon.c",
        "/chapter_2/invalid_parse/unclosed_paren.c",
        "/chapter_3/invalid_parse/missing_second_op.c",
        "/chapter_3/invalid_parse/extra_credit/bitwise_double_operator.c",
        "/chapter_5/invalid_parse/declare_keyword_as_var.c",
        "/chapter_5/invalid_parse/invalid_type.c",
        "/chapter_5/invalid_parse/invalid_variable_name.c",
        "/chapter_5/invalid_parse/invalid_specifier.c",
        "/chapter_5/invalid_parse/missing_semicolon.c",
        "/chapter_5/invalid_parse/return_in_assignment.c",
        "/chapter_18/invalid_parse/struct_member_no_declarator.c",
        "/chapter_18/invalid_parse/extra_credit/union_member_no_declarator.c",
    };

    char path[512];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(path, sizeof(path), "%s%s", TP_TESTS, cases[i]);
        int before = error_count();
        Node *root = parse_file(path);
        char msg[600];
        snprintf(msg, sizeof(msg), "reject %s", cases[i] + 1);
        check(error_count() > before, msg);
        node_free(root);
    }
}

// Fixtures under tests/tests/error_parse/ are parser-stage cases: the lexical
// pass accepts them, so only the parser can reject them. One syntax error (or
// more) must be reported; the returned tree may be partial.
static void test_error_fixtures(void)
{
    static const char *const cases[] = {
        "missing_semicolon.c",
        "missing_function_name.c",
        "missing_paren.c",
        "double_operator.c",
        "bad_operator.c",
        "unclosed_brace.c",
        "invalid_declarator_constraints.c",
        "invalid_array_declarators.c",
        "old_style_unknown_parameter.c",
        "old_style_duplicate_parameter.c",
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[256];
        char msg[300];
        snprintf(path, sizeof(path), "tests/tests/error_parse/%s", cases[i]);
        int before = error_count();
        Node *root = parse_file(path);
        snprintf(msg, sizeof(msg), "reject error_parse/%s", cases[i]);
        check(error_count() > before, msg);
        node_free(root);
    }
}

// The first broken function must not swallow the rest of the file.
static void test_parse_recovery(void)
{
    int before = error_count();
    Node *root = parse_file("tests/tests/error_parse/recover.c");
    bool saw_broken = false;
    bool saw_main = false;

    for (Node *fn = first_function(root); fn; fn = fn->next) {
        const StringView *name =
            fn->type == ND_FUNCTION
                ? declarator_name(fn->u.func.declarator)
                : NULL;
        if (name) {
            if (string_view_equal_cstr(*name, "broken"))
                saw_broken = true;
            if (string_view_equal_cstr(*name, "main"))
                saw_main = true;
        }
    }
    check(error_count() > before, "recover.c: first function reports its error");
    check(saw_broken && saw_main, "recover.c: parser continued past the error");
    node_free(root);
}

// Non-grammar test helper: build a tree from an in-memory snippet. A synthetic
// SourceFile stands in for the driver-loaded file, and the complete lexer
// pipeline supplies the same located and decoded token contract as production.
static Node *parse_snippet(const char *src)
{
    static SourceFile snippet_file = {
        .name = (char *)"<snippet>",
    };
    snippet_file.contents = (char *)src;
    snippet_file.size = strlen(src);

    Token *tokens = tokenize(&snippet_file);
    Node *root = parse(tokens);
    token_free(tokens);
    return root;
}

static bool is_unary(Node *node, Operator op)
{
    return node && node->type == ND_UNARY && node->u.unary.op == op;
}

// Operator coverage added in M4: relational/logical/shift levels, ternary,
// assignment, comma, prefix/postfix and sizeof all reach the right node shape.
static void test_operators(void)
{
#define CHECK_EXPR(text, cond, msg)                                                                \
    do {                                                                                           \
        Node *root = parse_snippet("int main(void) { return " text "; }");                         \
        Node *expr = first_return_expr(first_function(root));                                      \
        check(cond, msg);                                                                          \
        node_free(root);                                                                           \
    } while (0)

    CHECK_EXPR("1 < 2", is_binary(expr, BINARY_LT), "parse 1 < 2");
    CHECK_EXPR("1 != 2", is_binary(expr, BINARY_NE), "parse 1 != 2");
    CHECK_EXPR("1 == 2", is_binary(expr, BINARY_EQ), "parse 1 == 2");
    CHECK_EXPR("1 && 2 || 3",
               is_binary(expr, BINARY_LOGOR) && is_binary(expr->u.binary.lhs, BINARY_LOGAND),
               "parse 1 && 2 || 3");
    CHECK_EXPR("1 | 2 ^ 3 & 4",
               is_binary(expr, BINARY_BITOR) && is_binary(expr->u.binary.rhs, BINARY_BITXOR) &&
                   is_binary(expr->u.binary.rhs->u.binary.rhs, BINARY_BITAND),
               "parse 1 | 2 ^ 3 & 4");
    CHECK_EXPR("1 << 2 + 3",
               is_binary(expr, BINARY_SHL) && is_binary(expr->u.binary.rhs, BINARY_ADD),
               "parse 1 << 2 + 3");
    CHECK_EXPR("1 = 2 = 3",
               expr && expr->type == ND_ASSIGN && expr->u.assign.rhs &&
                   expr->u.assign.rhs->type == ND_ASSIGN,
               "parse 1 = 2 = 3 (right associative)");
    CHECK_EXPR("(value) = 3", expr && expr->type == ND_ASSIGN,
               "parenthesized unary-expression selects assignment");
    CHECK_EXPR("1 ? 2 : 3", expr && expr->type == ND_COND, "parse 1 ? 2 : 3");
    CHECK_EXPR("1, 2", is_binary(expr, BINARY_COMMA), "parse 1, 2");
    CHECK_EXPR("++1", is_unary(expr, UNARY_PRE_INC), "parse ++1");
    CHECK_EXPR("++sizeof 1",
               is_unary(expr, UNARY_PRE_INC) &&
                   is_unary(expr->u.unary.operand, UNARY_SIZEOF),
               "parse prefix increment before keyword unary-expression");
    CHECK_EXPR("1--", is_unary(expr, UNARY_POST_DEC), "parse 1--");
    CHECK_EXPR("&1", is_unary(expr, UNARY_ADDR), "parse &1");
    CHECK_EXPR("*1", is_unary(expr, UNARY_DEREF), "parse *1");
    CHECK_EXPR("sizeof 1", is_unary(expr, UNARY_SIZEOF), "parse sizeof 1");
#if GNU_EXTENSION
    CHECK_EXPR("__alignof__ 1", is_unary(expr, UNARY_ALIGNOF), "parse __alignof__ 1");
    CHECK_EXPR("__alignof__(int)",
               expr && expr->type == ND_ALIGNOF_TYPE &&
                   expr->u.type_query.type_name,
               "parse __alignof__(type-name)");
#endif

#undef CHECK_EXPR

    // assignment-expression may select its assignment branch only when the
    // operator immediately follows a complete unary-expression.
    static char assignment_prefix_src[] = "a + b = c";
    static SourceFile assignment_prefix_file = {
        .name = (char *)"<assignment-prefix>",
        .contents = assignment_prefix_src,
        .size = sizeof(assignment_prefix_src) - 1,
    };
    Token *tokens = tokenize(&assignment_prefix_file);
    ParserCursor cursor = { .token = tokens };
    Node *prefix = parse_assignment_expression(&cursor);
    check(is_binary(prefix, BINARY_ADD) &&
              token_is(current_token(&cursor), TOK_ASSIGN),
          "assignment branch requires a unary-expression lhs");
    node_free(prefix);
    token_free(tokens);

    static char cast_prefix_src[] = "(int)a = b";
    assignment_prefix_file.contents = cast_prefix_src;
    assignment_prefix_file.size = sizeof(cast_prefix_src) - 1;
    tokens = tokenize(&assignment_prefix_file);
    cursor = (ParserCursor){ .token = tokens };
    prefix = parse_assignment_expression(&cursor);
    check(prefix && prefix->type == ND_CAST &&
              token_is(current_token(&cursor), TOK_ASSIGN),
          "cast-expression cannot become an assignment lhs");
    node_free(prefix);
    token_free(tokens);

    int errors = error_count();
    Node *root = parse_snippet("int main(void) { return _Alignof 1; }");
    check(error_count() > errors, "reject _Alignof expression");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int main(void) { int a; a = 1; *&a = 1; return a; }");
    check(error_count() == errors, "identifiers and dereferences are assignable");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int main(void) { 1 = 2; return 0; }");
    Node *assignment = first_statement(first_function(root));
    check(error_count() == errors && assignment &&
              assignment->type == ND_EXPR_STMT && assignment->u.expr.expr &&
              assignment->u.expr.expr->type == ND_ASSIGN,
          "assignment constraints remain semantic work");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int main(void) { int a; int b; a = b = 1; return a; }");
    check(error_count() == errors, "chained assignment parses");
    node_free(root);

    errors = error_count();
    root = parse_snippet("int main(void) { int a; int b; (1 ? a : b) = 1; return a; }");
    check(error_count() == errors,
          "parenthesized assignment lhs remains parser-valid");
    node_free(root);

    errors = error_count();
    const char *postfix_src =
        "int main(void) { return items[1].next->value[2]; }";
    root = parse_snippet(postfix_src);
    Node *postfix = first_return_expr(first_function(root));
    check(error_count() == errors && postfix &&
              string_view_begin(postfix->span.text) == strstr(postfix_src, "items[1]") &&
              string_view_length(postfix->span.text) == strlen("items[1].next->value[2]"),
          "subscript and member suffixes form one postfix-expression");
    Node *indirect = postfix && postfix->type == ND_SUBSCRIPT
                         ? postfix->u.subscript.base
                         : NULL;
    Node *direct = indirect && indirect->type == ND_MEMBER
                       ? indirect->u.member_access.base
                       : NULL;
    check(indirect && indirect->type == ND_MEMBER &&
              indirect->u.member_access.kind == MEMBER_INDIRECT && direct &&
              direct->type == ND_MEMBER &&
              direct->u.member_access.kind == MEMBER_DIRECT,
          "member suffix selection preserves direct and indirect variants");
    node_free(root);

    // A long prefix chain must be folded iteratively (no recursion frames).
    char src[4096];
    char *p = src;
    p += sprintf(p, "int main(void) { int x; return ");
    for (int i = 0; i < 1000; i++) {
        *p++ = '-';
        *p++ = ' ';
    }
    memcpy(p, "x; }", sizeof("x; }"));
    errors = error_count();
    root = parse_snippet(src);
    check(error_count() == errors, "deep prefix chain parses without recursion");
    Node *stmt = first_statement(first_function(root));
    stmt = stmt ? stmt->next : NULL;
    Node *ret = stmt && stmt->type == ND_RETURN ? stmt->u.expr.expr : NULL;
    check(ret && ret->type == ND_UNARY && ret->u.unary.op == UNARY_NEG,
          "deep prefix chain folds to the outermost operator");
    node_free(root);

    // Long right-associated chains must not consume one C call frame per
    // assignment or conditional layer.
    const size_t deep_layers = 20000;
    size_t capacity = deep_layers * 8 + 64;
    char *deep_src = xmalloc(capacity);
    p = deep_src;
    p += sprintf(p, "int main(void) { return ");
    for (size_t i = 0; i < deep_layers; i++)
        p += sprintf(p, "a=");
    memcpy(p, "a; }", sizeof("a; }"));
    errors = error_count();
    root = parse_snippet(deep_src);
    Node *deep = first_return_expr(first_function(root));
    size_t depth = 0;
    while (deep && deep->type == ND_ASSIGN) {
        depth++;
        deep = deep->u.assign.rhs;
    }
    check(error_count() == errors && depth == deep_layers,
          "deep assignment chain parses iteratively");
    node_free(root);
    free(deep_src);

    deep_src = xmalloc(capacity);
    p = deep_src;
    p += sprintf(p, "int main(void) { return ");
    for (size_t i = 0; i < deep_layers; i++)
        p += sprintf(p, "a?a:");
    memcpy(p, "a; }", sizeof("a; }"));
    errors = error_count();
    root = parse_snippet(deep_src);
    deep = first_return_expr(first_function(root));
    depth = 0;
    while (deep && deep->type == ND_COND) {
        depth++;
        deep = deep->u.cond.else_expr;
    }
    check(error_count() == errors && depth == deep_layers,
          "deep conditional chain parses iteratively");
    node_free(root);
    free(deep_src);
}

static void test_primary_expressions(void)
{
    Node *root = parse_snippet("int main(void) { return \"a\" \"b\"; }");
    Node *expr = first_return_expr(first_function(root));
    check(expr && expr->type == ND_STRING && expr->u.string.length == 2 &&
              memcmp(expr->u.string.value, "ab", 2) == 0,
          "parse concatenated string-literal primary expression");
    node_free(root);

    int errors = error_count();
    root = parse_snippet("_Static_assert(1, \"a\" \"b\");");
    Node *assertion = root ? root->u.program.external_declarations : NULL;
    Node *message = assertion && assertion->type == ND_STATIC_ASSERT
                        ? assertion->u.static_assertion.message
                        : NULL;
    check(error_count() == errors && message && message->type == ND_STRING &&
              message->u.string.length == 2 &&
              memcmp(message->u.string.value, "ab", 2) == 0,
          "static assertion consumes one concatenated string literal");
    node_free(root);
}

// Joined node spans resolve line/column against the synthetic snippet file; byte
// extents come from the operand tokens.
static void test_span_extent(void)
{
    const char *src = "int main(void) { return 1 + 2; }";
    Node *root = parse_snippet(src);
    Node *fn = first_function(root);
    Node *expr = first_return_expr(fn);
    const char *open = strchr(src, '{');

    check(fn && string_view_begin(fn->span.text) == src &&
              string_view_length(fn->span.text) == strlen(src),
          "function span covers the definition");
    check(fn && string_view_begin(fn->u.func.body->span.text) == open &&
              string_view_length(fn->u.func.body->span.text) == strlen(open),
          "block span covers its braces");
    check(expr && expr->type == ND_BINARY &&
              string_view_begin(expr->span.text) == strstr(src, "1 + 2") &&
              string_view_length(expr->span.text) == 5,
          "binary span covers both operands");
    node_free(root);

    src = "int main(void) { return ((1)); }";
    root = parse_snippet(src);
    expr = first_return_expr(first_function(root));
    check(expr && string_view_length(expr->span.text) == 5 &&
              string_view_begin(expr->span.text)[0] == '(' &&
              string_view_end(expr->span.text)[-1] == ')',
          "parenthesized expression span covers every parenthesis");
    node_free(root);

    src = "int main(void) { return sizeof 1; }";
    root = parse_snippet(src);
    expr = first_return_expr(first_function(root));
    check(expr && expr->type == ND_UNARY && expr->u.unary.op == UNARY_SIZEOF &&
              string_view_begin(expr->span.text) == strstr(src, "sizeof") &&
              string_view_length(expr->span.text) == 8,
          "sizeof span covers keyword and operand");
    node_free(root);

    src = "int main(void) { return 1 += 2; }";
    root = parse_snippet(src);
    expr = first_return_expr(first_function(root));
    check(expr && expr->type == ND_ASSIGN &&
              string_view_begin(expr->span.text) == strstr(src, "1 +=") &&
              string_view_length(expr->span.text) == 6,
          "assignment span keeps the += operator");
    node_free(root);

    src = "int main(void) { return 1--; }";
    root = parse_snippet(src);
    expr = first_return_expr(first_function(root));
    check(expr && expr->type == ND_UNARY && expr->u.unary.op == UNARY_POST_DEC &&
              string_view_begin(expr->span.text) == strstr(src, "1--") &&
              string_view_length(expr->span.text) == 3,
          "postfix span covers the decrement");
    node_free(root);

    src = "int main(void){return 0;}\nint other(void){return 1;}";
    root = parse_snippet(src);
    Node *last = first_function(root);
    while (last && last->next)
        last = last->next;
    check(string_view_begin(root->span.text) == src &&
              string_view_length(root->span.text) == strlen(src),
          "program span covers all functions");
    check(last && string_view_begin(last->span.text) == strstr(src, "int other"),
          "later function span starts at its own declaration");
    node_free(root);
}

// Every builtin scalar keyword must reach function-definition construction
// with the right TypeKind; signed/unsigned map to int/uint.
static void test_typenames(void)
{
    static const struct {
        const char *spelling;
        TypeKind kind;
    } cases[] = {
        { "int", TYPE_INT },       { "char", TYPE_CHAR },   { "short", TYPE_SHORT },
        { "long", TYPE_LONG },     { "float", TYPE_FLOAT }, { "double", TYPE_DOUBLE },
        { "void", TYPE_VOID },     { "_Bool", TYPE_BOOL },  { "signed", TYPE_INT },
        { "unsigned", TYPE_UINT },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char src[128];
        char msg[160];
        snprintf(src, sizeof(src), "%s f(void) { return 1; }", cases[i].spelling);
        snprintf(msg, sizeof(msg), "return type of %s", cases[i].spelling);
        Node *root = parse_snippet(src);
        Node *fn = first_function(root);
        check(fn && declaration_specifiers_builtin_type(
                        fn->u.func.specifiers) == cases[i].kind,
              msg);
        node_free(root);
    }

    int errors = error_count();
    Node *root = parse_snippet("long long long long long long long long value;");
    check(error_count() == errors + 1, "many long specifiers report one bounded error");
    node_free(root);
}

// The diagnostic accessor used by the parser tests must observe new errors
// without needing a reset hook. Tests must use a real file for spans.
static void test_error_count(void)
{
    int before = error_count();
    Node *root = parse_file(TP_TESTS "/chapter_1/invalid_parse/no_semicolon.c");
    check(error_count() > before, "syntax error increments error_count");
    node_free(root);
}

// Recovery must keep parsing after an error: one run reports every independent
// syntax error and still returns the surrounding tree.
static void test_error_recovery_reports_all(void)
{
    int before = error_count();
    Node *root = parse_file("tests/tests/error_parse/multiple_errors.c");
    int functions = 0;
    for (Node *fn = root ? root->u.program.external_declarations : NULL; fn;
         fn = fn->next)
        functions++;
    check(error_count() - before >= 3, "recovery reports every independent syntax error");
    check(root && root->type == ND_PROGRAM && functions == 4,
          "recovery keeps parsing after errors");
    node_free(root);

    before = error_count();
    root = parse_snippet(
        "int broken = ; unsigned long next; int main(void) { return 0; }");
    Node *next = root ? root->u.program.external_declarations : NULL;
    check(error_count() > before && is_decl(next, TYPE_ULONG, "next"),
          "top-level recovery preserves complete declaration-specifiers");
    node_free(root);
}

// Block recovery must ignore boundaries inside nested delimiter groups and stop
// before the next valid block-item at the failed item's nesting depth.
static void test_block_recovery_respects_delimiters(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "int recover(void) { if () { int swallowed; } return 7; }");
    Node *statement = first_statement(first_function(root));

    check(error_count() > before && statement && statement->type == ND_RETURN &&
              statement->u.expr.expr && statement->u.expr.expr->type == ND_NUM,
          "block recovery preserves a statement after a nested invalid item");
    node_free(root);
}

// Top-level synchronization must recognize a declaration whose optional
// init-declarator-list is absent instead of consuming it with the broken item.
static void test_recovery_preserves_tag_only_declaration(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "int broken = ; struct Saved; "
        "int main(void) { return 0; }");
    Node *saved = root ? root->u.program.external_declarations : NULL;

    check(error_count() > before && saved && saved->type == ND_DECL &&
              saved->u.decl.specifiers->type->kind == TYPE_SPECIFIER_STRUCT &&
              saved->next && saved->next->type == ND_FUNCTION &&
              !saved->next->next,
          "top-level recovery preserves a tag-only declaration");
    node_free(root);
}

// Top-level recovery must reconstruct delimiters already consumed by the failed
// external-declaration and must not promote an inner declaration-like prefix.
static void test_top_level_recovery_respects_delimiters(void)
{
    int before = error_count();
    Node *root = parse_snippet(
        "int broken(int value int phantom; ); struct Saved; "
        "int main(void) { return 0; }");
    Node *saved = root ? root->u.program.external_declarations : NULL;

    check(error_count() > before && saved && saved->type == ND_DECL &&
              saved->u.decl.specifiers->type->kind == TYPE_SPECIFIER_STRUCT &&
              saved->next && saved->next->type == ND_FUNCTION &&
              !saved->next->next,
          "top-level recovery ignores declaration prefixes inside delimiters");
    node_free(root);
}

// Block recovery must treat a following preprocessing directive as trivia so
// it neither emits a second syntax error nor consumes the next valid statement.
static void test_block_recovery_skips_directives(void)
{
    int before = error_count();
    Node *root =
        parse_file("tests/tests/error_parse/block_recovery_directive.c");
    Node *stmt = first_statement(first_function(root));
    check(error_count() == before + 1,
          "block recovery skips a following preprocessing directive");
    check(stmt && stmt->type == ND_RETURN && is_num(stmt->u.expr.expr, 0),
          "block recovery retains the statement after a directive");
    node_free(root);
}

// Empty statements are valid C; they become ND_EMPTY so they do not surface as
// missing expressions in the block list.
static void test_null_statement(void)
{
    int before = error_count();

    Node *root = parse_snippet("int main(void) { ; ; return 0; }");
    Node *fn = first_function(root);
    Node *stmt = first_statement(fn);
    check(error_count() == before, "empty statements are accepted");
    check(fn && stmt && stmt->type == ND_EMPTY && stmt->next && stmt->next->type == ND_EMPTY &&
              stmt->next->next && stmt->next->next->type == ND_RETURN,
          "empty statements become ND_EMPTY nodes");
    node_free(root);

    root = parse_snippet("int main(void) { { ; } return 1; }");
    fn = first_function(root);
    stmt = first_statement(fn);
    check(stmt && stmt->type == ND_BLOCK && stmt->u.block.items &&
              stmt->u.block.items->type == ND_EMPTY,
          "nested empty statement inside a block");
    node_free(root);

    root = parse_snippet("int main(void) { int; int a; return 0; }");
    stmt = first_statement(first_function(root));
    check(stmt && stmt->type == ND_DECL && !stmt->u.decl.declarators &&
              stmt->next && stmt->next->type == ND_DECL &&
              node_initializer(stmt->next) == NULL && stmt->next->next &&
              stmt->next->next->type == ND_RETURN,
          "constraint-invalid declaration stays before following valid items");
    node_free(root);
}

// Declaration context: implicit return types and nested definitions are
// intentionally rejected; recovery still returns the rest of the program.
static void test_declaration_context(void)
{
    int before = error_count();
    Node *root = parse_file("tests/tests/error_parse/hidden_return_type.c");
    check(error_count() - before >= 2, "implicit return type is rejected per definition");
    check(root && root->type == ND_PROGRAM, "recovery keeps parsing after an implicit return type");
    node_free(root);

    before = error_count();
    root = parse_file("tests/tests/error_parse/nested_function.c");
    check(error_count() > before, "function definition inside a block is rejected");
    check(root && root->type == ND_PROGRAM &&
              root->u.program.external_declarations,
          "recovery continues after a nested function definition");
    node_free(root);
}

static int run_tests(void)
{
    test_consumption();
    test_independent_cursors();
    test_token_type_precedes_spelling();
    test_function_definition_dispatch();
    test_constructors();
    test_deep_tree_release();
    test_parse_valid();
    test_null_statement();
    test_declaration_context();
    test_parse_invalid();
    test_parse_recovery();
    test_error_fixtures();
    test_operators();
    test_primary_expressions();
    test_span_extent();
    test_typenames();
    test_declarations();
    test_typedef_names();
    test_storage_classes();
    test_recursive_declarators();
    test_type_name_expressions();
    test_declaration_regressions();
    test_structured_type_specifiers();
    test_remaining_declaration_specifiers();
    test_atomic_qualifier_dispatch();
    test_remaining_primary_and_postfix_expressions();
    test_assignment_semantic_boundary();
    test_remaining_specifier_constraints();
    test_array_declarators();
    test_initializer_lists();
    test_control_flow();
    test_functions();
    test_error_count();
    test_error_recovery_reports_all();
    test_block_recovery_respects_delimiters();
    test_recovery_preserves_tag_only_declaration();
    test_top_level_recovery_respects_delimiters();
    test_block_recovery_skips_directives();
    test_expected_failures();

    release_test_sources();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all parser tests passed\n");
    return 0;
}

int main(void)
{
    return run_tests();
}

#endif // TEST_PARSER
