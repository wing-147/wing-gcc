// Coordinate semantic analysis and publish successful translation-unit results.

#include "frontend/sema.h"
#include "sema_internal.h"
#include "util/diagnostic.h"

typedef struct {
    const SourceSpan *alignment;
    const SourceSpan *incomplete_tentative;
    const SourceSpan *undefined_inline;
    const SourceSpan *inline_internal_reference;
    const SourceSpan *inline_modifiable_static;
} SemaFinalChecks;

// ---------------------------------------------------------------------------
// Translation-unit lifecycle
// ---------------------------------------------------------------------------

// Initialize per-translation-unit state and acquire an empty result.
static void init_sema_context(SemaContext *ctx)
{
    *ctx = (SemaContext){ .result = new_sema_result() };
    init_linkage_index(&ctx->linkage);
}

// Coordinate the signature and body, whose declarations share one scope.
static SemaStatus analyze_function_definition(SemaContext *ctx, const Node *node)
{
    SemaScope *file_scope = ctx->scope;
    const SemaType *base = NULL;
    SemaStatus specifiers = analyze_function_specifiers(ctx, node, &base);
    if (file_scope->visibility_unknown)
        return specifiers;

    SemaScope function_scope;
    push_scope(ctx, &function_scope);
    const SemaType *return_type = NULL;
    SemaEntity *function = NULL;
    SemaStatus signature = analyze_function_signature(ctx, file_scope, node, base, specifiers,
                                                      &return_type, &function);
    ctx->function = function;
    unsigned function_specifiers =
        function ? function->u.function.specifiers : node->u.func.specifiers->function_specifiers;
    SemaStatus body = SEMA_OK;
    if (!file_scope->visibility_unknown && !function_scope.visibility_unknown)
        body = analyze_function_body(ctx, node->u.func.body, return_type, function_specifiers);
    ctx->function = NULL;
    pop_scope(ctx);

    return merge_sema_status(signature, body);
}

// ---------------------------------------------------------------------------
// End-of-file entity checks and completion
// ---------------------------------------------------------------------------

// Report a tentative definition still lacking a complete type at file end.
static void report_incomplete_tentative_entity(const SourceSpan *span, StringView name)
{
    error_at(span, "tentative definition of '%.*s' has incomplete type",
             (int)string_view_length(name), string_view_begin(name));
}

// Check whether a tentative definition is known to remain incomplete at end of file.
static bool check_incomplete_tentative_entity(const SemaEntity *entity)
{
    if (entity->definition != SEMA_TENTATIVE)
        return false;
    const SemaType *type = entity->type;
    if ((type->kind == SEMA_TYPE_ARRAY && !type->u.array.count) || type_is_complete_object(type))
        return false;
    if ((type->kind == SEMA_TYPE_STRUCT || type->kind == SEMA_TYPE_UNION) &&
        type->u.tag->definition)
        return false;
    return true;
}

// A rejected declaration can obscure completion only if it could define this object.
static bool check_possible_object_definition(const SemaType *type,
                                             const SemaFailedObjectDefinition *failed)
{
    for (; failed; failed = failed->next) {
        if (failed->reason == SEMA_OBJECT_DEFINITION_TYPE_UNAVAILABLE ||
            type_is_compatible(type, failed->type))
            return true;
    }
    return false;
}

// Anchor the inline reference constraint at the internal-linkage name.
static void report_inline_reference(const SourceSpan *span)
{
    error_at(span, "inline definition references an internal-linkage name");
}

// Anchor the inline storage constraint at the modifiable object definition.
static void report_inline_static(const SourceSpan *span)
{
    error_at(span, "inline definition defines a modifiable static or thread-local object");
}

// Report an external inline declaration with no definition in this unit.
static void report_undefined_inline(const SourceSpan *span, StringView name)
{
    warn_at(span, "inline function '%.*s' is declared but never defined",
            (int)string_view_length(name), string_view_begin(name));
}

// Diagnose a too-weak request once its formerly incomplete type is known.
static void report_entity_alignment(const SourceSpan *span)
{
    error_at(span, "alignment specifier reduces the completed type's required alignment");
}

// Check whether this translation unit supplies an inline definition.
static bool check_inline_definition(const SemaEntity *entity, const SemaEntityAnalysis *analysis)
{
    return entity->type->kind == SEMA_TYPE_FUNCTION && entity->linkage == SEMA_LINKAGE_EXTERNAL &&
           (entity->u.function.specifiers & FUNCTION_SPEC_INLINE) &&
           entity->definition == SEMA_DEFINED && !analysis->has_external_function_declaration;
}

// Check each end-of-file constraint against the facts it depends on.
static SemaFinalChecks check_final_entity(const SemaEntity *entity,
                                          const SemaEntityAnalysis *analysis, unsigned recovery,
                                          const SemaFailedObjectDefinition *failed_objects)
{
    SemaFinalChecks checks = { 0 };
    if (entity->type->kind != SEMA_TYPE_FUNCTION) {
        unsigned alignment = entity->u.object.alignment;
        if (alignment && type_is_complete_object(entity->type) &&
            alignment < type_alignment(entity->type))
            checks.alignment = entity->name_span;
        if (check_incomplete_tentative_entity(entity) &&
            !check_possible_object_definition(entity->type, failed_objects))
            checks.incomplete_tentative = &entity->definition_node->span;
        return checks;
    }
    if (entity->linkage != SEMA_LINKAGE_EXTERNAL ||
        !(entity->u.function.specifiers & FUNCTION_SPEC_INLINE))
        return checks;
    if (entity->definition != SEMA_DEFINED) {
        if (!(recovery & SEMA_FAILED_FUNCTION_DEFINITION))
            checks.undefined_inline = entity->name_span;
    } else if (!(recovery & SEMA_FAILED_EXTERNAL_FUNCTION_DECLARATION) &&
               check_inline_definition(entity, analysis)) {
        checks.inline_internal_reference = analysis->inline_internal_reference;
        checks.inline_modifiable_static = analysis->inline_modifiable_static;
    }
    return checks;
}

// Obtain the checks for one entity, then report the issues at their recorded anchors.
static void analyze_final_entity(SemaEntity *entity, const SemaEntityAnalysis *analysis,
                                 void *context)
{
    const SemaLinkageIndex *linkage = context;
    unsigned recovery = lookup_linkage_recovery(linkage, entity->linkage, entity->name);
    const SemaFailedObjectDefinition *failed_objects =
        lookup_failed_object_definitions(linkage, entity->linkage, entity->name);
    SemaFinalChecks checks = check_final_entity(entity, analysis, recovery, failed_objects);
    if (checks.alignment)
        report_entity_alignment(checks.alignment);
    if (checks.incomplete_tentative)
        report_incomplete_tentative_entity(checks.incomplete_tentative, entity->name);
    if (checks.undefined_inline)
        report_undefined_inline(checks.undefined_inline, entity->name);
    if (checks.inline_internal_reference)
        report_inline_reference(checks.inline_internal_reference);
    if (checks.inline_modifiable_static)
        report_inline_static(checks.inline_modifiable_static);
}

// Commit end-of-file types and definition states after all diagnostics have run.
static void analyze_entity_completion(SemaEntity *entity, const SemaEntityAnalysis *analysis,
                                      void *context)
{
    SemaResult *result = context;
    SemaEntity value = *entity;
    if (value.definition == SEMA_TENTATIVE) {
        if (value.type->kind == SEMA_TYPE_ARRAY && !value.type->u.array.count)
            value.type = new_array_type(result, value.type->u.array.element, 1);
        value.definition = SEMA_ZERO_DEFINED;
    } else if (check_inline_definition(entity, analysis)) {
        value.u.function.is_inline_definition = true;
    } else {
        return;
    }
    update_entity(entity, value);
}

// ---------------------------------------------------------------------------
// Translation-unit traversal
// ---------------------------------------------------------------------------

// Walk external declarations in source order. A signature outside this stage
// can introduce file-scope names that the rest of the walk would miss.
static SemaStatus analyze_translation_unit(SemaContext *ctx, const Node *root)
{
    SemaStatus status = SEMA_OK;
    for (const Node *item = root->u.program.external_declarations; item; item = item->next) {
        SemaStatus current;
        switch (item->type) {
        case ND_FUNCTION:
            current = analyze_function_definition(ctx, item);
            break;
        case ND_DECL:
            current = analyze_declaration(ctx, item);
            break;
        case ND_STATIC_ASSERT:
            current = analyze_static_assertion(ctx, item);
            break;
        default:
            current = mark_unsupported(ctx, true);
            break;
        }
        status = merge_sema_status(status, current);
        if (ctx->scope->visibility_unknown)
            break;
    }
    return status;
}

// ---------------------------------------------------------------------------
// Analysis status and entry point
// ---------------------------------------------------------------------------

// Retain a known semantic error ahead of unsupported syntax or success.
SemaStatus merge_sema_status(SemaStatus first, SemaStatus second)
{
    if (first == SEMA_INVALID || second == SEMA_INVALID)
        return SEMA_INVALID;
    return first == SEMA_UNSUPPORTED || second == SEMA_UNSUPPORTED ? SEMA_UNSUPPORTED : SEMA_OK;
}

// Remember unsupported syntax. If skipped evaluation could hide declarations,
// the active scope cannot safely resolve any further names.
SemaStatus mark_unsupported(SemaContext *ctx, bool may_declare_names)
{
    ctx->unsupported = true;
    if (may_declare_names)
        mark_scope_visibility_unknown(ctx->scope);
    return SEMA_UNSUPPORTED;
}

// Own the analysis until completion. Discard incomplete results and prioritize
// demonstrated semantic errors over unsupported syntax.
SemaStatus sema_analyze(const Node *root, SemaResult **out)
{
    if (!root || root->type != ND_PROGRAM)
        unreachable();
    *out = NULL;

    int errors_before = error_count();
    SemaContext ctx;
    init_sema_context(&ctx);
    SemaScope file_scope;
    push_scope(&ctx, &file_scope);
    SemaStatus status = analyze_translation_unit(&ctx, root);
    if (!file_scope.visibility_unknown)
        visit_sema_entities(ctx.result, analyze_final_entity, &ctx.linkage);
    pop_scope(&ctx);
    free_linkage_index(&ctx.linkage);

    if (error_count() != errors_before)
        status = SEMA_INVALID;
    else if (ctx.unsupported)
        status = merge_sema_status(status, SEMA_UNSUPPORTED);
    if (status == SEMA_OK)
        visit_sema_entities(ctx.result, analyze_entity_completion, ctx.result);
    if (status == SEMA_OK) {
        *out = ctx.result;
    } else {
        sema_free(ctx.result);
    }
    return status;
}

#ifdef TEST_SEMA
#include "frontend/lexer.h"
#include "frontend/parser.h"

#include <errno.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Semantic test fixtures
// ---------------------------------------------------------------------------

// Exit codes for validating a source file with sema_test.
typedef enum {
    TEST_ACCEPTED = 0,
    TEST_SEMANTIC_ERROR = 1,
    TEST_UNSUPPORTED = 2,
    TEST_SYNTAX_ERROR = 3,
    TEST_DRIVER_ERROR = 4,
} TestOutcome;

