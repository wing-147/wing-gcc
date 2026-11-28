// Expression analysis establishes intrinsic properties and delegates conversions.

#include "sema_internal.h"

#include "util/diagnostic.h"

#include <stdlib.h>

typedef enum {
    EXPRESSION_TYPEDEF_NAME,
    EXPRESSION_INVALID_ADDRESS,
    EXPRESSION_INVALID_INDIRECTION,
    EXPRESSION_INCOMPLETE_SIZEOF,
    EXPRESSION_NONSCALAR_LOGNOT,
    EXPRESSION_INVALID_UNARY,
    EXPRESSION_INVALID_BINARY,
    EXPRESSION_NONREAL_INCREMENT,
    EXPRESSION_INVALID_UPDATE,
    EXPRESSION_NONSCALAR_CONDITION,
    EXPRESSION_INCOMPATIBLE_BRANCHES,
    EXPRESSION_INVALID_CAST,
    EXPRESSION_INCOMPLETE_ARGUMENT,
    EXPRESSION_ARGUMENT_COUNT,
    EXPRESSION_INVALID_CALLEE,
    EXPRESSION_INCOMPATIBLE_CONVERSION,
    EXPRESSION_UNMODIFIABLE_ASSIGNMENT,
    EXPRESSION_UNMODIFIABLE_COMPOUND,
    EXPRESSION_UNMODIFIABLE_INCREMENT,
    EXPRESSION_INVALID_SUBSCRIPT,
    EXPRESSION_INVALID_MEMBER_BASE,
    EXPRESSION_UNKNOWN_MEMBER,
    EXPRESSION_BIT_FIELD_SIZEOF,
    EXPRESSION_INCOMPLETE_CALL_RESULT,
    EXPRESSION_INCOMPLETE_VALUE,
    EXPRESSION_INVALID_COMPOUND_LITERAL,
    EXPRESSION_INVALID_ALIGNOF,
    EXPRESSION_GENERIC_TYPE,
    EXPRESSION_GENERIC_DUPLICATE,
    EXPRESSION_GENERIC_DEFAULT,
    EXPRESSION_GENERIC_NO_MATCH,
    EXPRESSION_GENERIC_MULTIPLE_MATCHES,
} ExpressionIssue;

typedef enum {
    IDENTIFIER_RESOLVED,
    IDENTIFIER_UNDECLARED,
    IDENTIFIER_FAILED_BINDING,
    IDENTIFIER_TYPEDEF,
} IdentifierIssue;

typedef struct {
    IdentifierIssue issue;
    const SemaSymbol *symbol;
    SemaExprProperties value;
    SemaStatus failure_status;
} IdentifierResolution;

static SemaStatus analyze_increment_expression(SemaContext *ctx, const Node *node);

// ---------------------------------------------------------------------------
// Expression records, diagnostics and names
// ---------------------------------------------------------------------------

// Report an expression constraint at the construct that requires it.
static SemaStatus report_invalid_expression(const SourceSpan *span, ExpressionIssue issue)
{
    static const char *const g_messages[] = {
        [EXPRESSION_TYPEDEF_NAME] = "typedef name used as an expression",
        [EXPRESSION_INVALID_ADDRESS] = "invalid operand of unary '&'",
        [EXPRESSION_INVALID_INDIRECTION] = "operand of unary '*' must be a pointer",
        [EXPRESSION_INCOMPLETE_SIZEOF] = "sizeof requires a complete object type",
        [EXPRESSION_NONSCALAR_LOGNOT] = "operand of '!' must be scalar",
        [EXPRESSION_INVALID_UNARY] = "invalid operand type for unary operator",
        [EXPRESSION_INVALID_BINARY] = "invalid operands to binary operator",
        [EXPRESSION_NONREAL_INCREMENT] = "increment or decrement requires a real or pointer operand",
        [EXPRESSION_INVALID_UPDATE] = "invalid operand type for update",
        [EXPRESSION_NONSCALAR_CONDITION] = "conditional control must have scalar type",
        [EXPRESSION_INCOMPATIBLE_BRANCHES] = "incompatible conditional operands",
        [EXPRESSION_INVALID_CAST] = "invalid cast between these types",
        [EXPRESSION_INCOMPLETE_ARGUMENT] = "argument must have complete object type",
        [EXPRESSION_ARGUMENT_COUNT] = "wrong number of arguments for prototype",
        [EXPRESSION_INVALID_CALLEE] = "called expression is not a function pointer",
        [EXPRESSION_INCOMPATIBLE_CONVERSION] = "incompatible types in implicit conversion",
        [EXPRESSION_UNMODIFIABLE_ASSIGNMENT] = "left operand of assignment is not a modifiable lvalue",
        [EXPRESSION_UNMODIFIABLE_COMPOUND] = "left operand of compound assignment is not a modifiable lvalue",
        [EXPRESSION_UNMODIFIABLE_INCREMENT] = "increment or decrement requires a modifiable lvalue",
        [EXPRESSION_INVALID_SUBSCRIPT] = "subscript requires a pointer to a complete object and an integer",
        [EXPRESSION_INVALID_MEMBER_BASE] = "member access requires a complete structure or union",
        [EXPRESSION_UNKNOWN_MEMBER] = "no such member in structure or union",
        [EXPRESSION_BIT_FIELD_SIZEOF] = "sizeof cannot be applied to a bit-field",
        [EXPRESSION_INCOMPLETE_CALL_RESULT] = "called function has incomplete return type",
        [EXPRESSION_INCOMPLETE_VALUE] = "value used from an incomplete object type",
        [EXPRESSION_INVALID_COMPOUND_LITERAL] = "compound literal requires a complete object type or an array of unknown size",
        [EXPRESSION_INVALID_ALIGNOF] = "_Alignof requires a complete object type",
        [EXPRESSION_GENERIC_TYPE] = "generic association requires a complete object type",
        [EXPRESSION_GENERIC_DUPLICATE] = "generic selection specifies compatible types more than once",
        [EXPRESSION_GENERIC_DEFAULT] = "generic selection specifies more than one default association",
        [EXPRESSION_GENERIC_NO_MATCH] = "generic selector has no compatible association",
        [EXPRESSION_GENERIC_MULTIPLE_MATCHES] = "generic selector matches more than one association",
    };
    error_at(span, "%s", g_messages[issue]);
    return SEMA_INVALID;
}

