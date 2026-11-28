// Resolve statement constraints and source-level targets in one function.

#include "sema_internal.h"

#include "util/diagnostic.h"

#include <stdlib.h>

typedef enum {
    RETURN_VALUE_VALID,
    RETURN_VALUE_UNEXPECTED,
    RETURN_VALUE_MISSING,
} ReturnValueIssue;

typedef enum {
    STATEMENT_NONSCALAR_CONDITION,
    STATEMENT_NONINTEGER_SWITCH,
    STATEMENT_INVALID_BREAK,
    STATEMENT_INVALID_CONTINUE,
    STATEMENT_ORPHAN_CASE,
    STATEMENT_ORPHAN_DEFAULT,
    STATEMENT_NONCONSTANT_CASE,
    STATEMENT_UNEVALUABLE_CASE,
} StatementIssue;

typedef struct PendingGoto {
    const Node *node;
    struct PendingGoto *next;
} PendingGoto;

typedef struct {
    const Node *node;
    const SemaType *comparison_type;
    HashMap cases;
    const Node *default_label;
} SwitchContext;

typedef struct ControlFrame {
    const Node *node;
    SwitchContext *selection;
    const struct ControlFrame *outer;
} ControlFrame;

typedef struct {
    SemaContext *sema;
    const SemaType *return_type;
    unsigned function_specifiers;
    HashMap labels;
    PendingGoto *first_goto;
    PendingGoto **next_goto;
    const ControlFrame *control;
    bool labels_complete;
} StatementContext;

static SemaStatus analyze_statement(StatementContext *ctx, const Node *node, bool controlled);
static SemaStatus analyze_block_items(StatementContext *ctx, const Node *items);

// ---------------------------------------------------------------------------
// Function and control state
// ---------------------------------------------------------------------------

// Initialize statement-owned state for a single function body.
static void init_statement_context(StatementContext *ctx, SemaContext *sema,
                                   const SemaType *return_type, unsigned function_specifiers)
{
    *ctx = (StatementContext){ .sema = sema, .return_type = return_type,
                             .function_specifiers = function_specifiers,
                             .labels_complete = true };
    ctx->next_goto = &ctx->first_goto;
    hashmap_init(&ctx->labels, HASHMAP_KEY_STRING_VIEW);
}

// Release the function indices and pending queue. The AST remains borrowed.
static void free_statement_context(StatementContext *ctx)
{
    while (ctx->first_goto) {
        PendingGoto *pending = ctx->first_goto;
        ctx->first_goto = pending->next;
        free(pending);
    }
    hashmap_free(&ctx->labels);
}

// Look up a name in this function's label namespace.
static const Node *lookup_label(const StatementContext *ctx, StringView name)
{
    return hashmap_get(&ctx->labels, name);
}

// Index a checked label definition under its borrowed name.
static void bind_label(StatementContext *ctx, const Node *label)
{
    hashmap_put(&ctx->labels, label->u.label.name, (void *)label);
}

// Queue a goto in source order while borrowing its AST node.
static void attach_pending_goto(StatementContext *ctx, const Node *node)
{
    PendingGoto *pending = xmalloc(sizeof(*pending));
    *pending = (PendingGoto){ .node = node };
    *ctx->next_goto = pending;
    ctx->next_goto = &pending->next;
}

// Initialize the label index for one switch. A failed control type is NULL.
static void init_switch_context(SwitchContext *ctx, const Node *node,
                                const SemaType *comparison_type)
{
    *ctx = (SwitchContext){ .node = node, .comparison_type = comparison_type };
    hashmap_init(&ctx->cases, HASHMAP_KEY_UINT64);
}

// Release the switch's borrowed label index on exit from the statement.
static void free_switch_context(SwitchContext *ctx)
{
    hashmap_free(&ctx->cases);
}

// Enter a loop or switch body with a frame whose lifetime belongs to the caller.
static void push_control_frame(StatementContext *ctx, ControlFrame *frame,
                               const Node *node, SwitchContext *selection)
{
    *frame = (ControlFrame){ .node = node, .selection = selection, .outer = ctx->control };
    ctx->control = frame;
}

