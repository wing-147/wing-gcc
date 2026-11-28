// Conversion rules derive ordered steps from typed operands for the caller to commit.

#include "sema_internal.h"

typedef enum {
    POINTER_POINTEE_INCOMPATIBLE,
    POINTER_POINTEE_COMPOSITE,
    POINTER_POINTEE_VOID,
} PointerPointee;

// Promote bit-fields by their representable range and other values by their type.
static const SemaType *type_value_integer_promotion(SemaExprProperties value)
{
    if (value.bit_field && type_is_integer(value.type) &&
        value.bit_field->u.bit_field.width < type_integer_traits(type_builtin(SEMA_TYPE_INT)).width)
        return type_builtin(SEMA_TYPE_INT);
    return type_integer_promotion(value.type);
}

// Choose the common type of two promoted integers using rank and range.
static const SemaType *common_integer_type(const SemaType *lhs, const SemaType *rhs)
{
    IntegerTraits left = type_integer_traits(lhs);
    IntegerTraits right = type_integer_traits(rhs);
    if (left.is_unsigned == right.is_unsigned)
        return left.rank >= right.rank ? lhs : rhs;

    const SemaType *signed_type = left.is_unsigned ? rhs : lhs;
    const SemaType *unsigned_type = left.is_unsigned ? lhs : rhs;
    IntegerTraits signed_traits = type_integer_traits(signed_type);
    IntegerTraits unsigned_traits = type_integer_traits(unsigned_type);
    if (unsigned_traits.rank >= signed_traits.rank)
        return unsigned_type;
    if (signed_traits.width > unsigned_traits.width)
        return signed_type;
    return type_builtin(signed_traits.unsigned_kind);
}

// Find the corresponding real floating rank, with zero for integers.
static unsigned floating_rank(const SemaType *type)
{
    switch (type->kind) {
    case SEMA_TYPE_FLOAT:
    case SEMA_TYPE_FLOAT_COMPLEX: return 1;
    case SEMA_TYPE_DOUBLE:
    case SEMA_TYPE_DOUBLE_COMPLEX: return 2;
    case SEMA_TYPE_LDOUBLE:
    case SEMA_TYPE_LDOUBLE_COMPLEX: return 3;
    default: return 0;
    }
}

// Select a floating type of this rank in the designated domain.
static const SemaType *type_at_floating_rank(unsigned rank, bool is_complex)
{
    static const SemaTypeKind g_real[] = {
        SEMA_TYPE_FLOAT, SEMA_TYPE_DOUBLE, SEMA_TYPE_LDOUBLE,
    };
    static const SemaTypeKind g_complex[] = {
        SEMA_TYPE_FLOAT_COMPLEX, SEMA_TYPE_DOUBLE_COMPLEX, SEMA_TYPE_LDOUBLE_COMPLEX,
    };
    return type_builtin((is_complex ? g_complex : g_real)[rank - 1]);
}

// Determine promotions and common types without changing expression records.
static SemaArithmeticPlan plan_usual_arithmetic(SemaExprProperties lhs,
                                                 SemaExprProperties rhs)
{
    const SemaType *left = lhs.type;
    const SemaType *right = rhs.type;
    SemaArithmeticPlan plan = {
        .left.source = left, .right.source = right,
    };
    unsigned left_rank = floating_rank(left);
    unsigned right_rank = floating_rank(right);
    unsigned rank = left_rank > right_rank ? left_rank : right_rank;
    if (rank) {
        bool left_complex = !type_is_real(left);
        plan.left.converted = type_at_floating_rank(rank, left_complex);
        plan.right.converted = type_at_floating_rank(rank, !type_is_real(right));
        plan.result = left_complex ? plan.left.converted : plan.right.converted;
    } else {
        plan.left.promoted = type_value_integer_promotion(lhs);
        plan.right.promoted = type_value_integer_promotion(rhs);
        plan.result = common_integer_type(plan.left.promoted, plan.right.promoted);
        plan.left.converted = plan.result;
        plan.right.converted = plan.result;
    }
    return plan;
}