// Report a missing ordinary name at its use site.
static SemaStatus report_undeclared_identifier(const SourceSpan *span, StringView name)
{
    error_at(span, "'%.*s' undeclared", (int)string_view_length(name),
             string_view_begin(name));
    return SEMA_INVALID;
}

// Attach a typed value with no associated ordinary-name binding.
static SemaStatus attach_value(SemaContext *ctx, const Node *node, const SemaType *type)
{
    attach_expression(ctx->result, node, (SemaExprProperties){ .type = type, .category = SEMA_VALUE });
    return SEMA_OK;
}

// Fetch the type after the conversions requested by the current use.
static const SemaType *lookup_expression_type(const SemaContext *ctx, const Node *node)
{
    return sema_converted_properties(sema_lookup_expression(ctx->result, node)).type;
}

// Read the starting properties of a subsequent conversion.
static SemaExprProperties lookup_expression_value(const SemaContext *ctx, const Node *node)
{
    return sema_converted_properties(sema_lookup_expression(ctx->result, node));
}

// Borrow the plan's ordered steps until the result interface copies them.
static SemaConversionSequence conversion_sequence(const SemaConversionPlan *plan)
{
    return (SemaConversionSequence){ .steps = plan->steps, .count = plan->count };
}

// Append the two checked conversion chains to their respective operand records.
static void attach_operand_conversions(SemaResult *result, const Node *lhs,
                                       const Node *rhs, const SemaOperandConversions *operands)
{
    attach_conversion_sequence(result, lhs, conversion_sequence(&operands->left));
    attach_conversion_sequence(result, rhs, conversion_sequence(&operands->right));
}

// Read a decoded literal without interpreting its source spelling again.
static SemaStatus analyze_literal(SemaContext *ctx, const Node *node)
{
    return attach_value(ctx, node, type_literal(node->u.num.literal));
}

// Give a decoded string its character array type, including the terminator.
static SemaStatus analyze_string_literal(SemaContext *ctx, const Node *node)
{
    const SemaType *type = new_array_type(ctx->result, type_builtin(SEMA_TYPE_CHAR),
                                         (uint64_t)node->u.string.length + 1);
    attach_expression(ctx->result, node, (SemaExprProperties){
        .type = type, .category = SEMA_LVALUE,
    });
    return SEMA_OK;
}

// Interpret the visible binding and derive the identifier's intrinsic value category.
static IdentifierResolution resolve_identifier(const SemaScope *scope, StringView name)
{
    const SemaBinding *binding = lookup_visible_binding(scope, name);
    if (!binding)
        return (IdentifierResolution){ .issue = IDENTIFIER_UNDECLARED };
    if (binding->failure_status != SEMA_OK)
        return (IdentifierResolution){ .issue = IDENTIFIER_FAILED_BINDING,
                                       .failure_status = binding->failure_status };
    const SemaSymbol *symbol = binding->symbol;
    if (symbol->kind == SEMA_SYMBOL_TYPEDEF)
        return (IdentifierResolution){ .issue = IDENTIFIER_TYPEDEF };
    SemaValueCategory category = SEMA_LVALUE;
    if (symbol->kind == SEMA_SYMBOL_FUNCTION)
        category = SEMA_FUNCTION_DESIGNATOR;
    if (symbol->kind == SEMA_SYMBOL_ENUMERATOR)
        category = SEMA_VALUE;
    return (IdentifierResolution){ .issue = IDENTIFIER_RESOLVED, .symbol = symbol,
        .value = { .type = symbol->type, .category = category,
                   .register_object = (symbol->storage & STORAGE_REGISTER) != 0 } };
}

// Report an identifier resolution failure or publish the binding and its type snapshot.
static SemaStatus analyze_identifier(SemaContext *ctx, const Node *node)
{
    StringView name = node->u.identifier.name;
    IdentifierResolution resolution = resolve_identifier(ctx->scope, name);
    const SemaSymbol *symbol = resolution.symbol;
    switch (resolution.issue) {
    case IDENTIFIER_UNDECLARED:
        return report_undeclared_identifier(&node->span, name);
    case IDENTIFIER_FAILED_BINDING:
        return resolution.failure_status;
    case IDENTIFIER_TYPEDEF:
        return report_invalid_expression(&node->span, EXPRESSION_TYPEDEF_NAME);
    case IDENTIFIER_RESOLVED:
        break;
    }
    if (ctx->function && symbol->entity && symbol->entity->linkage == SEMA_LINKAGE_INTERNAL)
        attach_inline_reference(ctx->function, &node->span);
    attach_identifier_expression(ctx->result, node, resolution.value, symbol);
    return SEMA_OK;
}

// ---------------------------------------------------------------------------
// Unary operators and type queries
// ---------------------------------------------------------------------------

// Preserve the type and category of the operand of unary &.
static SemaStatus analyze_address_expression(SemaContext *ctx, const Node *node)
{
    const Node *operand = node->u.unary.operand;
    SemaStatus status = analyze_expression(ctx, operand);
    if (status != SEMA_OK)
        return status;

    const SemaExprInfo *info = sema_lookup_expression(ctx->result, operand);
    bool dereference = operand->type == ND_UNARY && operand->u.unary.op == UNARY_DEREF;
    if ((!dereference && info->original.category == SEMA_VALUE) || info->original.bit_field ||
        info->original.register_object)
        return report_invalid_expression(&node->span, EXPRESSION_INVALID_ADDRESS);
    return attach_value(ctx, node, new_pointer_type(ctx->result, info->original.type, 0));
}