// Restore the enclosing control frame after analyzing the body.
static void pop_control_frame(StatementContext *ctx)
{
    ctx->control = ctx->control->outer;
}

// Read the innermost breakable statement.
static const Node *lookup_break_target(const StatementContext *ctx)
{
    return ctx->control ? ctx->control->node : NULL;
}

// Read the innermost iteration statement, even across nested switches.
static const Node *lookup_continue_target(const StatementContext *ctx)
{
    for (const ControlFrame *frame = ctx->control; frame; frame = frame->outer)
        if (!frame->selection)
            return frame->node;
    return NULL;
}

// Read the innermost switch, even across nested loops.
static SwitchContext *lookup_enclosing_switch(const StatementContext *ctx)
{
    for (const ControlFrame *frame = ctx->control; frame; frame = frame->outer)
        if (frame->selection)
            return frame->selection;
    return NULL;
}

// Read a previously bound case using its converted target representation.
static const Node *lookup_case(const SwitchContext *ctx, uint64_t bits)
{
    return hashmap_get_uint64(&ctx->cases, bits);
}

// Index a checked case under its already converted value.
static void bind_case(SwitchContext *ctx, uint64_t bits, const Node *node)
{
    hashmap_put_uint64(&ctx->cases, bits, (void *)node);
}

// Record the checked default label of this switch.
static void bind_default(SwitchContext *ctx, const Node *node)
{
    ctx->default_label = node;
}

// ---------------------------------------------------------------------------
// Checks and diagnostics
// ---------------------------------------------------------------------------

// Check whether a statement already introduces a C11 block.
static bool check_statement_block(NodeType kind)
{
    switch (kind) {
    case ND_BLOCK:
    case ND_IF:
    case ND_SWITCH:
    case ND_WHILE:
    case ND_DO_WHILE:
    case ND_FOR:
        return true;
    default:
        return false;
    }
}

// Compare the presence of a return operand with the definition's return type.
static ReturnValueIssue check_return_value(const SemaType *type, bool has_value)
{
    if (type->kind == SEMA_TYPE_VOID)
        return has_value ? RETURN_VALUE_UNEXPECTED : RETURN_VALUE_VALID;
    return has_value ? RETURN_VALUE_VALID : RETURN_VALUE_MISSING;
}

// Anchor a return-value constraint violation at its statement.
static SemaStatus report_invalid_return(const SourceSpan *span, ReturnValueIssue issue)
{
    switch (issue) {
    case RETURN_VALUE_UNEXPECTED:
        error_at(span, "'return' with a value in a function returning void");
        break;
    case RETURN_VALUE_MISSING:
        error_at(span, "'return' with no value in a function returning non-void");
        break;
    case RETURN_VALUE_VALID:
        unreachable();
    }
    return SEMA_INVALID;
}

// Warn about an explicit path returning from a function declared _Noreturn.
static void report_noreturn_return(const SourceSpan *span)
{
    warn_at(span, "function declared '_Noreturn' has a return statement");
}

// Render the typed violation at the responsible statement or operand.
static SemaStatus report_statement_issue(const SourceSpan *span, StatementIssue issue)
{
    static const char *const g_messages[] = {
        [STATEMENT_NONSCALAR_CONDITION] = "controlling expression must have scalar type",
        [STATEMENT_NONINTEGER_SWITCH] = "switch quantity not an integer",
        [STATEMENT_INVALID_BREAK] = "break statement not within loop or switch",
        [STATEMENT_INVALID_CONTINUE] = "continue statement not within a loop",
        [STATEMENT_ORPHAN_CASE] = "case label not within a switch statement",
        [STATEMENT_ORPHAN_DEFAULT] = "'default' label not within a switch statement",
        [STATEMENT_NONCONSTANT_CASE] = "case label does not reduce to an integer constant",
        [STATEMENT_UNEVALUABLE_CASE] = "case constant expression cannot be evaluated",
    };
    error_at(span, "%s", g_messages[issue]);
    return SEMA_INVALID;
}

// Report an unresolved goto in the function containing the use.
static SemaStatus report_undefined_label(const SourceSpan *span, StringView name)
{
    error_at(span, "label '%.*s' used but not defined", (int)string_view_length(name),
             string_view_begin(name));
    return SEMA_INVALID;
}