// Check pointer arithmetic and return the pointer or ptrdiff_t result type.
static const SemaType *resolve_pointer_arithmetic(Operator op, const SemaType *left,
                                                  const SemaType *right)
{
    bool left_pointer = left->kind == SEMA_TYPE_POINTER &&
                        type_is_complete_object(left->u.pointee);
    bool right_pointer = right->kind == SEMA_TYPE_POINTER &&
                         type_is_complete_object(right->u.pointee);
    if (left_pointer && type_is_integer(right))
        return left;
    if (op == BINARY_ADD && right_pointer && type_is_integer(left))
        return right;
    if (op == BINARY_SUB && left_pointer && right_pointer &&
        type_is_compatible_unqualified(left->u.pointee, right->u.pointee))
        return type_builtin(SEMA_TYPE_LONG);
    return NULL;
}

// Check pointer assignment at the immediate pointed-to level. Compatibility
// still compares qualifiers at every deeper level.
static bool check_pointer_assignment(const SemaType *target, const SemaType *source)
{
    const SemaType *dst = target->u.pointee;
    const SemaType *src = source->u.pointee;
    if (src->qualifiers & ~dst->qualifiers)
        return false;
    return type_is_compatible_unqualified(dst, src) ||
           (dst->kind == SEMA_TYPE_VOID && type_is_object(src)) ||
           (src->kind == SEMA_TYPE_VOID && type_is_object(dst));
}

// Append one nonidentity step to a caller-owned plan and advance its endpoint.
static void convert_expression(SemaConversionPlan *plan, const SemaType *target,
                               SemaConversionKind kind, const Node *use)
{
    SemaExprProperties source = plan->value;
    if (source.category == SEMA_VALUE && type_is_identical(source.type, target))
        return;
    if (plan->count == SEMA_MAX_CONVERSION_STEPS)
        unreachable();
    plan->value = (SemaExprProperties){ .type = target, .category = SEMA_VALUE };
    if (kind == SEMA_CONVERSION_LVALUE_TO_VALUE)
        plan->value.bit_field = source.bit_field;
    plan->steps[plan->count++] = (SemaConversion){
        .kind = kind, .from = source, .to = plan->value, .use = use,
    };
}

// Describe the promotions and common-type conversion of an already-read operand.
static void convert_arithmetic_operand(SemaConversionPlan *plan,
                                        SemaArithmeticOperand operand, const Node *use)
{
    if (operand.promoted)
        convert_expression(plan, operand.promoted, SEMA_CONVERSION_INTEGER_PROMOTION, use);
    convert_expression(plan, operand.converted, SEMA_CONVERSION_USUAL_ARITHMETIC, use);
}

// Interpret the two pointees under the selected pointer conversion rule.
static PointerPointee resolve_common_pointee(const SemaType *a, const SemaType *b,
                                             bool allow_void)
{
    if (type_is_compatible_unqualified(a, b) && (allow_void || type_is_object(a)))
        return POINTER_POINTEE_COMPOSITE;
    if (allow_void && ((a->kind == SEMA_TYPE_VOID && type_is_object(b)) ||
                       (b->kind == SEMA_TYPE_VOID && type_is_object(a))))
        return POINTER_POINTEE_VOID;
    return POINTER_POINTEE_INCOMPATIBLE;
}

// Construct the chosen common pointer with both immediate pointee qualifiers.
static const SemaType *new_common_pointer_type(SemaResult *result,
                                               const SemaType *a, const SemaType *b,
                                               PointerPointee pointee)
{
    const SemaType *base;
    if (pointee == POINTER_POINTEE_COMPOSITE) {
        const SemaType *unqualified_a = new_qualified_type(result, a, 0);
        const SemaType *unqualified_b = new_qualified_type(result, b, 0);
        base = new_composite_type(result, unqualified_a, unqualified_b);
    } else {
        base = type_builtin(SEMA_TYPE_VOID);
    }
    base = new_qualified_type(result, base, a->qualifiers | b->qualifiers);
    return new_pointer_type(result, base, 0);
}