// Dereference a pointer to an object or a function designator.
static SemaStatus analyze_indirection_expression(SemaContext *ctx, const Node *node)
{
    const Node *operand = node->u.unary.operand;
    SemaStatus status = analyze_value_expression(ctx, operand, node);
    if (status != SEMA_OK)
        return status;
    const SemaType *pointer = lookup_expression_type(ctx, operand);
    if (pointer->kind != SEMA_TYPE_POINTER)
        return report_invalid_expression(&node->span, EXPRESSION_INVALID_INDIRECTION);

    const SemaType *type = pointer->u.pointee;
    SemaValueCategory category = SEMA_LVALUE;
    if (type->kind == SEMA_TYPE_FUNCTION)
        category = SEMA_FUNCTION_DESIGNATOR;
    else if (type->kind == SEMA_TYPE_VOID)
        category = SEMA_VALUE;
    attach_expression(ctx->result, node, (SemaExprProperties){ .type = type, .category = category });
    return SEMA_OK;
}

// Attach a target size for an expression or type operand of sizeof.
static SemaStatus analyze_sizeof_expression(SemaContext *ctx, const Node *node)
{
    const SemaType *type;
    SemaStatus status;
    if (node->type == ND_SIZEOF_TYPE) {
        status = analyze_type_name(ctx, node->u.type_query.type_name, &type);
    } else {
        const Node *operand = node->u.unary.operand;
        status = analyze_expression(ctx, operand);
        if (status == SEMA_OK) {
            SemaExprProperties properties = lookup_expression_value(ctx, operand);
            if (properties.bit_field)
                return report_invalid_expression(&node->span, EXPRESSION_BIT_FIELD_SIZEOF);
            type = properties.type;
        }
    }
    if (status != SEMA_OK)
        return status;
    if (!type_is_complete_object(type))
        return report_invalid_expression(&node->span, EXPRESSION_INCOMPLETE_SIZEOF);
    SemaExprProperties value = { .type = type_size_t(), .category = SEMA_VALUE };
    attach_sizeof_expression(ctx->result, node, value, type_size(type));
    return SEMA_OK;
}

// Analyze an unevaluated alignment operand and attach its constant value.
static SemaStatus analyze_alignof_expression(SemaContext *ctx, const Node *node)
{
    const SemaType *type;
    SemaStatus status;
    if (node->type == ND_ALIGNOF_TYPE) {
        status = analyze_type_name(ctx, node->u.type_query.type_name, &type);
    } else {
        const Node *operand = node->u.unary.operand;
        status = analyze_expression(ctx, operand);
        if (status == SEMA_OK)
            type = lookup_expression_type(ctx, operand);
    }
    if (status != SEMA_OK)
        return status;
    if (!type_is_complete_object(type))
        return report_invalid_expression(&node->span, EXPRESSION_INVALID_ALIGNOF);
    attach_alignof_expression(ctx->result, node,
        (SemaExprProperties){ .type = type_size_t(), .category = SEMA_VALUE }, type_alignment(type));
    return SEMA_OK;
}

// Resolve the designated object of a subscript with already typed operands.
static const SemaType *resolve_subscript_type(const SemaType *base, const SemaType *index)
{
    if (type_is_integer(base) && index->kind == SEMA_TYPE_POINTER) {
        const SemaType *integer = base;
        base = index;
        index = integer;
    }
    return base->kind == SEMA_TYPE_POINTER && type_is_integer(index) &&
           type_is_complete_object(base->u.pointee) ? base->u.pointee : NULL;
}

// Analyze and convert both operands before attaching the subscript designator.
static SemaStatus analyze_subscript_expression(SemaContext *ctx, const Node *node)
{
    const Node *base = node->u.subscript.base;
    const Node *index = node->u.subscript.index;
    SemaStatus left = analyze_expression(ctx, base);
    if (ctx->scope->visibility_unknown)
        return left;
    SemaStatus right = analyze_expression(ctx, index);
    SemaStatus status = merge_sema_status(left, right);
    if (status != SEMA_OK)
        return status;

    SemaResult *result = ctx->result;
    SemaExprProperties left_value = lookup_expression_value(ctx, base);
    SemaExprProperties right_value = lookup_expression_value(ctx, index);
    SemaOperandConversions operands = {
        .left = convert_value(result, node, left_value),
        .right = convert_value(result, node, right_value),
    };
    const SemaType *type = resolve_subscript_type(operands.left.value.type, operands.right.value.type);
    if (!type)
        return report_invalid_expression(&node->span, EXPRESSION_INVALID_SUBSCRIPT);
    // Only an array operand designates the containing object before conversion.
    bool register_object = (left_value.type->kind == SEMA_TYPE_ARRAY && left_value.register_object) ||
                           (right_value.type->kind == SEMA_TYPE_ARRAY && right_value.register_object);
    attach_operand_conversions(result, base, index, &operands);
    attach_expression(result, node, (SemaExprProperties){
        .type = type, .category = SEMA_LVALUE, .register_object = register_object,
    });
    return SEMA_OK;
}

// Interpret direct and indirect member bases without performing a lookup.
static const SemaType *resolve_member_base(const SemaType *type, bool indirect)
{
    if (indirect) {
        if (type->kind != SEMA_TYPE_POINTER)
            return NULL;
        type = type->u.pointee;
    }
    if (type->kind != SEMA_TYPE_STRUCT && type->kind != SEMA_TYPE_UNION)
        return NULL;
    return type_is_complete_object(type) ? type : NULL;
}

// Apply the member-access qualification and byte-offset rules to a found route.
static void resolve_member_properties(const SemaMemberPath *path,
                                       unsigned *qualifiers, uint64_t *offset)
{
    for (size_t i = 0; i < path->count; ++i) {
        const SemaMember *member = path->members[i];
        *qualifiers |= member->type->qualifiers;
        *offset += member->offset;
    }
}