// Report a repeated label and identify its first definition.
static SemaStatus report_duplicate_label(const SourceSpan *span, StringView name,
                                         const SourceSpan *previous)
{
    error_at(span, "duplicate label '%.*s' (first defined at %u:%u)",
             (int)string_view_length(name), string_view_begin(name),
             previous->line, previous->column);
    return SEMA_INVALID;
}

// Report the repeated switch label with the first matching source location.
static SemaStatus report_duplicate_switch_label(const SourceSpan *span,
                                                const SourceSpan *previous, NodeType kind)
{
    if (kind == ND_DEFAULT)
        error_at(span, "multiple default labels in one switch (first at %u:%u)",
                 previous->line, previous->column);
    else
        error_at(span, "duplicate case value (first at %u:%u)", previous->line, previous->column);
    return SEMA_INVALID;
}

// Report the reason the typed case operand could not produce a constant.
static SemaStatus report_invalid_case(const SourceSpan *span, SemaIntegerConstantStatus status)
{
    StatementIssue issue = status == SEMA_INTEGER_CONSTANT_INVALID_OPERANDS
        ? STATEMENT_NONCONSTANT_CASE : STATEMENT_UNEVALUABLE_CASE;
    return report_statement_issue(span, issue);
}

// ---------------------------------------------------------------------------
// Basic statements and conditional or iteration statements
// ---------------------------------------------------------------------------

// Analyze a scalar condition, recording its read and nonzero test on success.
static SemaStatus analyze_condition(StatementContext *ctx, const Node *stmt, const Node *expr)
{
    SemaResult *result = ctx->sema->result;
    if (expr) {
        SemaStatus status = analyze_expression(ctx->sema, expr);
        if (status != SEMA_OK)
            return status;
        const SemaExprInfo *info = sema_lookup_expression(result, expr);
        SemaConversionPlan value = convert_value(result, stmt, sema_converted_properties(info));
        if (!type_is_scalar(value.value.type))
            return report_statement_issue(&expr->span, STATEMENT_NONSCALAR_CONDITION);
        attach_conversion_sequence(result, expr, (SemaConversionSequence){ value.steps, value.count });
    }
    attach_condition(result, stmt, (SemaConditionInfo){ .expression = expr });
    return SEMA_OK;
}

// Check independent return operands before any checks that require a known return type.
static SemaStatus analyze_return_statement(StatementContext *ctx, const Node *node)
{
    if (ctx->function_specifiers & FUNCTION_SPEC_NORETURN)
        report_noreturn_return(&node->span);
    const Node *expression = node->u.expr.expr;
    if (!ctx->return_type)
        return expression ? analyze_expression(ctx->sema, expression) : SEMA_OK;
    ReturnValueIssue issue = check_return_value(ctx->return_type, expression != NULL);
    if (issue == RETURN_VALUE_UNEXPECTED)
        analyze_value_expression(ctx->sema, expression, node);
    if (issue != RETURN_VALUE_VALID)
        return report_invalid_return(&node->span, issue);
    return expression ? analyze_assignment_value(ctx->sema, expression, node, ctx->return_type)
                      : SEMA_OK;
}

// Analyze an expression statement in a discarded-value context.
static SemaStatus analyze_expression_statement(StatementContext *ctx, const Node *node)
{
    return analyze_value_expression(ctx->sema, node->u.expr.expr, node);
}

// Analyze each arm in its own block, independently after condition errors.
static SemaStatus analyze_if_statement(StatementContext *ctx, const Node *node)
{
    SemaStatus condition = analyze_condition(ctx, node, node->u.if_stmt.cond);
    SemaStatus then_status = analyze_statement(ctx, node->u.if_stmt.then_stmt, true);
    SemaStatus else_status = SEMA_OK;
    if (node->u.if_stmt.else_stmt)
        else_status = analyze_statement(ctx, node->u.if_stmt.else_stmt, true);
    return merge_sema_status(condition, merge_sema_status(then_status, else_status));
}