typedef struct {
    SourceFile *source;
    Node *root;
    SemaResult *result;
    SemaStatus status;
    int errors;
} AnalyzedSnippet;

// Fail the test driver at the first violated semantic expectation.
static void check_test(bool passed, const char *message)
{
    if (!passed)
        fatal("sema_test: %s", message);
}

// Parse real source and retain the inputs for a semantic test.
static AnalyzedSnippet parse_snippet(const char *text)
{
    SourceFile *source = xmalloc(sizeof(*source));
    *source = (SourceFile){ .name = xstrdup("<sema-test>"),
                            .contents = xstrdup(text),
                            .size = strlen(text) };
    int errors_before = error_count();
    Token *tokens = tokenize(source);
    check_test(error_count() == errors_before, "fixture passes lexer");
    Node *root = parse(tokens);
    token_free(tokens);
    check_test(error_count() == errors_before, "fixture passes parser");

    return (AnalyzedSnippet){ .source = source, .root = root };
}

// Retain the AST, result, and diagnostic count from the production entry.
static AnalyzedSnippet analyze_snippet(const char *text)
{
    AnalyzedSnippet snippet = parse_snippet(text);
    int errors_before = error_count();
    snippet.status = sema_analyze(snippet.root, &snippet.result);
    snippet.errors = error_count() - errors_before;
    return snippet;
}

// Release the semantic annotations before their borrowed AST and source.
static void free_snippet(AnalyzedSnippet *snippet)
{
    sema_free(snippet->result);
    parse_free(snippet->root);
    source_free(snippet->source);
}

// Verify the smallest runnable path from source to a returned int value.
static void test_integer_return(void)
{
    AnalyzedSnippet snippet = analyze_snippet("int f(void) { return 0; }");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0, "a constant return succeeds");
    const Node *function = snippet.root->u.program.external_declarations;
    const SemaSymbol *symbol = sema_lookup_symbol(snippet.result, function);
    check_test(symbol && symbol->type->kind == SEMA_TYPE_FUNCTION &&
                   symbol->type->u.function.parameter_count == 0 &&
                   symbol->type->u.function.return_type->kind == SEMA_TYPE_INT,
               "the void prototype declares zero parameters and returns int");
    const Node *expression = function->u.func.body->u.block.items->u.expr.expr;
    const SemaExprInfo *info = sema_lookup_expression(snippet.result, expression);
    check_test(info && info->original.type->kind == SEMA_TYPE_INT &&
                   info->original.category == SEMA_VALUE && info->conversions.count == 0,
               "an int constant needs no implicit conversion");
    free_snippet(&snippet);
}

// Check symbol identity and the complete lvalue conversion at one use.
static void check_lvalue_use(const SemaResult *result, const Node *expression,
                             const Node *declaration, const Node *use)
{
    const SemaExprInfo *info = sema_lookup_expression(result, expression);
    const SemaSymbol *symbol = sema_lookup_symbol(result, declaration);
    check_test(info && symbol && info->u.symbol == symbol,
               "the identifier resolves to its visible declaration");
    SemaExprProperties converted = sema_converted_properties(info);
    check_test(info->original.type == symbol->type && info->original.category == SEMA_LVALUE &&
                   converted.type->kind == SEMA_TYPE_INT && !converted.type->qualifiers &&
                   converted.category == SEMA_VALUE && info->conversions.count == 1,
               "reading an object records one int lvalue conversion");
    const SemaConversion *step = &info->conversions.steps[0];
    check_test(step->kind == SEMA_CONVERSION_LVALUE_TO_VALUE && step->use == use &&
                   step->from.type == info->original.type &&
                   step->from.category == info->original.category &&
                   step->to.type == converted.type && step->to.category == converted.category,
               "the step preserves its cause and both endpoints");
}

// Verify source-order declarations, shadowing and assignment conversions.
static void test_local_conversions(void)
{
    AnalyzedSnippet snippet = analyze_snippet("int f(int p) {"
                                              "  const int x = p; int y = x, z = y;"
                                              "  { int y = z; z = y; }"
                                              "  return y = x;"
                                              "}");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0,
               "initializers, shadowing and assignments succeed");

    const SemaResult *result = snippet.result;
    const Node *function = snippet.root->u.program.external_declarations;
    const Node *parameter = function->u.func.declarator->u.function.clause.prototype.parameters;
    const Node *first = function->u.func.body->u.block.items;
    const Node *x = first->u.decl.declarators;
    const Node *y = first->next->u.decl.declarators;
    const Node *z = y->next;
    const Node *inner = first->next->next;
    const Node *inner_y = inner->u.block.items->u.decl.declarators;
    const Node *inner_assignment = inner->u.block.items->next->u.expr.expr;
    const Node *assignment = inner->next->u.expr.expr;

    const SemaSymbol *f = sema_lookup_symbol(result, function);
    const SemaSymbol *p = sema_lookup_symbol(result, parameter);
    const SemaSymbol *const_x = sema_lookup_symbol(result, x);
    check_test(f && p && f->type->u.function.parameter_count == 1 &&
                   f->type->u.function.parameters[0] == p->type && const_x &&
                   const_x->type->qualifiers == TYPE_QUAL_CONST,
               "function signatures and declared qualifiers survive scope exit");

    check_lvalue_use(result, x->u.init_decl.init, parameter, x);
    check_lvalue_use(result, y->u.init_decl.init, x, y);
    check_lvalue_use(result, z->u.init_decl.init, y, z);
    check_lvalue_use(result, inner_y->u.init_decl.init, z, inner_y);
    check_lvalue_use(result, inner_assignment->u.assign.rhs, inner_y, inner_assignment);
    check_lvalue_use(result, assignment->u.assign.rhs, x, assignment);

    const SemaExprInfo *lhs = sema_lookup_expression(result, assignment->u.assign.lhs);
    const SemaExprInfo *value = sema_lookup_expression(result, assignment);
    check_test(lhs && lhs->u.symbol == sema_lookup_symbol(result, y) &&
                   lhs->original.category == SEMA_LVALUE &&
                   sema_converted_properties(lhs).category == SEMA_LVALUE &&
                   lhs->conversions.count == 0,
               "the assignment target is the restored outer binding and stays an lvalue");
    check_test(value && value->original.category == SEMA_VALUE &&
                   value->original.type == lhs->original.type && value->conversions.count == 0,
               "the assignment result is an int value and needs no return conversion");
    free_snippet(&snippet);
}

// Verify a freshly declared name is already visible in its own initializer.
// The short circuit also keeps the source free of an uninitialized read.
static void test_declaration_point(void)
{
    AnalyzedSnippet snippet = analyze_snippet("int f(int x) { { int x = 0 && x; return x; } }");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0,
               "a local can shadow a parameter inside a nested block");
    const Node *function = snippet.root->u.program.external_declarations;
    const Node *block = function->u.func.body->u.block.items;
    const Node *item = block->u.block.items;
    const Node *declaration = item->u.decl.declarators;
    const Node *initializer = declaration->u.init_decl.init;
    check_lvalue_use(snippet.result, initializer->u.binary.rhs, declaration, initializer);
    check_lvalue_use(snippet.result, item->next->u.expr.expr, declaration, item->next);
    free_snippet(&snippet);
}

// Assert a failed analysis emits exactly the expected independent diagnostics.
static void check_failed_snippet(const char *text, SemaStatus status, int errors)
{
    AnalyzedSnippet snippet = analyze_snippet(text);
    check_test(snippet.status == status && snippet.errors == errors && !snippet.result,
               "a failed analysis has the expected status and diagnostic count");
    free_snippet(&snippet);
}