// Analyze the operator and attach the checked, qualified member designator.
static SemaStatus analyze_member_expression(SemaContext *ctx, const Node *node)
{
    const Node *base = node->u.member_access.base;
    SemaStatus status = analyze_expression(ctx, base);
    if (status != SEMA_OK)
        return status;
    SemaResult *result = ctx->result;
    SemaExprProperties original = lookup_expression_value(ctx, base);
    bool indirect = node->u.member_access.kind == MEMBER_INDIRECT;
    SemaConversionPlan plan = indirect ? convert_value(result, node, original)
                                       : (SemaConversionPlan){ .value = original };
    const SemaType *aggregate = resolve_member_base(plan.value.type, indirect);
    if (!aggregate)
        return report_invalid_expression(&node->span, EXPRESSION_INVALID_MEMBER_BASE);
    SemaMemberPath path = new_member_path(&aggregate->u.tag->u.aggregate, node->u.member_access.name);
    if (!path.count)
        return report_invalid_expression(&node->span, EXPRESSION_UNKNOWN_MEMBER);
    const SemaMember *member = path.members[path.count - 1];
    unsigned qualifiers = aggregate->qualifiers;
    uint64_t offset = 0;
    resolve_member_properties(&path, &qualifiers, &offset);
    free_member_path(&path);
    const SemaType *type = new_qualified_type(result, member->type, qualifiers);
    SemaExprProperties value = {
        .type = type, .category = indirect ? SEMA_LVALUE : original.category,
        .bit_field = member->kind == SEMA_MEMBER_BIT_FIELD ? member : NULL,
        .register_object = !indirect && original.register_object,
    };
    attach_conversion_sequence(result, base, conversion_sequence(&plan));
    attach_member_expression(result, node, value, member, offset);
    return SEMA_OK;
}

// Analyze the scalar arithmetic and logical unary operators.
static SemaStatus analyze_arithmetic_unary(SemaContext *ctx, const Node *node)
{
    const Node *operand = node->u.unary.operand;
    Operator op = node->u.unary.op;
    SemaStatus status = analyze_value_expression(ctx, operand, node);
    if (status != SEMA_OK)
        return status;
    const SemaType *type = lookup_expression_type(ctx, operand);
    if (op == UNARY_LOGNOT) {
        if (!type_is_scalar(type))
            return report_invalid_expression(&node->span, EXPRESSION_NONSCALAR_LOGNOT);
        return attach_value(ctx, node, type_builtin(SEMA_TYPE_INT));
    }
    if (op == UNARY_BITNOT ? !type_is_integer(type) : !type_is_arithmetic(type))
        return report_invalid_expression(&node->span, EXPRESSION_INVALID_UNARY);
    if (type_is_integer(type)) {
        SemaConversionPlan plan = { .value = lookup_expression_value(ctx, operand) };
        convert_integer_promotion(&plan, node);
        attach_conversion_sequence(ctx->result, operand, conversion_sequence(&plan));
        type = plan.value.type;
    }
    return attach_value(ctx, node, type);
}

// Dispatch unary syntax to its value or designator rule.
static SemaStatus analyze_unary_expression(SemaContext *ctx, const Node *node)
{
    switch (node->u.unary.op) {
    case UNARY_ADDR: return analyze_address_expression(ctx, node);
    case UNARY_DEREF: return analyze_indirection_expression(ctx, node);
    case UNARY_SIZEOF: return analyze_sizeof_expression(ctx, node);
    case UNARY_ALIGNOF: return analyze_alignof_expression(ctx, node);
    case UNARY_PRE_INC:
    case UNARY_PRE_DEC:
    case UNARY_POST_INC:
    case UNARY_POST_DEC:
        return analyze_increment_expression(ctx, node);
    case UNARY_PLUS:
    case UNARY_NEG:
    case UNARY_BITNOT:
    case UNARY_LOGNOT:
        return analyze_arithmetic_unary(ctx, node);
    default:
        return mark_unsupported(ctx, true);
    }
}

// ---------------------------------------------------------------------------
// Binary operations and updates
// ---------------------------------------------------------------------------

// Supply the source-level constant facts for a shared pointer conversion.
static const SemaType *analyze_pointer_operands(SemaResult *result, const Node *use,
                                                const Node *lhs, const Node *rhs,
                                                bool allow_void_or_null,
                                                SemaOperandConversions *operands)
{
    SemaPointerConversionContext context = {
        .allow_void_or_null = allow_void_or_null,
        .left_is_null = allow_void_or_null && operands->right.value.type->kind == SEMA_TYPE_POINTER &&
            eval_is_null_pointer_constant(result, lhs),
        .right_is_null = allow_void_or_null && operands->left.value.type->kind == SEMA_TYPE_POINTER &&
            eval_is_null_pointer_constant(result, rhs),
    };
    return convert_pointer_operands(result, use, context, operands);
}

// Choose the comparison rule and return int for accepted operands.
static const SemaType *analyze_comparison_operands(SemaResult *result, const Node *node,
                                                   SemaOperandConversions *operands)
{
    Operator op = node->u.binary.op;
    bool equality = op == BINARY_EQ || op == BINARY_NE;
    const SemaType *left = operands->left.value.type;
    const SemaType *right = operands->right.value.type;
    bool arithmetic = equality ? type_is_arithmetic(left) && type_is_arithmetic(right)
                               : type_is_real(left) && type_is_real(right);
    if (arithmetic) {
        convert_usual_arithmetic(node, operands);
    } else if (!analyze_pointer_operands(result, node, node->u.binary.lhs,
                                         node->u.binary.rhs, equality, operands)) {
        return NULL;
    }
    return type_builtin(SEMA_TYPE_INT);
}