// Analyze the controlled body with the current iteration as a jump target.
static SemaStatus analyze_loop_body(StatementContext *ctx, const Node *loop, const Node *body)
{
    ControlFrame frame;
    push_control_frame(ctx, &frame, loop, NULL);
    SemaStatus status = analyze_statement(ctx, body, true);
    pop_control_frame(ctx);
    return status;
}

// Analyze the condition before the while body in source order.
static SemaStatus analyze_while_statement(StatementContext *ctx, const Node *node)
{
    SemaStatus condition = analyze_condition(ctx, node, node->u.while_stmt.cond);
    SemaStatus body = analyze_loop_body(ctx, node, node->u.while_stmt.body);
    return merge_sema_status(condition, body);
}

// Analyze the do body before the condition, closing its block first.
static SemaStatus analyze_do_while_statement(StatementContext *ctx, const Node *node)
{
    SemaStatus body = analyze_loop_body(ctx, node, node->u.do_while.body);
    SemaStatus condition = analyze_condition(ctx, node, node->u.do_while.cond);
    return merge_sema_status(body, condition);
}

// Analyze a declaration or discarded expression in the loop's own scope.
static SemaStatus analyze_for_initializer(StatementContext *ctx, const Node *node)
{
    const Node *init = node->u.for_stmt.init;
    if (!init)
        return SEMA_OK;
    if (init->type == ND_DECL)
        return analyze_for_declaration(ctx->sema, init);
    return analyze_value_expression(ctx->sema, init, node);
}

// Analyze the clauses in source order and enter the body with its own scope.
static SemaStatus analyze_for_statement(StatementContext *ctx, const Node *node)
{
    SemaStatus status = analyze_for_initializer(ctx, node);
    if (!ctx->sema->scope->visibility_unknown) {
        SemaStatus condition = analyze_condition(ctx, node, node->u.for_stmt.cond);
        status = merge_sema_status(status, condition);
    }
    if (!ctx->sema->scope->visibility_unknown && node->u.for_stmt.post) {
        SemaStatus post = analyze_value_expression(ctx->sema, node->u.for_stmt.post, node);
        status = merge_sema_status(status, post);
    }
    SemaStatus body = analyze_loop_body(ctx, node, node->u.for_stmt.body);
    return merge_sema_status(status, body);
}

// ---------------------------------------------------------------------------
// Switch labels and source-level jumps
// ---------------------------------------------------------------------------

// Check the controlling integer and commit its ordered read and promotion.
static SemaStatus analyze_switch_condition(StatementContext *ctx, const Node *node,
                                            const SemaType **out)
{
    const Node *expr = node->u.switch_stmt.cond;
    SemaStatus status = analyze_expression(ctx->sema, expr);
    if (status != SEMA_OK)
        return status;
    SemaResult *result = ctx->sema->result;
    const SemaExprInfo *info = sema_lookup_expression(result, expr);
    SemaConversionPlan value = convert_value(result, node, sema_converted_properties(info));
    if (!type_is_integer(value.value.type))
        return report_statement_issue(&expr->span, STATEMENT_NONINTEGER_SWITCH);
    convert_integer_promotion(&value, node);
    attach_conversion_sequence(result, expr, (SemaConversionSequence){ value.steps, value.count });
    *out = value.value.type;
    return SEMA_OK;
}

// Enter the switch even after a type error so independent labels can be checked.
static SemaStatus analyze_switch_statement(StatementContext *ctx, const Node *node)
{
    const SemaType *comparison_type = NULL;
    SemaStatus condition = analyze_switch_condition(ctx, node, &comparison_type);
    SwitchContext selection;
    init_switch_context(&selection, node, comparison_type);
    ControlFrame frame;
    push_control_frame(ctx, &frame, node, &selection);
    SemaStatus body = analyze_statement(ctx, node->u.switch_stmt.body, true);
    pop_control_frame(ctx);
    free_switch_context(&selection);
    return merge_sema_status(condition, body);
}