// Check shared parameter scope, bad assignments and continued error recovery.
static void test_invalid_programs(void)
{
    check_failed_snippet("int f(int x) { int x; return x; }", SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { missing + 1; return other; }", SEMA_INVALID, 2);
    check_failed_snippet("int f(void) { const int x = 0; x = missing; return; }", SEMA_INVALID, 3);
    check_failed_snippet("int f(void) { return (0) = 1; }", SEMA_INVALID, 1);
}

// Check feature diagnostics for parsed syntax that semantic analysis defers.
static void test_unsupported_programs(void)
{
    check_failed_snippet("int f(a) int a; { return a; }", SEMA_INVALID, 1);
    check_failed_snippet("int f(int n) { int a[n]; return a[0]; }", SEMA_INVALID, 1);
    check_failed_snippet("int f(int (*a)(int n, int b[n])); int g(void) { return f(0); }",
                         SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { { enum { A }; A; } return missing; }", SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { return missing + 1L; }", SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { return 1L + missing; }", SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { 1L; return missing; }", SEMA_INVALID, 1);
}

// Check local analysis statuses before the public entry accounts for diagnostic counts.
static void test_recovery_status_precedence(void)
{
    check_test(merge_sema_status(SEMA_UNSUPPORTED, SEMA_INVALID) == SEMA_INVALID &&
                   merge_sema_status(SEMA_INVALID, SEMA_UNSUPPORTED) == SEMA_INVALID,
               "invalid has priority over unsupported in either order");
    AnalyzedSnippet snippet = parse_snippet("int n; int call(); void f(void) {"
                                            "((int (*)[n])0)[missing_index];"
                                            "sizeof(int[n]) + missing_binary;"
                                            "(*(int (*)[n])0)[0] = missing_assignment;"
                                            "(*(int (*)[n])0)[0] += missing_update;"
                                            "sizeof(int[n]) ? missing_branch : 0;"
                                            "call(missing_arg, sizeof(int[n]));"
                                            "int a = missing_decl, b[n]; }");
    SemaContext ctx;
    init_sema_context(&ctx);
    SemaScope file, block;
    push_scope(&ctx, &file);
    const Node *first = snippet.root->u.program.external_declarations;
    check_test(analyze_declaration(&ctx, first) == SEMA_OK &&
                   analyze_declaration(&ctx, first->next) == SEMA_OK,
               "the bound and callee are visible");
    push_scope(&ctx, &block);
    const Node *item = first->next->next->u.func.body->u.block.items;
    for (; item->type == ND_EXPR_STMT; item = item->next) {
        int before = error_count();
        check_test(analyze_expression(&ctx, item->u.expr.expr) == SEMA_INVALID &&
                       !block.visibility_unknown && error_count() == before + 2,
                   "an independent expression error follows the unsupported type diagnostic");
    }
    int before = error_count();
    check_test(analyze_declaration(&ctx, item) == SEMA_INVALID && !block.visibility_unknown &&
                   error_count() == before + 2,
               "an unsupported declarator leaves lexical visibility known");
    pop_scope(&ctx);
    pop_scope(&ctx);
    free_linkage_index(&ctx.linkage);
    snippet.result = ctx.result;
    free_snippet(&snippet);

    snippet = parse_snippet("int a = missing_a; void f(int n, int b[n]) {} int c = missing_c;");
    init_sema_context(&ctx);
    push_scope(&ctx, &file);
    before = error_count();
    check_test(analyze_translation_unit(&ctx, snippet.root) == SEMA_INVALID &&
                   !file.visibility_unknown && error_count() == before + 3,
               "an invalid signature leaves later file declarations visible");
    pop_scope(&ctx);
    free_linkage_index(&ctx.linkage);
    snippet.result = ctx.result;
    free_snippet(&snippet);
}

// Check that a failed declaration leaves its initializer and following declarators recoverable.
static void test_declarator_sibling_recovery(void)
{
    check_failed_snippet("int a[1]; long a[1] = {0}, b = missing;", SEMA_INVALID, 2);
    check_failed_snippet(
        "int a[1]; long a[1] = {[missing_index] = {missing_value}}, b = missing_next;",
        SEMA_INVALID, 4);
    check_failed_snippet("void f(int a[missing_a], int b[missing_b]);", SEMA_INVALID, 2);
    check_failed_snippet("enum { n = 0 };"
                         "void f(int n[missing_a], int b[n.member], int c[missing_b]);",
                         SEMA_INVALID, 2);
    check_failed_snippet("void f(int a[missing][sizeof(enum { N = 3 })], int b[N]);", SEMA_INVALID,
                         1);
    check_failed_snippet("int n; void f(int a[n], int b[a[0]]);", SEMA_INVALID, 1);
    check_failed_snippet("struct S { int a[missing_a], b[missing_b]; int c[missing_c];"
                         "_Static_assert(0, \"independent assertion\"); };",
                         SEMA_INVALID, 4);
    check_failed_snippet("struct S { int a[missing_a]; int tail[]; };", SEMA_INVALID, 1);
    check_failed_snippet("struct S { _Atomic(const int) a[missing_a], b[missing_b]; };",
                         SEMA_INVALID, 3);
}

// Check variable declarators in object, typedef, parameter, and member contexts.
static void test_variable_type_constraints(void)
{
    check_failed_snippet("int n; int a[n];", SEMA_INVALID, 1);
    check_failed_snippet("int n; typedef int (*T)[n];", SEMA_INVALID, 1);
    check_failed_snippet("void f(int n) { static int a[2][n]; }", SEMA_INVALID, 1);
    check_failed_snippet("void f(int n) { static _Thread_local int a[n]; }", SEMA_INVALID, 1);
    check_failed_snippet("void f(int n) { static int (*p)[n]; static int (*q[2])[n]; }",
                         SEMA_INVALID, 2);
    check_failed_snippet("void f(int n) { extern int (*p)[n]; }", SEMA_INVALID, 1);
    check_failed_snippet("void f(int n) { int a[n] = {missing}; }", SEMA_INVALID, 2);
    check_failed_snippet("void f(int n) { int a[n], b = missing; a[0]; }", SEMA_INVALID, 2);
    check_failed_snippet("void f(int n) { int a[sizeof(enum { E = 3 }) + n], b = E; }",
                         SEMA_INVALID, 1);
    check_failed_snippet("void f(int n, int a[n]) {} int b = missing;", SEMA_INVALID, 2);
    check_failed_snippet("int f(a) int a; { return a; } int b = missing;", SEMA_INVALID, 2);
    check_failed_snippet("int n; struct S { int (*p)[n]; int b[missing]; };", SEMA_INVALID, 2);
    check_failed_snippet("void f(int n) { typedef int Row[n]; Row r; int b = missing; }",
                         SEMA_INVALID, 2);
    check_failed_snippet("int a[1 / 0];", SEMA_INVALID, 1);
}

// Check independent children of failed type names, casts, and compound literals.
static void test_failed_type_children(void)
{
    check_failed_snippet("void f(void) { (_Atomic(const int) (*)[missing_bound])missing_operand; }",
                         SEMA_INVALID, 3);
    check_failed_snippet("void f(void) { (void){missing_init}; }", SEMA_INVALID, 2);
    check_failed_snippet("void f(void) { (_Atomic(const int)[missing_bound]){missing_init}; }",
                         SEMA_INVALID, 3);
}

// Report only end-of-file constraints whose facts survive error recovery.
static void test_failed_translation_unit_finalization(void)
{
    check_failed_snippet("struct S; struct S a; int b = missing;", SEMA_INVALID, 2);
    check_failed_snippet("struct S a; int b = missing; struct S { int x; };", SEMA_INVALID, 1);
    check_failed_snippet("struct S a; struct S { int x[missing]; };", SEMA_INVALID, 1);
    check_failed_snippet("struct S a; struct S a = {missing};", SEMA_INVALID, 2);
    check_failed_snippet("struct S; struct S a; int a = 0;", SEMA_INVALID, 2);
    check_failed_snippet("struct S a; struct T a = {0};", SEMA_INVALID, 2);
    check_failed_snippet("int a = missing; struct S a;", SEMA_INVALID, 2);
    check_failed_snippet("struct S a = {missing}; struct S a;", SEMA_INVALID, 2);
    check_failed_snippet("struct S a = {missing}; int a = other; struct S a;", SEMA_INVALID, 3);
    check_failed_snippet("struct S a; void g(void) { extern int a; }", SEMA_INVALID, 2);
    check_failed_snippet("struct S; _Alignas(1) extern struct S a;"
                         "void g(void) { extern int a; } struct S { long i; };",
                         SEMA_INVALID, 2);
    check_failed_snippet("struct S; _Alignas(1) extern struct S a;"
                         "struct S a = {missing}; struct S { long i; };",
                         SEMA_INVALID, 3);
    check_failed_snippet("static int hidden; inline int f(void) { return hidden; }"
                         "void g(void) { extern int f(int); }",
                         SEMA_INVALID, 2);
    check_failed_snippet("static int hidden; inline int f(void) { return hidden; }"
                         "inline int f(int);",
                         SEMA_INVALID, 2);
    check_failed_snippet(
        "static int hidden; inline int f(void) { return hidden; } extern int f(void);"
        "void g(void) { extern int f(int); }",
        SEMA_INVALID, 1);
    check_failed_snippet("extern _Atomic(const int) f(void);"
                         "static int hidden; inline int f(void) { return hidden; }",
                         SEMA_INVALID, 1);
    check_failed_snippet("static int hidden; inline int f(void) { return 0; }"
                         "inline int f(int a[missing_bound]) { return hidden; }",
                         SEMA_INVALID, 1);
    check_failed_snippet(
        "static int hidden; inline int f(void) { static int local; return hidden; }"
        "inline int f(void);",
        SEMA_INVALID, 2);
}

// Verify the kind and endpoint types of one conversion.
static void check_conversion_step(const SemaConversion *step, SemaConversionKind kind,
                                  SemaTypeKind from, SemaTypeKind to)
{
    check_test(step->kind == kind && step->from.type->kind == from && step->to.type->kind == to,
               "conversion order and endpoint types match");
}

// Check the conversion at one exact position in an expression's chain.
static void check_conversion(const SemaExprInfo *info, size_t index, SemaConversionKind kind,
                             SemaTypeKind from, SemaTypeKind to)
{
    check_test(info && index < info->conversions.count, "expected conversion exists");
    check_conversion_step(&info->conversions.steps[index], kind, from, to);
}

// Verify integer rank and range, shift rules and arithmetic domains.
static void test_arithmetic_conversions(void)
{
    AnalyzedSnippet snippet = analyze_snippet(
        "long widen(short s) { return s + 1L; }"
        "long sh(unsigned short s, signed char n) { return s << n; }"
        "long mixed(unsigned int u, long s) { return u + s; }"
        "unsigned long long mixed_rank(long long s, unsigned long u) { return s + u; }"
        "double real(short s) { return s + 1.0; }"
        "double _Complex complex(float _Complex z) { return z + 1.0; }");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0,
               "integer, real and complex arithmetic succeeds");
    const Node *fn = snippet.root->u.program.external_declarations;
    const Node *sum = fn->u.func.body->u.block.items->u.expr.expr;
    const SemaExprInfo *lhs = sema_lookup_expression(snippet.result, sum->u.binary.lhs);
    check_test(lhs->conversions.count == 3, "short to long records three distinct steps");
    check_conversion(lhs, 0, SEMA_CONVERSION_LVALUE_TO_VALUE, SEMA_TYPE_SHORT, SEMA_TYPE_SHORT);
    check_conversion(lhs, 1, SEMA_CONVERSION_INTEGER_PROMOTION, SEMA_TYPE_SHORT, SEMA_TYPE_INT);
    check_conversion(lhs, 2, SEMA_CONVERSION_USUAL_ARITHMETIC, SEMA_TYPE_INT, SEMA_TYPE_LONG);

    fn = fn->next;
    const Node *shift = fn->u.func.body->u.block.items->u.expr.expr;
    lhs = sema_lookup_expression(snippet.result, shift->u.binary.lhs);
    const SemaExprInfo *rhs = sema_lookup_expression(snippet.result, shift->u.binary.rhs);
    check_conversion(lhs, 1, SEMA_CONVERSION_INTEGER_PROMOTION, SEMA_TYPE_USHORT, SEMA_TYPE_INT);
    check_conversion(rhs, 1, SEMA_CONVERSION_INTEGER_PROMOTION, SEMA_TYPE_SCHAR, SEMA_TYPE_INT);
    check_conversion(sema_lookup_expression(snippet.result, shift), 0, SEMA_CONVERSION_ASSIGNMENT,
                     SEMA_TYPE_INT, SEMA_TYPE_LONG);

    fn = fn->next;
    sum = fn->u.func.body->u.block.items->u.expr.expr;
    lhs = sema_lookup_expression(snippet.result, sum->u.binary.lhs);
    check_conversion(lhs, 1, SEMA_CONVERSION_USUAL_ARITHMETIC, SEMA_TYPE_UINT, SEMA_TYPE_LONG);
    fn = fn->next;
    sum = fn->u.func.body->u.block.items->u.expr.expr;
    lhs = sema_lookup_expression(snippet.result, sum->u.binary.lhs);
    check_conversion(lhs, 1, SEMA_CONVERSION_USUAL_ARITHMETIC, SEMA_TYPE_LLONG, SEMA_TYPE_ULLONG);

    fn = fn->next;
    sum = fn->u.func.body->u.block.items->u.expr.expr;
    lhs = sema_lookup_expression(snippet.result, sum->u.binary.lhs);
    check_test(lhs->conversions.count == 2, "floating arithmetic skips integer promotion");
    check_conversion(lhs, 1, SEMA_CONVERSION_USUAL_ARITHMETIC, SEMA_TYPE_SHORT, SEMA_TYPE_DOUBLE);
    fn = fn->next;
    sum = fn->u.func.body->u.block.items->u.expr.expr;
    lhs = sema_lookup_expression(snippet.result, sum->u.binary.lhs);
    check_conversion(lhs, 1, SEMA_CONVERSION_USUAL_ARITHMETIC, SEMA_TYPE_FLOAT_COMPLEX,
                     SEMA_TYPE_DOUBLE_COMPLEX);
    check_test(sema_lookup_expression(snippet.result, sum->u.binary.rhs)->conversions.count == 0,
               "the real operand retains its domain");
    free_snippet(&snippet);
}