// Select the binary operand rule and delegate the required conversions.
static const SemaType *analyze_binary_operands(SemaResult *result, const Node *node,
                                               SemaOperandConversions *operands)
{
    const SemaType *left = operands->left.value.type;
    const SemaType *right = operands->right.value.type;
    Operator op = node->u.binary.op;

    switch (op) {
    case BINARY_COMMA:
        return right;
    case BINARY_LOGAND:
    case BINARY_LOGOR:
        return type_is_scalar(left) && type_is_scalar(right) ? type_builtin(SEMA_TYPE_INT) : NULL;
    case BINARY_LT:
    case BINARY_LE:
    case BINARY_GT:
    case BINARY_GE:
    case BINARY_EQ:
    case BINARY_NE:
        return analyze_comparison_operands(result, node, operands);
    default:
        break;
    }
    SemaArithmeticPlan plan;
    if (!plan_arithmetic(op, operands->left.value, operands->right.value, &plan))
        return NULL;
    convert_arithmetic_operands(&plan, node, operands);
    return plan.result;
}

// Analyze binary operands and preserve independent diagnostics.
static SemaStatus analyze_binary_expression(SemaContext *ctx, const Node *node)
{
    const Node *lhs = node->u.binary.lhs;
    const Node *rhs = node->u.binary.rhs;
    SemaStatus left = analyze_expression(ctx, lhs);
    if (ctx->scope->visibility_unknown)
        return left;
    SemaStatus right = analyze_expression(ctx, rhs);
    SemaStatus status = merge_sema_status(left, right);
    if (status != SEMA_OK)
        return status;

    SemaOperandConversions operands = {
        .left = convert_value(ctx->result, node, lookup_expression_value(ctx, lhs)),
        .right = convert_value(ctx->result, node, lookup_expression_value(ctx, rhs)),
    };
    const SemaType *type = analyze_binary_operands(ctx->result, node, &operands);
    if (!type)
        return report_invalid_expression(&node->span, EXPRESSION_INVALID_BINARY);
    attach_operand_conversions(ctx->result, lhs, rhs, &operands);
    return attach_value(ctx, node, type);
}

// Check the value category and type required to modify the designated object.
static bool check_modifiable_lvalue(SemaExprProperties properties)
{
    return properties.category == SEMA_LVALUE && type_is_modifiable_object(properties.type);
}

// Analyze a target while preserving its designator for the enclosing update.
static SemaStatus analyze_modifiable_target(SemaContext *ctx, const Node *target,
                                             ExpressionIssue issue)
{
    SemaStatus status = analyze_expression(ctx, target);
    if (status == SEMA_OK &&
        !check_modifiable_lvalue(sema_lookup_expression(ctx->result, target)->original))
        return report_invalid_expression(&target->span, issue);
    return status;
}

// Analyze simple assignment while preserving the left lvalue.
static SemaStatus analyze_simple_assignment(SemaContext *ctx, const Node *node)
{
    const Node *lhs = node->u.assign.lhs;
    const Node *rhs = node->u.assign.rhs;
    SemaStatus left = analyze_modifiable_target(
        ctx, lhs, EXPRESSION_UNMODIFIABLE_ASSIGNMENT);
    if (ctx->scope->visibility_unknown)
        return left;

    const SemaType *type = NULL;
    if (left == SEMA_OK)
        type = new_qualified_type(ctx->result, lookup_expression_type(ctx, lhs), 0);
    SemaStatus right = type ? analyze_assignment_value(ctx, rhs, node, type)
                            : analyze_value_expression(ctx, rhs, node);
    SemaStatus status = merge_sema_status(left, right);
    if (status != SEMA_OK)
        return status;
    bool atomic = lookup_expression_type(ctx, lhs)->qualifiers & TYPE_QUAL_ATOMIC;
    attach_assignment_expression(ctx->result, node,
        (SemaExprProperties){ .type = type, .category = SEMA_VALUE }, atomic);
    return SEMA_OK;
}

// Match a compound assignment to its arithmetic operator.
static Operator compound_arithmetic_operator(Operator op)
{
    switch (op) {
    case ADD_ASSIGN: return BINARY_ADD;
    case SUB_ASSIGN: return BINARY_SUB;
    case MUL_ASSIGN: return BINARY_MUL;
    case DIV_ASSIGN: return BINARY_DIV;
    case MOD_ASSIGN: return BINARY_REM;
    case SHL_ASSIGN: return BINARY_SHL;
    case SHR_ASSIGN: return BINARY_SHR;
    case AND_ASSIGN: return BINARY_BITAND;
    case XOR_ASSIGN: return BINARY_BITXOR;
    case OR_ASSIGN: return BINARY_BITOR;
    default: unreachable();
    }
}

// Check compound-assignment types before applying the common arithmetic rules.
static bool check_update_operands(const SemaType *target, const SemaType *rhs)
{
    return target->kind == SEMA_TYPE_POINTER ? type_is_integer(rhs)
                                              : type_is_arithmetic(rhs);
}