// Describe a value read or function decay. The result owns any derived type.
SemaConversionPlan convert_value(SemaResult *result, const Node *use,
                                 SemaExprProperties source)
{
    SemaConversionPlan plan = { .value = source };
    if (source.type->kind == SEMA_TYPE_ARRAY) {
        const SemaType *type = new_pointer_type(result, source.type->u.array.element, 0);
        convert_expression(&plan, type, SEMA_CONVERSION_ARRAY_TO_POINTER, use);
        return plan;
    }
    switch (source.category) {
    case SEMA_VALUE:
        return plan;
    case SEMA_LVALUE: {
        const SemaType *type = new_qualified_type(result, source.type, 0);
        SemaConversionKind kind = source.type->qualifiers & TYPE_QUAL_ATOMIC
            ? SEMA_CONVERSION_ATOMIC_LOAD : SEMA_CONVERSION_LVALUE_TO_VALUE;
        convert_expression(&plan, type, kind, use);
        return plan;
    }
    case SEMA_FUNCTION_DESIGNATOR: {
        const SemaType *type = new_pointer_type(result, source.type, 0);
        convert_expression(&plan, type, SEMA_CONVERSION_FUNCTION_TO_POINTER, use);
        return plan;
    }
    }
    unreachable();
}

// Describe the integer promotion required by a use of an already-read operand.
void convert_integer_promotion(SemaConversionPlan *plan, const Node *use)
{
    convert_expression(plan, type_value_integer_promotion(plan->value),
                       SEMA_CONVERSION_INTEGER_PROMOTION, use);
}

// Describe a checked case constant's conversion to the switch comparison type.
void convert_case_value(SemaConversionPlan *plan, const Node *use, const SemaType *target)
{
    convert_expression(plan, target, SEMA_CONVERSION_CASE, use);
}

// Check one arithmetic operator and plan the effective types for both operands.
bool plan_arithmetic(Operator op, SemaExprProperties lhs, SemaExprProperties rhs,
                     SemaArithmeticPlan *out)
{
    const SemaType *left = lhs.type;
    const SemaType *right = rhs.type;
    switch (op) {
    case BINARY_ADD:
    case BINARY_SUB:
        if (left->kind == SEMA_TYPE_POINTER || right->kind == SEMA_TYPE_POINTER) {
            const SemaType *type = resolve_pointer_arithmetic(op, left, right);
            if (!type)
                return false;
            *out = (SemaArithmeticPlan){
                .left = { .source = left, .converted = left },
                .right = { .source = right, .converted = right },
                .result = type,
            };
            return true;
        }
        break;
    case BINARY_SHL:
    case BINARY_SHR: {
        if (!type_is_integer(left) || !type_is_integer(right))
            return false;
        const SemaType *promoted_left = type_value_integer_promotion(lhs);
        const SemaType *promoted_right = type_value_integer_promotion(rhs);
        *out = (SemaArithmeticPlan){
            .left = { left, promoted_left, promoted_left },
            .right = { right, promoted_right, promoted_right },
            .result = promoted_left,
        };
        return true;
    }
    case BINARY_REM:
    case BINARY_BITAND:
    case BINARY_BITXOR:
    case BINARY_BITOR:
        if (!type_is_integer(left) || !type_is_integer(right))
            return false;
        break;
    case BINARY_MUL:
    case BINARY_DIV:
        break;
    default:
        return false;
    }
    if (!type_is_arithmetic(left) || !type_is_arithmetic(right))
        return false;
    *out = plan_usual_arithmetic(lhs, rhs);
    return true;
}

// Describe both sides of a checked computation in the caller's operand plans.
void convert_arithmetic_operands(const SemaArithmeticPlan *arithmetic, const Node *use,
                                 SemaOperandConversions *operands)
{
    convert_arithmetic_operand(&operands->left, arithmetic->left, use);
    convert_arithmetic_operand(&operands->right, arithmetic->right, use);
}