// Verify linked entity identity, lexical snapshots and translation-unit completion.
static void test_linked_entities(void)
{
    AnalyzedSnippet snippet = analyze_snippet("static int x; extern int x; int *p = &x;"
                                              "int f(void) { extern int x; return *p == x; }"
                                              "extern int y; int y; int y; int real = 5;");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0,
               "file-scope and block extern declarations succeed");
    const Node *file = snippet.root->u.program.external_declarations;
    const Node *x_decl = file->u.decl.declarators;
    const SemaSymbol *x = sema_lookup_symbol(snippet.result, x_decl);
    const SemaSymbol *x_seed = sema_lookup_symbol(snippet.result, file->next->u.decl.declarators);
    const Node *f = file->next->next->next;
    const Node *block_decl = f->u.func.body->u.block.items->u.decl.declarators;
    const SemaSymbol *x_block = sema_lookup_symbol(snippet.result, block_decl);
    check_test(x->entity == x_seed->entity && x->entity == x_block->entity &&
                   x->entity->linkage == SEMA_LINKAGE_INTERNAL &&
                   x->entity->definition == SEMA_ZERO_DEFINED,
               "visible extern inherits linkage and shares a completed tentative definition");
    StringView first_name = x_decl->u.init_decl.declarator->u.ident.name;
    check_test(string_view_begin(x->entity->name) == string_view_begin(first_name) &&
                   string_view_length(x->entity->name) == string_view_length(first_name),
               "updates keep the name borrowed from the first declaration");
    const SemaSymbol *y = sema_lookup_symbol(snippet.result, f->next->u.decl.declarators);
    const SemaSymbol *y_again =
        sema_lookup_symbol(snippet.result, f->next->next->next->u.decl.declarators);
    check_test(y->entity == y_again->entity && y->entity->definition == SEMA_ZERO_DEFINED,
               "external tentative definitions coalesce");
    free_snippet(&snippet);
}

// Verify that call conversions use each point's visible function signature.
static void test_call_snapshots(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("int target(); int early(void) { return target(1.0f); }"
                        "int target(double); int late(void) { return target(1.0f); }"
                        "int many(int n, ...); int var(void) { return many(1, 2.0f); }"
                        "int local(); int inside(void) { int local(int); return local(1); }"
                        "int outside(void) { return local(1); }");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0,
               "calls use prototypes, no prototype and variable arguments");
    const Node *first = snippet.root->u.program.external_declarations;
    const Node *early = first->next->u.func.body->u.block.items->u.expr.expr;
    const Node *late = first->next->next->next->u.func.body->u.block.items->u.expr.expr;
    const SemaExprInfo *early_callee = sema_lookup_expression(snippet.result, early->u.call.callee);
    const SemaExprInfo *late_callee = sema_lookup_expression(snippet.result, late->u.call.callee);
    check_test(early_callee->u.symbol->entity == late_callee->u.symbol->entity &&
                   early_callee->original.type->u.function.form == SEMA_FUNCTION_UNPROTOTYPED &&
                   late_callee->original.type->u.function.form == SEMA_FUNCTION_PROTOTYPE,
               "a later prototype updates the visible declaration only");
    check_conversion(early_callee, 0, SEMA_CONVERSION_FUNCTION_TO_POINTER, SEMA_TYPE_FUNCTION,
                     SEMA_TYPE_POINTER);
    check_conversion(sema_lookup_expression(snippet.result, early->u.call.args), 0,
                     SEMA_CONVERSION_DEFAULT_ARGUMENT, SEMA_TYPE_FLOAT, SEMA_TYPE_DOUBLE);
    check_conversion(sema_lookup_expression(snippet.result, late->u.call.args), 0,
                     SEMA_CONVERSION_ASSIGNMENT, SEMA_TYPE_FLOAT, SEMA_TYPE_DOUBLE);
    const Node *var = first->next->next->next->next->next;
    const Node *extra = var->u.func.body->u.block.items->u.expr.expr->u.call.args->next;
    check_conversion(sema_lookup_expression(snippet.result, extra), 0,
                     SEMA_CONVERSION_DEFAULT_ARGUMENT, SEMA_TYPE_FLOAT, SEMA_TYPE_DOUBLE);
    const Node *inside = var->next->next;
    const Node *inside_call = inside->u.func.body->u.block.items->next->u.expr.expr;
    const Node *outside_call = inside->next->u.func.body->u.block.items->u.expr.expr;
    const SemaSymbol *inside_symbol =
        sema_lookup_expression(snippet.result, inside_call->u.call.callee)->u.symbol;
    const SemaSymbol *outside_symbol =
        sema_lookup_expression(snippet.result, outside_call->u.call.callee)->u.symbol;
    check_test(inside_symbol->entity == outside_symbol->entity &&
                   inside_symbol->type->u.function.form == SEMA_FUNCTION_PROTOTYPE &&
                   outside_symbol->type->u.function.form == SEMA_FUNCTION_UNPROTOTYPED,
               "a block prototype shares the entity and stays local to its scope");
    free_snippet(&snippet);
}

// Preserve a zero-parameter definition across non-prototype declarations.
static void test_empty_function_definition(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("int zero(); int zero() { return 0; } int zero();"
                        "int early(void) { return zero(); } int zero(void);"
                        "int back(void); int back() { return zero(); }"
                        "int (*factory())() { return 0; } int (*factory(void))(int);");
    check_test(snippet.status == SEMA_OK && !snippet.errors, "empty function definitions succeed");
    const Node *first = snippet.root->u.program.external_declarations;
    const SemaSymbol *forward = sema_lookup_symbol(snippet.result, first->u.decl.declarators);
    const SemaSymbol *definition = sema_lookup_symbol(snippet.result, first->next);
    const Node *call = first->next->next->next->u.func.body->u.block.items->u.expr.expr;
    const SemaSymbol *visible =
        sema_lookup_expression(snippet.result, call->u.call.callee)->u.symbol;
    check_test(forward->entity == definition->entity && visible->entity == definition->entity &&
                   forward->type->u.function.form == SEMA_FUNCTION_UNPROTOTYPED &&
                   definition->type->u.function.form == SEMA_FUNCTION_EMPTY_DEFINITION &&
                   visible->type->u.function.form == SEMA_FUNCTION_EMPTY_DEFINITION &&
                   visible->type->u.function.parameter_count == 0 &&
                   visible->entity->type->u.function.form == SEMA_FUNCTION_PROTOTYPE,
               "the early call and earlier declarations retain their signature snapshots");
    free_snippet(&snippet);

    check_failed_snippet("int f(int); int f() { return 0; }", SEMA_INVALID, 1);
    check_failed_snippet("int f() { return 0; } int f(); int f(int);", SEMA_INVALID, 1);
    check_failed_snippet("void g(void) { extern int f(int, ...); } int f() { return 0; }",
                         SEMA_INVALID, 1);
}

// Verify typedef-derived qualifiers and pointer constants when initialized.
static void test_pointer_initializers(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("typedef int *P; const P p = 2 - 2;"
                        "typedef int F(void); F target;"
                        "int (*nil)(void) = (void *)(sizeof(int) - 4);"
                        "int f(void) { int (*fn)(void) = target; return fn(); }");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0,
               "typedefs, abstract declarators and function pointers succeed");
    const Node *first = snippet.root->u.program.external_declarations;
    const Node *decl = first->next->u.decl.declarators;
    const SemaSymbol *p = sema_lookup_symbol(snippet.result, decl);
    check_test(p->type->qualifiers == TYPE_QUAL_CONST && !p->type->u.pointee->qualifiers,
               "qualifying the alias qualifies the pointer itself");
    check_conversion(sema_lookup_expression(snippet.result, decl->u.init_decl.init), 0,
                     SEMA_CONVERSION_NULL_POINTER, SEMA_TYPE_INT, SEMA_TYPE_POINTER);
    const Node *nil = first->next->next->next->next->u.decl.declarators;
    check_conversion(sema_lookup_expression(snippet.result, nil->u.init_decl.init), 0,
                     SEMA_CONVERSION_NULL_POINTER, SEMA_TYPE_POINTER, SEMA_TYPE_POINTER);
    free_snippet(&snippet);
}

// Reject an assignment without publishing a read conversion on its source.
static void test_rejected_assignment_conversion(void)
{
    AnalyzedSnippet snippet = parse_snippet("const int value = 0; int *p = value;");
    const Node *first = snippet.root->u.program.external_declarations;
    const Node *initializer = first->next->u.decl.declarators->u.init_decl.init;
    SemaContext ctx;
    init_sema_context(&ctx);
    SemaScope scope;
    push_scope(&ctx, &scope);
    check_test(analyze_declaration(&ctx, first) == SEMA_OK, "source binding is valid");
    check_test(analyze_declaration(&ctx, first->next) == SEMA_INVALID,
               "a const integer lvalue fails pointer initialization");
    const SemaExprInfo *info = sema_lookup_expression(ctx.result, initializer);
    check_test(info && info->original.category == SEMA_LVALUE &&
                   sema_converted_properties(info).category == SEMA_LVALUE &&
                   sema_converted_properties(info).type == info->original.type &&
                   !info->conversions.count &&
                   !sema_lookup_initializer(ctx.result, first->next->u.decl.declarators),
               "a rejected conversion preserves the source lvalue and its qualifiers");
    pop_scope(&ctx);
    free_linkage_index(&ctx.linkage);
    snippet.result = ctx.result;
    free_snippet(&snippet);
}

// Verify qualified pointer conversions and null constants in either position.
static void test_common_pointer_conversions(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("int f(const int *p, volatile int *q) { return p < q; }"
                        "const int *g(const int *p) { return p ? p : (2 - 2); }"
                        "int h(const int *p) { return (2 - 2) == p; }"
                        "_Static_assert((0 ? 1 / 0 : 7) == 7, \"selected operand only\");");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0, "pointer operands succeed");
    const Node *first = snippet.root->u.program.external_declarations;
    const Node *comparison = first->u.func.body->u.block.items->u.expr.expr;
    const SemaExprInfo *left = sema_lookup_expression(snippet.result, comparison->u.binary.lhs);
    const SemaExprInfo *right = sema_lookup_expression(snippet.result, comparison->u.binary.rhs);
    check_test(left->conversions.count == 2 && right->conversions.count == 2 &&
                   sema_converted_properties(left).type == sema_converted_properties(right).type &&
                   sema_converted_properties(left).type->u.pointee->qualifiers ==
                       (TYPE_QUAL_CONST | TYPE_QUAL_VOLATILE),
               "both pointer reads precede conversion to a common qualified type");
    check_conversion(left, 0, SEMA_CONVERSION_LVALUE_TO_VALUE, SEMA_TYPE_POINTER,
                     SEMA_TYPE_POINTER);
    check_conversion(right, 0, SEMA_CONVERSION_LVALUE_TO_VALUE, SEMA_TYPE_POINTER,
                     SEMA_TYPE_POINTER);
    check_conversion(left, 1, SEMA_CONVERSION_POINTER, SEMA_TYPE_POINTER, SEMA_TYPE_POINTER);
    check_conversion(right, 1, SEMA_CONVERSION_POINTER, SEMA_TYPE_POINTER, SEMA_TYPE_POINTER);
    const Node *conditional = first->next->u.func.body->u.block.items->u.expr.expr;
    const SemaExprInfo *choice = sema_lookup_expression(snippet.result, conditional);
    const SemaConditionInfo *test = sema_lookup_condition(snippet.result, conditional);
    const Node *control = conditional->u.cond.cond;
    check_test(choice->kind == SEMA_EXPR_CONDITIONAL && test == &choice->u.condition &&
                   test->expression == control && !sema_lookup_condition(snippet.result, control),
               "the conditional expression owns its separate nonzero test");
    check_conversion(sema_lookup_expression(snippet.result, control), 0,
                     SEMA_CONVERSION_LVALUE_TO_VALUE, SEMA_TYPE_POINTER, SEMA_TYPE_POINTER);
    check_conversion(sema_lookup_expression(snippet.result, conditional->u.cond.else_expr), 0,
                     SEMA_CONVERSION_NULL_POINTER, SEMA_TYPE_INT, SEMA_TYPE_POINTER);
    const Node *equality = first->next->next->u.func.body->u.block.items->u.expr.expr;
    check_conversion(sema_lookup_expression(snippet.result, equality->u.binary.lhs), 0,
                     SEMA_CONVERSION_NULL_POINTER, SEMA_TYPE_INT, SEMA_TYPE_POINTER);
    free_snippet(&snippet);
}