// Analyze each operand once and delegate computation conversions and writeback.
// With no explicit RHS, the increment or decrement computes with the unit 1.
static SemaStatus analyze_update_expression(SemaContext *ctx, const Node *node,
                                            const Node *target, const Node *rhs,
                                            Operator op, bool returns_previous_value)
{
    ExpressionIssue issue = rhs ? EXPRESSION_UNMODIFIABLE_COMPOUND
                                : EXPRESSION_UNMODIFIABLE_INCREMENT;
    SemaStatus left = analyze_modifiable_target(ctx, target, issue);
    if (ctx->scope->visibility_unknown)
        return left;
    SemaStatus right = rhs ? analyze_expression(ctx, rhs) : SEMA_OK;
    SemaStatus status = merge_sema_status(left, right);
    if (status != SEMA_OK)
        return status;

    const SemaExprInfo *target_info = sema_lookup_expression(ctx->result, target);
    SemaUpdateConversions conversions = {
        .target_read = convert_value(ctx->result, node, sema_converted_properties(target_info)),
        .rhs = rhs ? convert_value(ctx->result, node, lookup_expression_value(ctx, rhs))
                   : (SemaConversionPlan){
            .value = { .type = type_builtin(SEMA_TYPE_INT), .category = SEMA_VALUE },
        },
    };
    const SemaType *type = conversions.target_read.value.type;
    const SemaType *source = conversions.rhs.value.type;
    if (!rhs && !type_is_real(type) && type->kind != SEMA_TYPE_POINTER)
        return report_invalid_expression(&node->span, EXPRESSION_NONREAL_INCREMENT);
    SemaArithmeticPlan plan;
    if (!check_update_operands(type, source) ||
        !plan_arithmetic(op, conversions.target_read.value, conversions.rhs.value, &plan))
        return report_invalid_expression(&node->span, EXPRESSION_INVALID_UPDATE);

    convert_update(&plan, node, &conversions);
    SemaUpdateInfo update = {
        .target = target, .arithmetic_operator = op, .calculation_type = plan.result,
        .target_read = conversion_sequence(&conversions.target_read),
        .writeback = conversion_sequence(&conversions.writeback),
        .flags = returns_previous_value ? SEMA_UPDATE_RETURNS_PREVIOUS_VALUE : 0,
    };
    if (target_info->original.type->qualifiers & TYPE_QUAL_ATOMIC)
        update.flags |= SEMA_UPDATE_ATOMIC;
    if (rhs) {
        attach_conversion_sequence(ctx->result, rhs, conversion_sequence(&conversions.rhs));
    } else {
        update.implicit_rhs_type = plan.right.source;
        update.implicit_rhs = conversion_sequence(&conversions.rhs);
    }
    attach_update_expression(ctx->result, node,
        (SemaExprProperties){ .type = plan.left.source, .category = SEMA_VALUE }, update);
    return SEMA_OK;
}

// Select the arithmetic operation and result timing for ++ or --.
static SemaStatus analyze_increment_expression(SemaContext *ctx, const Node *node)
{
    Operator op = node->u.unary.op;
    bool postfix = op == UNARY_POST_INC || op == UNARY_POST_DEC;
    Operator arithmetic = op == UNARY_PRE_INC || op == UNARY_POST_INC ? BINARY_ADD : BINARY_SUB;
    return analyze_update_expression(ctx, node, node->u.unary.operand, NULL,
                                     arithmetic, postfix);
}

// Dispatch simple and compound assignments to their different value uses.
static SemaStatus analyze_assignment_expression(SemaContext *ctx, const Node *node)
{
    if (node->u.assign.op == ASSIGN)
        return analyze_simple_assignment(ctx, node);
    return analyze_update_expression(ctx, node, node->u.assign.lhs, node->u.assign.rhs,
                                     compound_arithmetic_operator(node->u.assign.op), false);
}

// ---------------------------------------------------------------------------
// Conditional, cast, call and generic expressions
// ---------------------------------------------------------------------------

// Select the branch conversion context once the conditional operands are typed.
static const SemaType *analyze_conditional_operands(SemaResult *result, const Node *node,
                                                    SemaOperandConversions *operands)
{
    const SemaType *a = operands->left.value.type;
    const SemaType *b = operands->right.value.type;
    if (type_is_arithmetic(a) && type_is_arithmetic(b))
        return convert_usual_arithmetic(node, operands);
    if (a->kind == SEMA_TYPE_VOID && b->kind == SEMA_TYPE_VOID)
        return a;
    if ((a->kind == SEMA_TYPE_STRUCT || a->kind == SEMA_TYPE_UNION) &&
        type_is_complete_object(a) && type_is_compatible_unqualified(a, b))
        return a;
    return analyze_pointer_operands(result, node, node->u.cond.then_expr,
                                    node->u.cond.else_expr, true, operands);
}

// Analyze the conditional branches and form their C11 result type.
static SemaStatus analyze_conditional_expression(SemaContext *ctx, const Node *node)
{
    const Node *cond = node->u.cond.cond;
    const Node *lhs = node->u.cond.then_expr;
    const Node *rhs = node->u.cond.else_expr;
    SemaStatus status = analyze_expression(ctx, cond);
    if (ctx->scope->visibility_unknown)
        return status;
    status = merge_sema_status(status, analyze_expression(ctx, lhs));
    if (ctx->scope->visibility_unknown)
        return status;
    status = merge_sema_status(status, analyze_expression(ctx, rhs));
    if (status != SEMA_OK)
        return status;
    SemaConversionPlan control_value = convert_value(
        ctx->result, node, lookup_expression_value(ctx, cond));
    if (!type_is_scalar(control_value.value.type))
        return report_invalid_expression(&cond->span, EXPRESSION_NONSCALAR_CONDITION);

    SemaOperandConversions operands = {
        .left = convert_value(ctx->result, node, lookup_expression_value(ctx, lhs)),
        .right = convert_value(ctx->result, node, lookup_expression_value(ctx, rhs)),
    };
    const SemaType *type = analyze_conditional_operands(ctx->result, node, &operands);
    if (!type)
        return report_invalid_expression(&node->span, EXPRESSION_INCOMPATIBLE_BRANCHES);
    attach_conversion_sequence(ctx->result, cond, conversion_sequence(&control_value));
    attach_operand_conversions(ctx->result, lhs, rhs, &operands);
    attach_conditional_expression(ctx->result, node,
        (SemaExprProperties){ .type = type, .category = SEMA_VALUE },
        (SemaConditionInfo){ .expression = cond });
    return SEMA_OK;
}

// Type the explicit cast AST node. Its operand still has ordinary implicit
// value conversions, including decay when casting a function designator.
static SemaStatus analyze_cast_expression(SemaContext *ctx, const Node *node)
{
    const SemaType *target;
    SemaStatus status = analyze_type_name(ctx, node->u.cast.type_name, &target);
    const Node *operand = node->u.cast.expr;
    if (status != SEMA_OK)
        return ctx->scope->visibility_unknown ? status
            : merge_sema_status(status, analyze_expression(ctx, operand));
    status = analyze_value_expression(ctx, operand, node);
    if (status != SEMA_OK)
        return status;

    const SemaType *source = lookup_expression_type(ctx, operand);
    if (target->kind != SEMA_TYPE_VOID &&
        (!type_is_scalar(target) || !type_is_scalar(source) ||
         (target->kind == SEMA_TYPE_POINTER && !type_is_integer(source) &&
          source->kind != SEMA_TYPE_POINTER) ||
         (source->kind == SEMA_TYPE_POINTER && !type_is_integer(target) &&
          target->kind != SEMA_TYPE_POINTER)))
        return report_invalid_expression(&node->span, EXPRESSION_INVALID_CAST);
    return attach_value(ctx, node, new_qualified_type(ctx->result, target, 0));
}