// Validate a case operand before comparing or recording its converted value.
static SemaStatus analyze_case_value(StatementContext *ctx, SwitchContext *selection,
                                     const Node *node)
{
    const Node *expr = node->u.case_stmt.expr;
    SemaStatus status = analyze_expression(ctx->sema, expr);
    if (status != SEMA_OK)
        return status;

    SemaResult *result = ctx->sema->result;
    SemaIntegerConstant constant;
    SemaIntegerConstantStatus evaluation = eval_integer_constant(result, expr, &constant);
    if (evaluation != SEMA_INTEGER_CONSTANT_OK)
        return report_invalid_case(&expr->span, evaluation);
    const SemaType *type = selection->comparison_type;
    if (!type)
        return SEMA_OK;

    const SemaExprInfo *info = sema_lookup_expression(result, expr);
    SemaConversionPlan value = { .value = sema_converted_properties(info) };
    convert_case_value(&value, node, type);
    uint64_t bits = eval_integer_conversion(constant, type);
    const Node *prior = lookup_case(selection, bits);
    if (prior)
        return report_duplicate_switch_label(&expr->span, &prior->u.case_stmt.expr->span, ND_CASE);
    bind_case(selection, bits, node);
    attach_conversion_sequence(result, expr, (SemaConversionSequence){ value.steps, value.count });
    attach_switch_label(result, node, (SemaSwitchLabelInfo){ selection->node, type, bits });
    return SEMA_OK;
}

// Associate a case with its switch and continue with its labeled statement.
static SemaStatus analyze_case_statement(StatementContext *ctx, const Node *node)
{
    SwitchContext *selection = lookup_enclosing_switch(ctx);
    SemaStatus status = selection ? analyze_case_value(ctx, selection, node)
        : report_statement_issue(&node->span, STATEMENT_ORPHAN_CASE);
    SemaStatus child = analyze_statement(ctx, node->u.case_stmt.stmt, false);
    return merge_sema_status(status, child);
}

// Check and record the one permitted default label in this switch.
static SemaStatus analyze_default_label(StatementContext *ctx, const Node *node)
{
    SwitchContext *selection = lookup_enclosing_switch(ctx);
    if (!selection)
        return report_statement_issue(&node->span, STATEMENT_ORPHAN_DEFAULT);
    if (selection->default_label)
        return report_duplicate_switch_label(&node->span, &selection->default_label->span, ND_DEFAULT);
    bind_default(selection, node);
    attach_switch_label(ctx->sema->result, node, (SemaSwitchLabelInfo){ .switch_statement = selection->node });
    return SEMA_OK;
}

// Continue through the labeled statement even when its default label failed.
static SemaStatus analyze_default_statement(StatementContext *ctx, const Node *node)
{
    SemaStatus label = analyze_default_label(ctx, node);
    SemaStatus child = analyze_statement(ctx, node->u.default_stmt.stmt, false);
    return merge_sema_status(label, child);
}

// Register a unique name in the function's label namespace.
static SemaStatus analyze_label_definition(StatementContext *ctx, const Node *node)
{
    StringView name = node->u.label.name;
    const Node *prior = lookup_label(ctx, name);
    if (prior)
        return report_duplicate_label(&node->span, name, &prior->span);
    bind_label(ctx, node);
    return SEMA_OK;
}

// Analyze a label and its child without opening a scope for the label itself.
static SemaStatus analyze_label_statement(StatementContext *ctx, const Node *node)
{
    SemaStatus label = analyze_label_definition(ctx, node);
    SemaStatus child = analyze_statement(ctx, node->u.label.stmt, false);
    return merge_sema_status(label, child);
}

// Defer goto resolution until all available definitions have been collected.
static SemaStatus analyze_goto_statement(StatementContext *ctx, const Node *node)
{
    attach_pending_goto(ctx, node);
    return SEMA_OK;
}

// Resolve each deferred goto using only this function's definitions.
static SemaStatus analyze_function_labels(StatementContext *ctx)
{
    SemaStatus status = SEMA_OK;
    for (const PendingGoto *pending = ctx->first_goto; pending; pending = pending->next) {
        const Node *node = pending->node;
        StringView name = node->u.goto_stmt.name;
        const Node *target = lookup_label(ctx, name);
        if (target)
            attach_jump_target(ctx->sema->result, node, target);
        else if (ctx->labels_complete)
            status = report_undefined_label(&node->span, name);
    }
    return status;
}