// A failed signature still permits known return constraints and independent body checks.
static void test_function_definition_recovery(void)
{
    check_failed_snippet("int f(int); void f(int x) { x; return missing; }"
                         "int g(void) { return f(1); }",
                         SEMA_INVALID, 3);
    check_failed_snippet("int f(void) { return; } void g(void) { return; }", SEMA_INVALID, 1);
    check_failed_snippet("int f(int a[missing_bound]) { return missing_body; }", SEMA_INVALID, 2);
    check_failed_snippet("int f(int a[missing_bound]) { return; }", SEMA_INVALID, 2);
    check_failed_snippet("int (*f(int a[missing_bound]))(void) { return 1; }", SEMA_INVALID, 2);
    check_failed_snippet("int a; int f(int a[missing_bound]) { return a[0] + missing_body; }",
                         SEMA_INVALID, 2);
    check_failed_snippet("_Atomic(const int) f(int n, int a[missing_bound]) {"
                         "if (n) return missing_body; goto absent; return; }",
                         SEMA_INVALID, 4);
    check_failed_snippet("int f(a) int a; { return a; } int b = missing_file;", SEMA_INVALID, 2);
    check_failed_snippet("typedef int A[2]; A f(int p[missing]) { return 0; }", SEMA_INVALID, 2);
    check_failed_snippet("typedef int F(void); F f(int p[missing]) { return 0; }", SEMA_INVALID, 2);
    check_failed_snippet("typedef int A[2]; A f(int p[missing]);", SEMA_INVALID, 2);
    check_failed_snippet("typedef int A[2]; A f(int p[missing]) { return missing_body; }",
                         SEMA_INVALID, 3);
}

// Track register storage through designators, clearing it once an object is read.
static void test_register_subobject_address(void)
{
    check_failed_snippet(
        "struct S { int x; }; void f(void) { register struct S s; int *p = &s.x; }", SEMA_INVALID,
        1);
    check_failed_snippet("struct S { struct { int x; } inner; };"
                         "void f(void) { register struct S s; int *p = &s.inner.x; }",
                         SEMA_INVALID, 1);
    check_failed_snippet("struct S { union { int x; }; };"
                         "void f(void) { register struct S s; int *p = &_Generic(0, int: s.x); }",
                         SEMA_INVALID, 1);
    check_failed_snippet("struct S { int a[2][2]; };"
                         "void f(void) { register struct S s; int *p = &1[s.a][0]; }",
                         SEMA_INVALID, 1);
    check_failed_snippet("void f(void) { register int a[2]; int *p = &a[0]; }", SEMA_INVALID, 1);
    AnalyzedSnippet snippet = analyze_snippet(
        "struct S { int x; int *p; }; int f(void) {"
        "int i = 1; register struct S s = {1, &i}; struct S ordinary = {2, &i};"
        "register struct S *p = &ordinary; s.x; return *&p->x + *&(*p).x + *&s.p[0]; }");
    check_test(snippet.status == SEMA_OK && !snippet.errors,
               "pointer access and ordinary register member reads succeed");
    const Node *function = snippet.root->u.program.external_declarations->next;
    const Node *first = function->u.func.body->u.block.items;
    const Node *read = first->next->next->next->next->u.expr.expr;
    const SemaExprInfo *info = sema_lookup_expression(snippet.result, read);
    check_test(info->original.register_object && info->conversions.count == 1 &&
                   info->conversions.steps[0].from.register_object &&
                   !sema_converted_properties(info).register_object,
               "lvalue conversion consumes the register designator without restricting its value");
    free_snippet(&snippet);
}

// Verify the new boundaries for type compatibility, calls and constant forms.
static void test_stage_two_constraints(void)
{
    check_failed_snippet("int g(); int g(float);", SEMA_INVALID, 1);
    check_failed_snippet("typedef void V; int f(V x); int g(void) { return f(0); }", SEMA_INVALID,
                         1);
    check_failed_snippet("int f(void); int g(void) { return f(1); }", SEMA_INVALID, 1);
    check_failed_snippet("int f(int **); int g(const int **p) { return f(p); }", SEMA_INVALID, 1);
    check_failed_snippet("int g(void) { int x = 0; int *p = x; return 0; }", SEMA_INVALID, 1);
    check_failed_snippet("int g(void) { int **p; const int **q = p; return 0; }", SEMA_INVALID, 1);
    check_failed_snippet("int g(void) { return x; } int x;", SEMA_INVALID, 1);
    check_failed_snippet("int x; int y = x;", SEMA_INVALID, 1);
    check_failed_snippet("int x = 2147483647 + 1;", SEMA_INVALID, 1);
    check_failed_snippet("int g(void) { int x; static int *p = &x; return 0; }", SEMA_INVALID, 1);
}

// Verify the separate update chains and prefix/postfix value selection.
static void test_update_conversions(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("short add(volatile short *p) { return (*p) += 1L; }"
                        "double after(double d) { return d++; }"
                        "short before(short n) { return --n; }"
                        "int *next(int *p) { return ++p; }"
                        "int *pick(void); int once(void) { return (*pick())++; }"
                        "int atomic_add(_Atomic int *p, short n) { return *p += n; }");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0, "updates succeed");

    const Node *function = snippet.root->u.program.external_declarations;
    const Node *node = function->u.func.body->u.block.items->u.expr.expr;
    const SemaExprInfo *info = sema_lookup_expression(snippet.result, node);
    const SemaUpdateInfo *update = info->u.update;
    check_test(
        info->kind == SEMA_EXPR_UPDATE && info->original.category == SEMA_VALUE &&
            info->original.type->kind == SEMA_TYPE_SHORT && update &&
            update->target == node->u.assign.lhs && update->arithmetic_operator == BINARY_ADD &&
            update->calculation_type->kind == SEMA_TYPE_LONG && update->target_read.count == 3 &&
            update->writeback.count == 1 && !update->implicit_rhs_type &&
            !(update->flags & SEMA_UPDATE_RETURNS_PREVIOUS_VALUE),
        "compound addition keeps one designator and separates calculation from writeback");
    const SemaExprInfo *target = sema_lookup_expression(snippet.result, update->target);
    check_test(target->original.category == SEMA_LVALUE && !target->conversions.count &&
                   update->target_read.steps[0].from.type->qualifiers == TYPE_QUAL_VOLATILE &&
                   !update->target_read.steps[0].to.type->qualifiers,
               "the volatile read is recorded by the update");
    check_conversion_step(&update->target_read.steps[0], SEMA_CONVERSION_LVALUE_TO_VALUE,
                          SEMA_TYPE_SHORT, SEMA_TYPE_SHORT);
    check_conversion_step(&update->target_read.steps[1], SEMA_CONVERSION_INTEGER_PROMOTION,
                          SEMA_TYPE_SHORT, SEMA_TYPE_INT);
    check_conversion_step(&update->target_read.steps[2], SEMA_CONVERSION_USUAL_ARITHMETIC,
                          SEMA_TYPE_INT, SEMA_TYPE_LONG);
    check_conversion_step(update->writeback.steps, SEMA_CONVERSION_ASSIGNMENT, SEMA_TYPE_LONG,
                          SEMA_TYPE_SHORT);

    function = function->next;
    node = function->u.func.body->u.block.items->u.expr.expr;
    update = sema_lookup_expression(snippet.result, node)->u.update;
    check_test(update && (update->flags & SEMA_UPDATE_RETURNS_PREVIOUS_VALUE) &&
                   update->implicit_rhs.count == 1 &&
                   update->implicit_rhs_type->kind == SEMA_TYPE_INT && !update->writeback.count,
               "postfix returns the old value and records the unit conversion");
    check_conversion_step(update->implicit_rhs.steps, SEMA_CONVERSION_USUAL_ARITHMETIC,
                          SEMA_TYPE_INT, SEMA_TYPE_DOUBLE);

    function = function->next;
    node = function->u.func.body->u.block.items->u.expr.expr;
    update = sema_lookup_expression(snippet.result, node)->u.update;
    check_test(update && !(update->flags & SEMA_UPDATE_RETURNS_PREVIOUS_VALUE) &&
                   update->arithmetic_operator == BINARY_SUB && update->writeback.count == 1,
               "prefix decrement returns the written value");
    check_conversion_step(update->writeback.steps, SEMA_CONVERSION_ASSIGNMENT, SEMA_TYPE_INT,
                          SEMA_TYPE_SHORT);

    function = function->next;
    node = function->u.func.body->u.block.items->u.expr.expr;
    update = sema_lookup_expression(snippet.result, node)->u.update;
    check_test(update && update->calculation_type->kind == SEMA_TYPE_POINTER &&
                   !update->implicit_rhs.count && !update->writeback.count,
               "incrementing an object pointer has an integer offset and a pointer result");

    function = function->next->next;
    node = function->u.func.body->u.block.items->u.expr.expr;
    update = sema_lookup_expression(snippet.result, node)->u.update;
    check_test(update && update->target == node->u.unary.operand &&
                   update->target->u.unary.operand->type == ND_CALL &&
                   update->target_read.count == 1,
               "an update through a call result uses the original designator");
    node = function->next->u.func.body->u.block.items->u.expr.expr;
    update = sema_lookup_expression(snippet.result, node)->u.update;
    check_test(update && (update->flags & SEMA_UPDATE_ATOMIC) && !update->implicit_rhs_type &&
                   !update->implicit_rhs.count &&
                   update->target_read.steps[0].kind == SEMA_CONVERSION_ATOMIC_LOAD,
               "an explicit operand of an atomic update retains its own conversion chain");
    const SemaExprInfo *rhs = sema_lookup_expression(snippet.result, node->u.assign.rhs);
    check_conversion(rhs, 0, SEMA_CONVERSION_LVALUE_TO_VALUE, SEMA_TYPE_SHORT, SEMA_TYPE_SHORT);
    check_conversion(rhs, 1, SEMA_CONVERSION_INTEGER_PROMOTION, SEMA_TYPE_SHORT, SEMA_TYPE_INT);
    free_snippet(&snippet);
}

// Check constraints that differ from the general binary operator rules.
static void test_invalid_updates(void)
{
    check_failed_snippet("void f(int *p, int *q) { p -= q; }", SEMA_INVALID, 1);
    check_failed_snippet("void f(int n, int *p) { n += p; }", SEMA_INVALID, 1);
    check_failed_snippet("void f(void *p) { p++; }", SEMA_INVALID, 1);
    check_failed_snippet("void f(double _Complex x) { ++x; }", SEMA_INVALID, 1);
    check_failed_snippet("void f(const int n) { n += 1; }", SEMA_INVALID, 1);
}