// Apply a fixed-parameter assignment conversion or default argument promotions.
static SemaStatus analyze_argument(SemaContext *ctx, const Node *arg,
                                    const Node *call, const SemaType *function,
                                    size_t position)
{
    if (function && type_has_prototype(function) &&
        position < function->u.function.parameter_count)
        return analyze_assignment_value(ctx, arg, call, function->u.function.parameters[position]);

    SemaStatus status = analyze_value_expression(ctx, arg, call);
    if (status != SEMA_OK || !function)
        return status;
    if (!type_is_complete_object(lookup_expression_type(ctx, arg)))
        return report_invalid_expression(&arg->span, EXPRESSION_INCOMPLETE_ARGUMENT);
    SemaConversionPlan plan = { .value = lookup_expression_value(ctx, arg) };
    convert_default_argument(&plan, call);
    attach_conversion_sequence(ctx->result, arg, conversion_sequence(&plan));
    return SEMA_OK;
}

// Check and convert arguments using the signature from the callee's use.
static SemaStatus analyze_call_arguments(SemaContext *ctx, const Node *node,
                                          const SemaType *function)
{
    size_t count = 0;
    SemaStatus status = SEMA_OK;
    for (const Node *arg = node->u.call.args; arg; arg = arg->next) {
        SemaStatus current = analyze_argument(ctx, arg, node, function, count++);
        status = merge_sema_status(status, current);
        if (ctx->scope->visibility_unknown)
            return status;
    }
    if (function && type_has_prototype(function) &&
        (count < function->u.function.parameter_count ||
         (function->u.function.form != SEMA_FUNCTION_VARIADIC &&
          count > function->u.function.parameter_count)))
        return report_invalid_expression(&node->span, EXPRESSION_ARGUMENT_COUNT);
    return status;
}

// Type the call through the value of its callee expression.
static SemaStatus analyze_call_expression(SemaContext *ctx, const Node *node)
{
    const Node *callee = node->u.call.callee;
    SemaStatus status = analyze_value_expression(ctx, callee, node);
    if (ctx->scope->visibility_unknown)
        return status;
    const SemaType *function = NULL;
    if (status == SEMA_OK) {
        const SemaType *pointer = lookup_expression_type(ctx, callee);
        if (pointer->kind == SEMA_TYPE_POINTER && pointer->u.pointee->kind == SEMA_TYPE_FUNCTION)
            function = pointer->u.pointee;
        else
            status = report_invalid_expression(&callee->span, EXPRESSION_INVALID_CALLEE);
    }
    status = merge_sema_status(status, analyze_call_arguments(ctx, node, function));
    if (status != SEMA_OK)
        return status;
    const SemaType *type = function->u.function.return_type;
    if (type->kind != SEMA_TYPE_VOID && !type_is_complete_object(type))
        return report_invalid_expression(&node->span, EXPRESSION_INCOMPLETE_CALL_RESULT);
    return attach_value(ctx, node, type);
}

// Analyze the initializer of an unnamed object in the current lexical scope.
static SemaStatus analyze_compound_literal(SemaContext *ctx, const Node *node)
{
    const SemaType *type;
    SemaStatus status = analyze_type_name(ctx, node->u.compound_literal.type_name, &type);
    const Node *init = node->u.compound_literal.initializer;
    if (status == SEMA_OK && !type_is_complete_object(type) && type->kind != SEMA_TYPE_ARRAY)
        status = report_invalid_expression(&node->span, EXPRESSION_INVALID_COMPOUND_LITERAL);
    if (status != SEMA_OK)
        return ctx->scope->visibility_unknown ? status
            : merge_sema_status(status, analyze_untyped_initializer(ctx, init));
    bool static_duration = ctx->scope->parent == NULL;
    SemaInitializer initializer;
    status = analyze_initializer(ctx, node, init, type,
                                 static_duration, &initializer);
    if (status == SEMA_OK) {
        attach_compound_literal(ctx->result, node,
            (SemaExprProperties){ .type = initializer.type, .category = SEMA_LVALUE }, static_duration);
        attach_initializer(ctx->result, node, &initializer);
    }
    return status;
}

// Check whether a generic association repeats an already seen compatible type.
static bool check_generic_duplicate(const SemaType *const *types, size_t count, const SemaType *type)
{
    for (size_t i = 0; i < count; ++i)
        if (type_is_compatible(types[i], type))
            return true;
    return false;
}

// Analyze an association type independently of its result expression.
static SemaStatus analyze_generic_association_type(SemaContext *ctx, const GenericAssociation *a,
                                                   const SemaType *const *types, size_t used,
                                                   const SemaType **out)
{
    const SemaType *type;
    SemaStatus status = analyze_type_name(ctx, a->type_name, &type);
    if (status != SEMA_OK)
        return status;
    if (!type_is_complete_object(type))
        return report_invalid_expression(&a->span, EXPRESSION_GENERIC_TYPE);
    if (check_generic_duplicate(types, used, type))
        return report_invalid_expression(&a->span, EXPRESSION_GENERIC_DUPLICATE);
    *out = type;
    return SEMA_OK;
}