// Describe usual arithmetic conversions for a use with two arithmetic operands.
const SemaType *convert_usual_arithmetic(const Node *use, SemaOperandConversions *operands)
{
    SemaArithmeticPlan arithmetic = plan_usual_arithmetic(operands->left.value,
                                                         operands->right.value);
    convert_arithmetic_operands(&arithmetic, use, operands);
    return arithmetic.result;
}

// Derive common pointer conversions from typed operands. The result owns any
// new type. The caller owns the steps and can publish them after success.
const SemaType *convert_pointer_operands(SemaResult *result, const Node *use,
                                         SemaPointerConversionContext context,
                                         SemaOperandConversions *operands)
{
    const SemaType *left = operands->left.value.type;
    const SemaType *right = operands->right.value.type;
    if (context.allow_void_or_null && left->kind == SEMA_TYPE_POINTER && context.right_is_null) {
        convert_expression(&operands->right, left, SEMA_CONVERSION_NULL_POINTER, use);
        return left;
    }
    if (context.allow_void_or_null && right->kind == SEMA_TYPE_POINTER && context.left_is_null) {
        convert_expression(&operands->left, right, SEMA_CONVERSION_NULL_POINTER, use);
        return right;
    }
    if (left->kind != SEMA_TYPE_POINTER || right->kind != SEMA_TYPE_POINTER)
        return NULL;

    const SemaType *a = left->u.pointee;
    const SemaType *b = right->u.pointee;
    PointerPointee pointee = resolve_common_pointee(a, b, context.allow_void_or_null);
    if (pointee == POINTER_POINTEE_INCOMPATIBLE)
        return NULL;
    const SemaType *type = new_common_pointer_type(result, a, b, pointee);
    convert_expression(&operands->left, type, SEMA_CONVERSION_POINTER, use);
    convert_expression(&operands->right, type, SEMA_CONVERSION_POINTER, use);
    return type;
}

// Extend the planned read with computation and writeback for one designator.
void convert_update(const SemaArithmeticPlan *arithmetic, const Node *use,
                    SemaUpdateConversions *update)
{
    convert_arithmetic_operand(&update->target_read, arithmetic->left, use);
    convert_arithmetic_operand(&update->rhs, arithmetic->right, use);
    update->writeback = (SemaConversionPlan){
        .value = { .type = arithmetic->result, .category = SEMA_VALUE },
    };
    convert_expression(&update->writeback, arithmetic->left.source,
                       SEMA_CONVERSION_ASSIGNMENT, use);
}

// Check both types and describe the full assignment-like chain on success.
// Leave the output untouched if the constraint fails.
bool convert_assignment_value(SemaResult *result, const Node *use,
                              SemaExprProperties source, const SemaType *target,
                              bool null_pointer_constant, SemaConversionPlan *out)
{
    SemaConversionPlan plan = convert_value(result, use, source);
    const SemaType *source_type = plan.value.type;
    SemaConversionKind kind = SEMA_CONVERSION_ASSIGNMENT;
    if (target->kind == SEMA_TYPE_POINTER) {
        if (null_pointer_constant)
            kind = SEMA_CONVERSION_NULL_POINTER;
        else if (source_type->kind == SEMA_TYPE_POINTER &&
                 check_pointer_assignment(target, source_type))
            kind = SEMA_CONVERSION_POINTER;
        else
            return false;
    } else if (!((target->kind == SEMA_TYPE_STRUCT || target->kind == SEMA_TYPE_UNION) &&
                 type_is_compatible_unqualified(target, source_type)) &&
               !(type_is_arithmetic(target) && type_is_arithmetic(source_type)) &&
               !(target->kind == SEMA_TYPE_BOOL && source_type->kind == SEMA_TYPE_POINTER)) {
        return false;
    }
    convert_expression(&plan, target, kind, use);
    *out = plan;
    return true;
}

// Describe the default promotions of an already-read argument.
void convert_default_argument(SemaConversionPlan *plan, const Node *use)
{
    const SemaType *type = plan->value.bit_field ? type_value_integer_promotion(plan->value)
        : type_default_argument_promotion(plan->value.type);
    convert_expression(plan, type,
                       SEMA_CONVERSION_DEFAULT_ARGUMENT, use);
}