// ---------------------------------------------------------------------------
// Control statements and source-level targets
// ---------------------------------------------------------------------------

// Verify that an inner forward label is function-visible in its own namespace.
static void test_goto_targets(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("int f(void) { int target = 1; goto target; { target: return target; } }"
                        "int g(void) { target: goto target; }");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0, "same-function gotos succeed");
    const Node *first = snippet.root->u.program.external_declarations;
    const Node *jump = first->u.func.body->u.block.items->next;
    const Node *label = jump->next->u.block.items;
    check_test(sema_lookup_jump_target(snippet.result, jump) == label,
               "a forward goto resolves into a nested ordinary block");
    const Node *other_label = first->next->u.func.body->u.block.items;
    check_test(sema_lookup_jump_target(snippet.result, other_label->u.label.stmt) == other_label,
               "different functions may define the same label");
    free_snippet(&snippet);
    check_failed_snippet("int f(void) { goto other; } int g(void) { other: return 0; }",
                         SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { L: ; { L: ; } goto L; }", SEMA_INVALID, 1);
    check_failed_snippet("int f(int n) { goto L; { int a[n]; L: ; } return 0; }", SEMA_INVALID, 1);
}

// Verify jump destinations, promotions and case associations by AST identity.
static void test_switch_targets(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("int f(unsigned char n) { for (;;) { switch (n) {"
                        "case 0: break; case 256L: continue; default: ; } break; } return 0; }");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0, "switch inside for succeeds");
    const Node *function = snippet.root->u.program.external_declarations;
    const Node *loop = function->u.func.body->u.block.items;
    const Node *selection = loop->u.for_stmt.body->u.block.items;
    const Node *zero = selection->u.switch_stmt.body->u.block.items;
    const Node *larger = zero->next;
    const Node *otherwise = larger->next;
    check_test(sema_lookup_jump_target(snippet.result, zero->u.case_stmt.stmt) == selection &&
                   sema_lookup_jump_target(snippet.result, larger->u.case_stmt.stmt) == loop &&
                   sema_lookup_jump_target(snippet.result, selection->next) == loop,
               "break and continue choose the nearest applicable construct");
    const SemaConditionInfo *condition = sema_lookup_condition(snippet.result, loop);
    check_test(condition && !condition->expression, "an omitted for condition is always true");
    const SemaExprInfo *control =
        sema_lookup_expression(snippet.result, selection->u.switch_stmt.cond);
    check_test(control->conversions.count == 2, "switch reads and promotes its control");
    check_conversion(control, 0, SEMA_CONVERSION_LVALUE_TO_VALUE, SEMA_TYPE_UCHAR, SEMA_TYPE_UCHAR);
    check_conversion(control, 1, SEMA_CONVERSION_INTEGER_PROMOTION, SEMA_TYPE_UCHAR, SEMA_TYPE_INT);
    const SemaSwitchLabelInfo *label = sema_lookup_switch_label(snippet.result, larger);
    check_test(label && label->switch_statement == selection && label->value_bits == 256 &&
                   label->value_type == sema_converted_properties(control).type,
               "case records the converted value and owning switch");
    check_conversion(sema_lookup_expression(snippet.result, larger->u.case_stmt.expr), 0,
                     SEMA_CONVERSION_CASE, SEMA_TYPE_LONG, SEMA_TYPE_INT);
    label = sema_lookup_switch_label(snippet.result, otherwise);
    check_test(label && label->switch_statement == selection && !label->value_type,
               "default records the same owning switch without a value");
    free_snippet(&snippet);
}

// Verify controlling scopes, independent errors and per-switch constant keys.
static void test_control_constraints(void)
{
    check_failed_snippet("int f(void) { for (int i = 0; i < 1; ++i) ; return i; }", SEMA_INVALID,
                         1);
    check_failed_snippet("int f(void) { do { int i = 0; } while (i); return 0; }", SEMA_INVALID, 1);
    check_failed_snippet("typedef int F(void); int f(void) { for (F g; ; ) break; return 0; }",
                         SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { if ((void)0) break; else continue; return 0; }",
                         SEMA_INVALID, 3);
    check_failed_snippet(
        "int f(unsigned x) { switch (x) { case -1: ; case 4294967295u: ; } return 0; }",
        SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { switch (1.0) { default: ; default: ; } return 0; }",
                         SEMA_INVALID, 2);
    check_failed_snippet("int f(int n) { switch (n) { case n: ; case 1 / 0: ; } return 0; }",
                         SEMA_INVALID, 2);
    check_failed_snippet("int f(int n) { switch (n) { case (1, 2): ; } return 0; }", SEMA_INVALID,
                         1);
    check_failed_snippet("int f(void) { case 1: ; default: ; return 0; }", SEMA_INVALID, 2);

    AnalyzedSnippet snippet =
        analyze_snippet("int f(int n) { if (n) switch (n) {"
                        "case (int)1.5: switch (n) { case 1: ; default: ; }"
                        "default: while (n) { case 0 && (1 / 0, 2): break; } } return 0; }");
    check_test(snippet.status == SEMA_OK && snippet.errors == 0,
               "nested switches have independent cases and skip unevaluated operands");
    const Node *fn = snippet.root->u.program.external_declarations;
    const Node *branch = fn->u.func.body->u.block.items;
    const SemaConditionInfo *condition = sema_lookup_condition(snippet.result, branch);
    check_test(condition && condition->expression == branch->u.if_stmt.cond,
               "a scalar control records its source expression");
    free_snippet(&snippet);
}

// ---------------------------------------------------------------------------
// Fixed arrays and tagged types
// ---------------------------------------------------------------------------

// Verify array designators, decay and fixed parameter adjustment independently.
static void test_fixed_array_conversions(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("int a[sizeof(struct T { int x; })][sizeof(struct T)];"
                        "int pending[]; extern int later[]; int later[2];"
                        "int f(int p[static const 4]) { sizeof a; &a; a + 1; return p[0]; }");
    check_test(snippet.status == SEMA_OK && !snippet.errors, "fixed array declarations succeed");
    const Node *first = snippet.root->u.program.external_declarations;
    const SemaSymbol *array = sema_lookup_symbol(snippet.result, first->u.decl.declarators);
    check_test(array->type->kind == SEMA_TYPE_ARRAY && array->type->u.array.count == 4 &&
                   array->type->u.array.element->u.array.count == 4,
               "both bounds resolve the tag in source order");
    const SemaSymbol *pending = sema_lookup_symbol(snippet.result, first->next->u.decl.declarators);
    check_test(!pending->type->u.array.count && pending->entity->type->u.array.count == 1,
               "translation-unit completion preserves the incomplete declaration snapshot");
    const Node *fn = first->next->next->next->next;
    const Declarator *layer = fn->u.func.declarator;
    const SemaSymbol *parameter =
        sema_lookup_symbol(snippet.result, layer->u.function.clause.prototype.parameters);
    check_test(parameter->type->kind == SEMA_TYPE_POINTER &&
                   parameter->type->qualifiers == TYPE_QUAL_CONST &&
                   parameter->declared_type->kind == SEMA_TYPE_ARRAY &&
                   parameter->u.object.minimum_array_count == 4,
               "array adjustment keeps the original bound and the bracket qualifiers");
    const Node *item = fn->u.func.body->u.block.items;
    const Node *size = item->u.expr.expr;
    const Node *address = item->next->u.expr.expr;
    const Node *add = item->next->next->u.expr.expr;
    check_test(
        sema_lookup_expression(snippet.result, size)->u.sizeof_value == 64 &&
            !sema_lookup_expression(snippet.result, size->u.unary.operand)->conversions.count,
        "sizeof sees the whole array");
    check_test(
        !sema_lookup_expression(snippet.result, address->u.unary.operand)->conversions.count &&
            sema_lookup_expression(snippet.result, address)->original.type->u.pointee ==
                array->type,
        "address-of points to the whole array");
    const SemaExprInfo *operand = sema_lookup_expression(snippet.result, add->u.binary.lhs);
    check_conversion(operand, 0, SEMA_CONVERSION_ARRAY_TO_POINTER, SEMA_TYPE_ARRAY,
                     SEMA_TYPE_POINTER);
    check_test(sema_converted_properties(operand).type->u.pointee == array->type->u.array.element,
               "array addition points to the first row");
    free_snippet(&snippet);
}

// Verify nominal completion, anonymous member layout and bit-field promotion.
static void test_aggregate_types(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("struct S;"
                        "struct S { char c; union { int x; long y; }; unsigned :0; unsigned b:3; };"
                        "struct Link { struct Link *next; int values[]; };"
                        "int f(const struct S *p, struct Link *q) { sizeof(struct Link); p->x; "
                        "q->values; return p->b + 1; }");
    check_test(snippet.status == SEMA_OK && !snippet.errors, "aggregate types succeed");
    const Node *fn = snippet.root->u.program.external_declarations->next->next->next;
    const Node *body = fn->u.func.body->u.block.items;
    const SemaExprInfo *size = sema_lookup_expression(snippet.result, body->u.expr.expr);
    check_test(size->u.sizeof_value == 8, "flexible array contributes alignment but no elements");
    const SemaExprInfo *member = sema_lookup_expression(snippet.result, body->next->u.expr.expr);
    check_test(member->kind == SEMA_EXPR_MEMBER && member->u.member.field &&
                   member->u.member.offset == 8 &&
                   member->original.type->qualifiers == TYPE_QUAL_CONST &&
                   member->original.category == SEMA_LVALUE,
               "anonymous lookup accumulates offset and aggregate qualifiers");
    const SemaExprInfo *flexible =
        sema_lookup_expression(snippet.result, body->next->next->u.expr.expr);
    check_test(flexible->u.member.field &&
                   flexible->u.member.field->type->kind == SEMA_TYPE_ARRAY &&
                   !flexible->u.member.field->type->u.array.count && flexible->u.member.offset == 8,
               "an incomplete flexible array is visible to member access");
    const Node *add = body->next->next->next->u.expr.expr;
    const SemaExprInfo *field = sema_lookup_expression(snippet.result, add->u.binary.lhs);
    const Node *base = add->u.binary.lhs->u.member_access.base;
    const SemaType *pointer =
        sema_converted_properties(sema_lookup_expression(snippet.result, base)).type;
    const SemaMember *anonymous = pointer->u.pointee->u.tag->u.aggregate.members->next;
    const SemaMember *separator = anonymous->next;
    check_test(anonymous->kind == SEMA_MEMBER_ANONYMOUS &&
                   separator->kind == SEMA_MEMBER_BIT_FIELD && !separator->u.bit_field.width,
               "the anonymous aggregate and the unnamed zero-width bit-field have distinct kinds");
    check_test(field->original.bit_field == field->u.member.field &&
                   field->u.member.field->kind == SEMA_MEMBER_BIT_FIELD &&
                   field->u.member.field->u.bit_field.width == 3,
               "a bit-field retains source member identity and width");
    check_conversion(field, 1, SEMA_CONVERSION_INTEGER_PROMOTION, SEMA_TYPE_UINT, SEMA_TYPE_INT);
    check_test(field->conversions.steps[0].from.bit_field == field->u.member.field &&
                   field->conversions.steps[0].to.bit_field == field->u.member.field &&
                   field->conversions.steps[1].from.bit_field == field->u.member.field &&
                   !field->conversions.steps[1].to.bit_field,
               "the bit-field identity flows through the read and into integer promotion");
    free_snippet(&snippet);
}