// Check and attach the nearest source construct a break can exit.
static SemaStatus analyze_break_statement(StatementContext *ctx, const Node *node)
{
    const Node *target = lookup_break_target(ctx);
    if (!target)
        return report_statement_issue(&node->span, STATEMENT_INVALID_BREAK);
    attach_jump_target(ctx->sema->result, node, target);
    return SEMA_OK;
}

// Check and attach the nearest iteration statement a continue can resume.
static SemaStatus analyze_continue_statement(StatementContext *ctx, const Node *node)
{
    const Node *target = lookup_continue_target(ctx);
    if (!target)
        return report_statement_issue(&node->span, STATEMENT_INVALID_CONTINUE);
    attach_jump_target(ctx->sema->result, node, target);
    return SEMA_OK;
}

// ---------------------------------------------------------------------------
// Statement dispatch
// ---------------------------------------------------------------------------

// Dispatch an entered statement. Child analysis descends through proper AST edges.
static SemaStatus analyze_statement_node(StatementContext *ctx, const Node *node)
{
    switch (node->type) {
    case ND_BLOCK: return analyze_block_items(ctx, node->u.block.items);
    case ND_EXPR_STMT: return analyze_expression_statement(ctx, node);
    case ND_RETURN: return analyze_return_statement(ctx, node);
    case ND_IF: return analyze_if_statement(ctx, node);
    case ND_WHILE: return analyze_while_statement(ctx, node);
    case ND_DO_WHILE: return analyze_do_while_statement(ctx, node);
    case ND_FOR: return analyze_for_statement(ctx, node);
    case ND_SWITCH: return analyze_switch_statement(ctx, node);
    case ND_CASE: return analyze_case_statement(ctx, node);
    case ND_DEFAULT: return analyze_default_statement(ctx, node);
    case ND_LABEL: return analyze_label_statement(ctx, node);
    case ND_GOTO: return analyze_goto_statement(ctx, node);
    case ND_BREAK: return analyze_break_statement(ctx, node);
    case ND_CONTINUE: return analyze_continue_statement(ctx, node);
    case ND_EMPTY: return SEMA_OK;
    default: unreachable();
    }
}

// Analyze a statement in its C11 block when it is or serves as a block.
static SemaStatus analyze_statement(StatementContext *ctx, const Node *node, bool controlled)
{
    if (ctx->sema->scope->visibility_unknown) {
        ctx->labels_complete = false;
        return SEMA_UNSUPPORTED;
    }
    if (!controlled && !check_statement_block(node->type))
        return analyze_statement_node(ctx, node);
    SemaScope scope;
    push_scope(ctx->sema, &scope);
    SemaStatus status = analyze_statement_node(ctx, node);
    pop_scope(ctx->sema);
    return status;
}

// Dispatch one block item without processing its siblings.
static SemaStatus analyze_block_item(StatementContext *ctx, const Node *item)
{
    if (item->type == ND_DECL)
        return analyze_declaration(ctx->sema, item);
    if (item->type == ND_STATIC_ASSERT)
        return analyze_static_assertion(ctx->sema, item);
    return analyze_statement(ctx, item, false);
}

// Analyze items in source order until a skipped declaration obscures visibility.
static SemaStatus analyze_block_items(StatementContext *ctx, const Node *items)
{
    SemaStatus status = SEMA_OK;
    for (const Node *item = items; item; item = item->next) {
        if (ctx->sema->scope->visibility_unknown) {
            ctx->labels_complete = false;
            return merge_sema_status(status, SEMA_UNSUPPORTED);
        }
        SemaStatus current = analyze_block_item(ctx, item);
        status = merge_sema_status(status, current);
    }
    return status;
}

// ---------------------------------------------------------------------------
// Function-body analysis interface
// ---------------------------------------------------------------------------

// Analyze the outer block and deferred labels. A missing return type permits intrinsic checks.
SemaStatus analyze_function_body(SemaContext *ctx, const Node *body,
                                 const SemaType *return_type, unsigned function_specifiers)
{
    StatementContext function;
    init_statement_context(&function, ctx, return_type, function_specifiers);
    SemaStatus statements = analyze_block_items(&function, body->u.block.items);
    SemaStatus labels = analyze_function_labels(&function);
    free_statement_context(&function);
    return merge_sema_status(statements, labels);
}