// Check independent associations in source order while names remain resolvable.
static SemaStatus analyze_generic_associations(SemaContext *ctx, const Node *node,
                                               const SemaType *control, const Node **out)
{
    const GenericAssociation *first = node->u.generic_selection.associations;
    size_t count = 0;
    for (const GenericAssociation *a = first; a; a = a->next)
        ++count;
    const SemaType **types = xcalloc(count, sizeof(*types));
    size_t used = 0;
    const Node *selected = NULL;
    const Node *fallback = NULL;
    SemaStatus status = SEMA_OK;
    for (const GenericAssociation *a = first; a; a = a->next) {
        SemaStatus association = SEMA_OK;
        if (a->type_name) {
            const SemaType *type;
            association = analyze_generic_association_type(ctx, a, types, used, &type);
            if (association == SEMA_OK) {
                types[used++] = type;
                if (control && type_is_compatible(control, type)) {
                    if (selected)
                        association = report_invalid_expression(&a->span, EXPRESSION_GENERIC_MULTIPLE_MATCHES);
                    else
                        selected = a->expression;
                }
            }
        } else if (fallback) {
            association = report_invalid_expression(&a->span, EXPRESSION_GENERIC_DEFAULT);
        } else {
            fallback = a->expression;
        }
        status = merge_sema_status(status, association);
        if (ctx->scope->visibility_unknown)
            break;
        status = merge_sema_status(status, analyze_expression(ctx, a->expression));
        if (ctx->scope->visibility_unknown)
            break;
    }
    free(types);
    if (status != SEMA_OK || !control)
        return status;
    *out = selected ? selected : fallback;
    return *out ? SEMA_OK : report_invalid_expression(&node->span, EXPRESSION_GENERIC_NO_MATCH);
}

// Apply type-only conversions for matching and inherit the selected designator.
static SemaStatus analyze_generic_selection(SemaContext *ctx, const Node *node)
{
    const Node *control = node->u.generic_selection.control;
    SemaStatus status = analyze_expression(ctx, control);
    if (ctx->scope->visibility_unknown)
        return status;
    SemaConversionPlan plan = {0};
    if (status == SEMA_OK)
        plan = convert_value(ctx->result, node, lookup_expression_value(ctx, control));
    const Node *selected;
    status = merge_sema_status(status,
        analyze_generic_associations(ctx, node, plan.value.type, &selected));
    if (status == SEMA_OK) {
        const SemaExprInfo *info = sema_lookup_expression(ctx->result, selected);
        attach_conversion_sequence(ctx->result, control, conversion_sequence(&plan));
        attach_generic_selection(ctx->result, node, info->original, selected);
    }
    return status;
}

// Check a value read, allowing incomplete arrays to decay to element pointers.
static bool check_readable_value(SemaExprProperties source)
{
    return source.category != SEMA_LVALUE || source.type->kind == SEMA_TYPE_ARRAY ||
           type_is_complete_object(source.type);
}

// ---------------------------------------------------------------------------
// Expression analysis interfaces
// ---------------------------------------------------------------------------

// Dispatch syntax. Recursive analysis descends only through proper AST children.
SemaStatus analyze_expression(SemaContext *ctx, const Node *node)
{
    switch (node->type) {
    case ND_NUM: return analyze_literal(ctx, node);
    case ND_IDENTIFIER: return analyze_identifier(ctx, node);
    case ND_UNARY: return analyze_unary_expression(ctx, node);
    case ND_BINARY: return analyze_binary_expression(ctx, node);
    case ND_ASSIGN: return analyze_assignment_expression(ctx, node);
    case ND_COND: return analyze_conditional_expression(ctx, node);
    case ND_CAST: return analyze_cast_expression(ctx, node);
    case ND_CALL: return analyze_call_expression(ctx, node);
    case ND_SIZEOF_TYPE: return analyze_sizeof_expression(ctx, node);
    case ND_ALIGNOF_TYPE: return analyze_alignof_expression(ctx, node);
    case ND_STRING: return analyze_string_literal(ctx, node);
    case ND_SUBSCRIPT: return analyze_subscript_expression(ctx, node);
    case ND_MEMBER: return analyze_member_expression(ctx, node);
    case ND_COMPOUND_LITERAL: return analyze_compound_literal(ctx, node);
    case ND_GENERIC_SELECTION: return analyze_generic_selection(ctx, node);
    default: return mark_unsupported(ctx, true);
    }
}

// Analyze an expression in a context that reads an ordinary value.
SemaStatus analyze_value_expression(SemaContext *ctx, const Node *node, const Node *use)
{
    SemaStatus status = analyze_expression(ctx, node);
    if (status != SEMA_OK)
        return status;
    SemaExprProperties source = lookup_expression_value(ctx, node);
    if (!check_readable_value(source))
        return report_invalid_expression(&node->span, EXPRESSION_INCOMPLETE_VALUE);
    SemaConversionPlan plan = convert_value(ctx->result, use, source);
    attach_conversion_sequence(ctx->result, node, conversion_sequence(&plan));
    return SEMA_OK;
}

// Check and publish the conversion of an already analyzed assignment operand.
SemaStatus analyze_typed_assignment_value(SemaContext *ctx, const Node *node,
                                          const Node *use, const SemaType *target)
{
    SemaExprProperties source = lookup_expression_value(ctx, node);
    if (!check_readable_value(source))
        return report_invalid_expression(&node->span, EXPRESSION_INCOMPLETE_VALUE);
    const SemaType *destination = new_qualified_type(ctx->result, target, 0);
    bool null_pointer_constant = destination->kind == SEMA_TYPE_POINTER &&
                                 eval_is_null_pointer_constant(ctx->result, node);
    SemaConversionPlan plan;
    if (!convert_assignment_value(ctx->result, use, source, destination, null_pointer_constant, &plan))
        return report_invalid_expression(&node->span, EXPRESSION_INCOMPATIBLE_CONVERSION);
    attach_conversion_sequence(ctx->result, node, conversion_sequence(&plan));
    return SEMA_OK;
}

// Analyze and check the source of one assignment-like conversion.
SemaStatus analyze_assignment_value(SemaContext *ctx, const Node *node,
                                    const Node *use, const SemaType *target)
{
    SemaStatus status = analyze_expression(ctx, node);
    return status == SEMA_OK ? analyze_typed_assignment_value(ctx, node, use, target) : status;
}