// Check definition parameter scope and enum binding after the initializer.
static void test_enumerator_scope(void)
{
    AnalyzedSnippet snippet = analyze_snippet(
        "typedef long T; enum { OUTER = 2 };"
        "int f(enum { T = sizeof(T), E = OUTER + 1 } p) {"
        "int data[T]; return sizeof(T) + E + sizeof data; }"
        "enum Rep { MINIMUM = -1 };"
        "_Static_assert(sizeof(enum Rep) == sizeof(int) && _Alignof(enum Rep) == _Alignof(int),"
        " \"enum layout\");");
    check_test(snippet.status == SEMA_OK && !snippet.errors, "enumerator scope succeeds");
    const Node *fn = snippet.root->u.program.external_declarations->next->next;
    const Node *data = fn->u.func.body->u.block.items->u.decl.declarators;
    const SemaSymbol *array = sema_lookup_symbol(snippet.result, data);
    check_test(array->type->u.array.count == 8, "the enumerator value uses the prior typedef");
    const Node *sum = fn->u.func.body->u.block.items->next->u.expr.expr;
    const Node *size = sum->u.binary.lhs->u.binary.lhs;
    check_test(size->type == ND_UNARY &&
                   sema_lookup_expression(snippet.result, size)->u.sizeof_value == 4,
               "the definition parameter enum hides the outer typedef in the body");
    free_snippet(&snippet);
}

// Recover independent explicit values and preserve the failed enum names' scope.
static void test_enumerator_recovery(void)
{
    check_failed_snippet("enum { B = 8 }; void f(void) {"
                         "enum E { A = missing_a, B, C = 1 / (B - 8), D = missing_b, E0 = 4, F };"
                         "_Static_assert(F == 5, \"resumed sequence\"); }",
                         SEMA_INVALID, 2);
    check_failed_snippet("enum { A = 2147483647, B, C, D = missing };", SEMA_INVALID, 2);
}

// Verify independent constraints on array bounds, tags and member expressions.
static void test_fixed_type_constraints(void)
{
    check_failed_snippet("int a[0];", SEMA_INVALID, 1);
    check_failed_snippet("int a[1.0];", SEMA_INVALID, 1);
    check_failed_snippet("struct S; struct S a[2];", SEMA_INVALID, 1);
    check_failed_snippet("struct S { struct S self; };", SEMA_INVALID, 1);
    check_failed_snippet("struct S { int a; int a; };", SEMA_INVALID, 1);
    check_failed_snippet("struct S { unsigned a:3; }; int f(struct S s) { return sizeof s.a; }",
                         SEMA_INVALID, 1);
    check_failed_snippet("enum E; ", SEMA_INVALID, 1);
    check_failed_snippet("enum { E = 2147483647, F };", SEMA_INVALID, 1);
    check_failed_snippet("int f(int a[*]);", SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { int a[2] = {1, 2}; return a[1.5]; }", SEMA_INVALID, 1);
    check_failed_snippet("#include \"external.h\"\nint f(void) { return shared.member; }",
                         SEMA_INVALID, 1);
    check_failed_snippet("#include \"external.h\"\nint f(void) { return sizeof(struct Shared); }",
                         SEMA_INVALID, 1);
}

// Check bit-field width constraints without requiring a valid member type.
static void test_bit_field_width_recovery(void)
{
    check_failed_snippet("struct S { float field : -1; };", SEMA_INVALID, 2);
    check_failed_snippet("struct S { _Atomic(const int) field : 0; };", SEMA_INVALID, 2);
    check_failed_snippet("struct S { float field : 1000; };", SEMA_INVALID, 1);
    check_failed_snippet("struct S { unsigned field : 1000; };", SEMA_INVALID, 1);
}

// ---------------------------------------------------------------------------
// Initializers and declaration attributes
// ---------------------------------------------------------------------------

// Verify sparse placement, exact-width strings and completion of named arrays.
static void test_initializer_placement(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("int a[][2] = {1, 2, [2][1] = 7}; char s[3] = \"abc\";"
                        "int f(void) { int *self[] = {(int *)self}; return ((int[]){2, 3})[1]; }");
    check_test(snippet.status == SEMA_OK && !snippet.errors,
               "array and literal initializers succeed");
    const Node *first = snippet.root->u.program.external_declarations;
    const Node *decl = first->u.decl.declarators;
    const SemaInitializer *init = sema_lookup_initializer(snippet.result, decl);
    check_test(init && init->type->u.array.count == 3 && init->count == 4 &&
                   init->steps[0].kind == SEMA_INITIALIZER_ZERO && init->steps[1].offset == 0 &&
                   init->steps[2].offset == 4 && init->steps[3].offset == 20,
               "brace elision and nested designators select sparse offsets");
    check_test(sema_lookup_symbol(snippet.result, decl)->entity->type == init->type,
               "the named object receives the inferred array length");
    init = sema_lookup_initializer(snippet.result, first->next->u.decl.declarators);
    check_test(init && init->steps[1].kind == SEMA_INITIALIZER_STRING &&
                   init->steps[1].u.string.literal->type == ND_STRING &&
                   init->steps[1].u.string.bytes == 3,
               "an exact string bound omits the terminator");
    const Node *local = first->next->next->u.func.body->u.block.items->u.decl.declarators;
    init = sema_lookup_initializer(snippet.result, local);
    check_test(init && init->steps[1].kind == SEMA_INITIALIZER_EXPRESSION,
               "the completed array retains the initializer's expression step");
    const Node *self = init->steps[1].u.expression.operand->u.cast.expr;
    const SemaSymbol *prior = sema_lookup_expression(snippet.result, self)->u.symbol;
    const SemaSymbol *completed = sema_lookup_symbol(snippet.result, local);
    check_test(prior && prior != completed && !prior->type->u.array.count &&
                   completed->type->u.array.count == 1 && prior->entity == completed->entity,
               "an initializer reference retains the incomplete lexical type");
    const Node *subscript = first->next->next->u.func.body->u.block.items->next->u.expr.expr;
    const Node *literal = subscript->u.subscript.base;
    check_test(sema_lookup_initializer(snippet.result, literal)->type->u.array.count == 2,
               "a compound literal completes its own array before decay");
    free_snippet(&snippet);

    check_failed_snippet("int a[2] = {[2] = 1};", SEMA_INVALID, 1);
    check_failed_snippet("struct P {int x;}; struct P p = {.y = 1};", SEMA_INVALID, 1);
    check_failed_snippet("struct P {int x, tail[];}; struct P p = {.tail = {1}};", SEMA_INVALID, 1);
    check_failed_snippet("int a; int b[2] = {a};", SEMA_INVALID, 1);
    check_failed_snippet("int a = (int)2e308;", SEMA_INVALID, 1);
    check_failed_snippet("_Static_assert(0, \"zero\");", SEMA_INVALID, 1);
}

// Recover source expressions and resume typed placement at known subobjects.
static void test_initializer_sibling_recovery(void)
{
    check_failed_snippet("int a[2] = {missing_a, missing_b};", SEMA_INVALID, 2);
    check_failed_snippet("int a[1] = {0, missing_a, missing_b};", SEMA_INVALID, 3);
    check_failed_snippet("int a = {{missing_a, missing_b}};", SEMA_INVALID, 3);
    check_failed_snippet("int a[2][1] = {missing_a, (int *)0};", SEMA_INVALID, 2);
    check_failed_snippet("struct S { int a[2]; int b; };"
                         "struct S s = {.a[3][missing_index] = {missing_value}, .b = (int *)0};",
                         SEMA_INVALID, 4);
    check_failed_snippet(
        "struct Child { int member; }; struct Outer { struct Child child; int x; };"
        "struct Outer o = {missing_a, (void *)0, .x = (int *)0};",
        SEMA_INVALID, 2);
    check_failed_snippet("struct S { int a; }; struct S s = {.missing[1.5] = 0};", SEMA_INVALID, 2);
    check_failed_snippet("struct S { int a; }; struct S s = {.missing[-1] = 0};", SEMA_INVALID, 2);
    check_failed_snippet("struct S { int a; }; struct S s = {.missing[100] = 0};", SEMA_INVALID, 1);
}

// Commit an inferred bound only when all declarations of the entity agree.
static void test_linked_array_completion(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("void prior(void) { extern int a[2]; } int a[] = {1, 2};");
    check_test(snippet.status == SEMA_OK && !snippet.errors,
               "the inferred bound agrees with the entity");
    const Node *first = snippet.root->u.program.external_declarations;
    const Node *local = first->u.func.body->u.block.items->u.decl.declarators;
    const Node *definition = first->next->u.decl.declarators;
    const SemaSymbol *prior = sema_lookup_symbol(snippet.result, local);
    const SemaSymbol *symbol = sema_lookup_symbol(snippet.result, definition);
    const SemaInitializer *initializer = sema_lookup_initializer(snippet.result, definition);
    check_test(prior->entity == symbol->entity && prior->type->u.array.count == 2 &&
                   symbol->type == initializer->type && symbol->type->u.array.count == 2 &&
                   !symbol->declared_type->u.array.count &&
                   symbol->entity->type->u.array.count == 2 &&
                   symbol->entity->definition_node == definition,
               "the two declarations and the initializer retain compatible types");
    free_snippet(&snippet);

    check_failed_snippet("void f(void) { extern int a[2]; } int a[] = {1, 2, 3};"
                         "int a[2] = {4, 5};",
                         SEMA_INVALID, 1);
    check_failed_snippet("void f(void) { extern char a[2]; } char a[] = \"ab\";", SEMA_INVALID, 1);
    check_failed_snippet("int a[2]; int a[] = {1, 2, 3}; int a[2] = {4, 5};", SEMA_INVALID, 1);
}

// Keep target infinities valid in floating static initializers.
static void test_floating_initializer_overflow(void)
{
    AnalyzedSnippet snippet = analyze_snippet("double positive = 2e308; float negative = -2e39f;");
    check_test(snippet.status == SEMA_OK && !snippet.errors,
               "floating constant overflow can produce a target infinity");
    free_snippet(&snippet);
}

// Verify that replacing an aggregate or switching union members resets its subobject.
static void test_initializer_overrides(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("union U { struct {int x, y;} a; struct {int p, q;} b; };"
                        "union U u = {.a.x=1, .a.y=2, .b.p=3, .b.q=4};");
    check_test(snippet.status == SEMA_OK && !snippet.errors, "union designators succeed");
    const Node *decl = snippet.root->u.program.external_declarations->next->u.decl.declarators;
    const SemaInitializer *init = sema_lookup_initializer(snippet.result, decl);
    check_test(init && init->count >= 5, "the union retains its placement sequence");
    size_t final = init->count - 1;
    check_test(init->steps[final - 2].kind == SEMA_INITIALIZER_ZERO &&
                   init->steps[final - 1].offset == 0 && init->steps[final].offset == 4,
               "switching union members clears the old member and preserves the new fields");
    free_snippet(&snippet);

    snippet = analyze_snippet(
        "struct S { char pad; union { struct { char c; int x, y; }; long other; }; int end; };"
        "struct S s = {.x=1, 2, .other=3, .y=4, .end=5};");
    check_test(snippet.status == SEMA_OK && !snippet.errors,
               "nested anonymous designators succeed");
    decl = snippet.root->u.program.external_declarations->next->u.decl.declarators;
    init = sema_lookup_initializer(snippet.result, decl);
    check_test(init && init->count == 9 && init->steps[1].kind == SEMA_INITIALIZER_ZERO &&
                   init->steps[1].offset == 8 && init->steps[2].offset == 12 &&
                   init->steps[3].offset == 16 && init->steps[4].kind == SEMA_INITIALIZER_ZERO &&
                   init->steps[5].offset == 8 && init->steps[6].kind == SEMA_INITIALIZER_ZERO &&
                   init->steps[7].offset == 16 && init->steps[8].offset == 24,
               "the route keeps positional continuation and records each union reset in order");
    free_snippet(&snippet);
}

// Keep association expression and type checks independent of earlier failures.
static void test_generic_recovery(void)
{
    check_failed_snippet("int f(void) { return _Generic(0, int: missing_a, default: missing_b); }",
                         SEMA_INVALID, 2);
    check_failed_snippet("int f(void) { return _Generic(0, void: missing_a, default: missing_b); }",
                         SEMA_INVALID, 3);
    check_failed_snippet("int f(void) { return _Generic(missing_a, int: missing_b); }",
                         SEMA_INVALID, 2);
}

// Keep selected-expression identities, target query values and storage facts in their own variants.
static void test_expression_payloads(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("int target(void) { return 0; }"
                        "int (*p)(void) = _Generic(0, int: _Generic(0, int: target));"
                        "int object; int *q = &_Generic(0, int: object);"
                        "int *r = &(int){7};"
                        "int measure(void) { return sizeof(int) + _Alignof(int); }");
    check_test(snippet.status == SEMA_OK && !snippet.errors,
               "selected function, selected object and compound-literal addresses are constant");
    const Node *target = snippet.root->u.program.external_declarations;
    const Node *literal = target->u.func.body->u.block.items->u.expr.expr;
    const SemaExprInfo *value = sema_lookup_expression(snippet.result, literal);
    check_test(value->kind == SEMA_EXPR_PLAIN && !value->conversions.count,
               "a literal contains only common semantic facts");

    const Node *p = target->next;
    const Node *generic = p->u.decl.declarators->u.init_decl.init;
    const SemaExprInfo *outer = sema_lookup_expression(snippet.result, generic);
    check_test(outer->kind == SEMA_EXPR_GENERIC_SELECTION && outer->u.selected,
               "the generic expression retains its selected node");
    const SemaExprInfo *inner = sema_lookup_expression(snippet.result, outer->u.selected);
    check_test(inner->kind == SEMA_EXPR_GENERIC_SELECTION && inner->u.selected,
               "a nested selection retains a separate source record");
    const SemaExprInfo *identifier = sema_lookup_expression(snippet.result, inner->u.selected);
    check_test(identifier->kind == SEMA_EXPR_IDENTIFIER &&
                   identifier->u.symbol == sema_lookup_symbol(snippet.result, target),
               "the identifier variant retains the resolved declaration");

    const Node *r = p->next->next->next;
    const Node *address = r->u.decl.declarators->u.init_decl.init;
    const SemaExprInfo *compound = sema_lookup_expression(snippet.result, address->u.unary.operand);
    check_test(compound->kind == SEMA_EXPR_COMPOUND_LITERAL && compound->u.static_duration,
               "a file-scope compound literal publishes its storage duration");
    const Node *sum = r->next->u.func.body->u.block.items->u.expr.expr;
    const SemaExprInfo *size = sema_lookup_expression(snippet.result, sum->u.binary.lhs);
    const SemaExprInfo *alignment = sema_lookup_expression(snippet.result, sum->u.binary.rhs);
    check_test(size->kind == SEMA_EXPR_SIZEOF && size->u.sizeof_value == 4 &&
                   alignment->kind == SEMA_EXPR_ALIGNOF && alignment->u.alignof_value == 4,
               "each type query publishes its own target value");
    free_snippet(&snippet);
}

// Verify recorded alignment, storage duration, generic selection and atomic operations.
static void test_c11_declaration_attributes(void)
{
    AnalyzedSnippet snippet =
        analyze_snippet("_Alignas(16) _Thread_local _Atomic(int) tls = 1;"
                        "struct S { char c; _Alignas(16) int n; _Static_assert(1, \"ok\"); };"
                        "_Static_assert(_Alignof(struct S) == 16, \"alignment\");"
                        "inline int identity(int n) { return n; }"
                        "int f(void) { _Atomic int a = 1; const int c = 2;"
                        "_Generic(c, int: a, default: tls) = 3; a++; return a; }");
    check_test(snippet.status == SEMA_OK && !snippet.errors, "C11 declaration attributes succeed");
    const Node *first = snippet.root->u.program.external_declarations;
    const SemaSymbol *tls = sema_lookup_symbol(snippet.result, first->u.decl.declarators);
    check_test(tls->entity->u.object.duration == SEMA_DURATION_THREAD &&
                   tls->entity->u.object.alignment == 16 &&
                   tls->type->qualifiers & TYPE_QUAL_ATOMIC,
               "thread duration and alignment are distinct from the atomic value type");
    const Node *inlined = first->next->next->next;
    check_test(sema_lookup_symbol(snippet.result, inlined)->entity->u.function.is_inline_definition,
               "an external inline definition is recognized at translation-unit end");
    const Node *statement = inlined->next->u.func.body->u.block.items->next->next;
    const Node *assign = statement->u.expr.expr;
    const SemaExprInfo *selection = sema_lookup_expression(snippet.result, assign->u.assign.lhs);
    const SemaExprInfo *store = sema_lookup_expression(snippet.result, assign);
    check_test(selection && selection->kind == SEMA_EXPR_GENERIC_SELECTION &&
                   selection->u.selected && selection->original.category == SEMA_LVALUE &&
                   store->kind == SEMA_EXPR_ASSIGNMENT && store->u.atomic_store,
               "generic selection retains its selected atomic designator");
    const SemaExprInfo *update =
        sema_lookup_expression(snippet.result, statement->next->u.expr.expr);
    check_test(update->kind == SEMA_EXPR_UPDATE && update->u.update &&
                   update->u.update->flags ==
                       (SEMA_UPDATE_ATOMIC | SEMA_UPDATE_RETURNS_PREVIOUS_VALUE) &&
                   update->u.update->target_read.steps[0].kind == SEMA_CONVERSION_ATOMIC_LOAD,
               "an atomic postfix update reads atomically and returns the previous value");
    const SemaExprInfo *load =
        sema_lookup_expression(snippet.result, statement->next->next->u.expr.expr);
    check_conversion(load, 0, SEMA_CONVERSION_ATOMIC_LOAD, SEMA_TYPE_INT, SEMA_TYPE_INT);
    free_snippet(&snippet);

    check_failed_snippet("_Alignas(2) int x;", SEMA_INVALID, 1);
    check_failed_snippet("extern _Alignas(16) int x; int x = 1;", SEMA_INVALID, 1);
    check_failed_snippet("extern _Thread_local int x; int x;", SEMA_INVALID, 1);
    check_failed_snippet("_Atomic(const int) x;", SEMA_INVALID, 1);
    check_failed_snippet("int x; _Atomic int *p = &x;", SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { return _Generic(1, int: 1, signed: 2); }", SEMA_INVALID, 1);
    check_failed_snippet(
        "enum A {A}; enum B {B}; int f(void) { return _Generic(1u, enum A: 1, enum B: 2); }",
        SEMA_INVALID, 1);
    check_failed_snippet("int f(void) { return _Generic(1, int: 1, default: unknown); }",
                         SEMA_INVALID, 1);
    check_failed_snippet("static int x; inline int f(void) { return x; }", SEMA_INVALID, 1);
    check_failed_snippet("inline int f(void) { static int x; return x; }", SEMA_INVALID, 1);
    snippet = analyze_snippet("typedef int Function(void); inline Function f; extern int f(void);"
                              "static int x; inline int f(void) { return x; }");
    check_test(
        snippet.status == SEMA_OK && !snippet.errors,
        "a typedef can declare an inline function and extern selects an external definition");
    const Node *definition = snippet.root->u.program.external_declarations->next->next->next->next;
    check_test(
        !sema_lookup_symbol(snippet.result, definition)->entity->u.function.is_inline_definition,
        "only an inline-only definition carries the inline linkage restriction");
    free_snippet(&snippet);
}

// ---------------------------------------------------------------------------
// Test driver
// ---------------------------------------------------------------------------

// Validate a source file through the semantic entry and release its inputs.
static TestOutcome test_source_file(const char *path)
{
    Token *tokens = tokenize_file(path);
    if (!tokens) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return TEST_DRIVER_ERROR;
    }
    SourceFile *source = current_source();
    Node *root = error_count() ? NULL : parse(tokens);
    SemaResult *result = NULL;
    TestOutcome outcome = TEST_SYNTAX_ERROR;
    if (root && !error_count()) {
        switch (sema_analyze(root, &result)) {
        case SEMA_OK:
            outcome = TEST_ACCEPTED;
            break;
        case SEMA_INVALID:
            outcome = TEST_SEMANTIC_ERROR;
            break;
        case SEMA_UNSUPPORTED:
            outcome = TEST_UNSUPPORTED;
            break;
        }
    }
    sema_free(result);
    parse_free(root);
    token_free(tokens);
    source_free(source);
    return outcome;
}

// Validate a file or run the semantic unit tests.
int main(int argc, char **argv)
{
    if (argc == 2)
        return test_source_file(argv[1]);
    if (argc != 1) {
        fprintf(stderr, "usage: %s [source-file]\n", argv[0]);
        return TEST_DRIVER_ERROR;
    }

    test_integer_return();
    test_local_conversions();
    test_declaration_point();
    test_invalid_programs();
    test_unsupported_programs();
    test_recovery_status_precedence();
    test_declarator_sibling_recovery();
    test_variable_type_constraints();
    test_failed_type_children();
    test_failed_translation_unit_finalization();
    test_arithmetic_conversions();
    test_linked_entities();
    test_call_snapshots();
    test_empty_function_definition();
    test_pointer_initializers();
    test_rejected_assignment_conversion();
    test_common_pointer_conversions();
    test_function_definition_recovery();
    test_register_subobject_address();
    test_stage_two_constraints();
    test_update_conversions();
    test_invalid_updates();
    test_goto_targets();
    test_switch_targets();
    test_control_constraints();
    test_fixed_array_conversions();
    test_aggregate_types();
    test_enumerator_scope();
    test_enumerator_recovery();
    test_fixed_type_constraints();
    test_bit_field_width_recovery();
    test_initializer_placement();
    test_initializer_sibling_recovery();
    test_linked_array_completion();
    test_floating_initializer_overflow();
    test_initializer_overrides();
    test_generic_recovery();
    test_expression_payloads();
    test_c11_declaration_attributes();
    puts("all semantic checks passed");
    return 0;
}
#endif
