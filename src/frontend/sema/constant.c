// Constant evaluation reads typed AST facts. Target integer arithmetic is
// performed without signed overflow in the compiler's own execution.

#include "sema_internal.h"

#include <float.h>
#include <math.h>
#include <stdint.h>

__extension__ typedef __int128 WideInt;

typedef struct {
    const SemaType *type;
    uint64_t bits;
    long double _Complex real_or_complex;
} Constant;

static bool eval_arithmetic(const SemaResult *result, const Node *node, Constant *out,
                            SemaIntegerConstantStatus *failure);
static bool eval_address_constant(const SemaResult *result, const Node *node);

// ---------------------------------------------------------------------------
// Arithmetic constant evaluation
// ---------------------------------------------------------------------------

// Interpret the target's two's-complement integer bits in a wider host type.
static WideInt integer_value(Constant value)
{
    IntegerTraits traits = type_integer_traits(value.type);
    WideInt bits = value.bits;
    WideInt sign_bit = (WideInt)1 << (traits.width - 1);
    if (!traits.is_unsigned && bits >= sign_bit)
        bits -= sign_bit * 2;
    return bits;
}

// Form the target representation after an integer conversion.
static Constant integer_bits(const SemaType *type, WideInt value)
{
    unsigned width = type_integer_traits(type).width;
    uint64_t mask = UINT64_MAX >> (64 - width);
    return (Constant){ .type = type, .bits = (uint64_t)value & mask };
}

// Check the signed result range before recording a target arithmetic result.
static bool eval_integer_result(const SemaType *type, WideInt value, Constant *out)
{
    IntegerTraits traits = type_integer_traits(type);
    WideInt limit = (WideInt)1 << (traits.width - 1);
    if (!traits.is_unsigned && (value < -limit || value >= limit))
        return false;
    *out = integer_bits(type, value);
    return true;
}

// Read scalar truth from a known arithmetic constant.
static bool constant_truth(Constant value)
{
    if (type_is_integer(value.type))
        return value.bits != 0;
    return value.real_or_complex != 0.0L;
}

// Round one floating component to the target width. Target overflow selects
// a signed infinity without casting a finite out-of-range host value.
static long double eval_floating_component_conversion(long double value, SemaTypeKind kind)
{
    if (kind == SEMA_TYPE_FLOAT || kind == SEMA_TYPE_FLOAT_COMPLEX) {
        if (value > FLT_MAX)
            return HUGE_VALL;
        if (value < -FLT_MAX)
            return -HUGE_VALL;
        return (float)value;
    }
    if (kind == SEMA_TYPE_DOUBLE || kind == SEMA_TYPE_DOUBLE_COMPLEX) {
        if (value > DBL_MAX)
            return HUGE_VALL;
        if (value < -DBL_MAX)
            return -HUGE_VALL;
        return (double)value;
    }
    return value;
}

// Apply a numeric conversion. Reject a floating-to-integer value outside
// the destination's range before using the corresponding host cast.
static bool eval_constant_conversion(Constant source, const SemaType *target, Constant *out)
{
    if (target->kind == SEMA_TYPE_BOOL) {
        *out = integer_bits(target, constant_truth(source));
        return true;
    }
    if (type_is_integer(target)) {
        if (type_is_integer(source.type)) {
            *out = integer_bits(target, integer_value(source));
            return true;
        }
        IntegerTraits traits = type_integer_traits(target);
        WideInt limit = (WideInt)1 << (traits.width - !traits.is_unsigned);
        long double low = traits.is_unsigned ? 0.0L : -(long double)limit;
        long double number = __real__ source.real_or_complex;
        if (!(number >= low ? number < (long double)limit : number > low - 1.0L))
            return false;
        *out = integer_bits(target, (WideInt)number);
        return true;
    }
    if (!type_is_arithmetic(target))
        return false;

    long double _Complex value = type_is_integer(source.type)
                                     ? (long double)integer_value(source)
                                     : source.real_or_complex;
    if (type_is_real(target))
        __imag__ value = 0;
    if (isnan(__real__ value) || isnan(__imag__ value))
        return false;
    __real__ value = eval_floating_component_conversion(__real__ value, target->kind);
    __imag__ value = eval_floating_component_conversion(__imag__ value, target->kind);
    *out = (Constant){ .type = target, .real_or_complex = value };
    return true;
}

// Read an operand and replay the implicit arithmetic conversions at its use.
static bool eval_operand(const SemaResult *result, const Node *node, Constant *out,
                         SemaIntegerConstantStatus *failure)
{
    if (!eval_arithmetic(result, node, out, failure))
        return false;
    const SemaExprInfo *info = sema_lookup_expression(result, node);
    for (size_t i = 0; i < info->conversions.count; ++i)
        if (!eval_constant_conversion(*out, info->conversions.steps[i].to.type, out))
            return false;
    return true;
}

// Check the permitted operands of a C11 integer or arithmetic constant
// expression. Recurse through proper operands and treat sizeof as a leaf.
static bool check_constant_operands(const SemaResult *result, const Node *node,
                                     bool integer_only)
{
    const SemaType *type = sema_lookup_expression(result, node)->original.type;
    if (integer_only ? !type_is_integer(type) : !type_is_arithmetic(type))
        return false;
    switch (node->type) {
    case ND_NUM:
    case ND_SIZEOF_TYPE:
    case ND_ALIGNOF_TYPE:
        return true;
    case ND_IDENTIFIER: {
        const SemaSymbol *symbol = sema_lookup_expression(result, node)->u.symbol;
        return symbol->kind == SEMA_SYMBOL_ENUMERATOR;
    }
    case ND_UNARY:
        return node->u.unary.op == UNARY_SIZEOF || node->u.unary.op == UNARY_ALIGNOF ||
               check_constant_operands(result, node->u.unary.operand, integer_only);
    case ND_CAST: {
        const Node *source = node->u.cast.expr;
        if (integer_only && source->type == ND_NUM)
            return true;
        return check_constant_operands(result, source, integer_only);
    }
    case ND_BINARY:
        return check_constant_operands(result, node->u.binary.lhs, integer_only) &&
               check_constant_operands(result, node->u.binary.rhs, integer_only);
    case ND_COND:
        return check_constant_operands(result, node->u.cond.cond, integer_only) &&
               check_constant_operands(result, node->u.cond.then_expr, integer_only) &&
               check_constant_operands(result, node->u.cond.else_expr, integer_only);
    case ND_GENERIC_SELECTION:
        return check_constant_operands(result, sema_lookup_expression(result, node)->u.selected,
                                       integer_only);
    default:
        return false;
    }
}

// Evaluate a unary operator using the operand's converted type and value.
static bool eval_unary(const SemaResult *result, const Node *node,
                       const SemaType *type, Constant *out,
                       SemaIntegerConstantStatus *failure)
{
    Operator op = node->u.unary.op;
    if (op == UNARY_SIZEOF) {
        *out = integer_bits(type, sema_lookup_expression(result, node)->u.sizeof_value);
        return true;
    }
    if (op == UNARY_ALIGNOF) {
        *out = integer_bits(type, sema_lookup_expression(result, node)->u.alignof_value);
        return true;
    }
    Constant operand;
    if (!eval_operand(result, node->u.unary.operand, &operand, failure))
        return false;
    switch (op) {
    case UNARY_PLUS:
        *out = operand;
        return true;
    case UNARY_LOGNOT:
        *out = integer_bits(type, !constant_truth(operand));
        return true;
    case UNARY_BITNOT:
        *out = integer_bits(type, ~integer_value(operand));
        return true;
    case UNARY_NEG:
        if (type_is_integer(type))
            return eval_integer_result(type, -integer_value(operand), out);
        operand.real_or_complex = -operand.real_or_complex;
        *out = operand;
        return true;
    default:
        return false;
    }
}

// Evaluate a target shift after validating the width and signed operand.
static bool eval_shift(Operator op, Constant lhs, WideInt rhs, Constant *out)
{
    IntegerTraits traits = type_integer_traits(lhs.type);
    if (rhs < 0 || rhs >= traits.width)
        return false;
    WideInt left = integer_value(lhs);
    if (op == BINARY_SHR)
        return eval_integer_result(lhs.type, left >> (unsigned)rhs, out);
    if (left < 0)
        return false;
    return eval_integer_result(lhs.type, left << (unsigned)rhs, out);
}

// Evaluate integer operators with target widths and checked signed arithmetic.
static bool eval_integer_binary(Operator op, Constant lhs, Constant rhs,
                                 const SemaType *type, Constant *out)
{
    WideInt a = integer_value(lhs);
    WideInt b = integer_value(rhs);
    WideInt value;
    switch (op) {
    case BINARY_ADD: value = a + b; break;
    case BINARY_SUB: value = a - b; break;
    case BINARY_MUL:
        value = type_integer_traits(type).is_unsigned
                    ? (WideInt)(lhs.bits * rhs.bits) : a * b;
        break;
    case BINARY_DIV:
    case BINARY_REM: {
        if (!b)
            return false;
        Constant quotient;
        if (!eval_integer_result(type, a / b, &quotient))
            return false;
        value = op == BINARY_DIV ? a / b : a % b;
        break;
    }
    case BINARY_SHL:
    case BINARY_SHR:
        return eval_shift(op, lhs, b, out);
    case BINARY_BITAND: value = a & b; break;
    case BINARY_BITXOR: value = a ^ b; break;
    case BINARY_BITOR: value = a | b; break;
    case BINARY_LT: value = a < b; break;
    case BINARY_LE: value = a <= b; break;
    case BINARY_GT: value = a > b; break;
    case BINARY_GE: value = a >= b; break;
    case BINARY_EQ: value = a == b; break;
    case BINARY_NE: value = a != b; break;
    default: return false;
    }
    return eval_integer_result(type, value, out);
}

// Evaluate real or complex operations in the converted operand domains.
static bool eval_floating_binary(Operator op, Constant lhs, Constant rhs,
                                  const SemaType *type, Constant *out)
{
    long double _Complex a = lhs.real_or_complex;
    long double _Complex b = rhs.real_or_complex;
    long double _Complex value;
    if (type_is_integer(type)) {
        bool comparison;
        switch (op) {
        case BINARY_LT: comparison = __real__ a < __real__ b; break;
        case BINARY_LE: comparison = __real__ a <= __real__ b; break;
        case BINARY_GT: comparison = __real__ a > __real__ b; break;
        case BINARY_GE: comparison = __real__ a >= __real__ b; break;
        case BINARY_EQ: comparison = a == b; break;
        case BINARY_NE: comparison = a != b; break;
        default: return false;
        }
        *out = integer_bits(type, comparison);
        return true;
    }
    switch (op) {
    case BINARY_ADD: value = a + b; break;
    case BINARY_SUB: value = a - b; break;
    case BINARY_MUL: value = a * b; break;
    case BINARY_DIV:
        if (b == 0.0L)
            return false;
        value = a / b;
        break;
    default: return false;
    }
    return eval_constant_conversion((Constant){ .type = type, .real_or_complex = value }, type, out);
}

// Evaluate a binary node while respecting the logical evaluation boundary.
static bool eval_binary(const SemaResult *result, const Node *node,
                        const SemaType *type, Constant *out,
                        SemaIntegerConstantStatus *failure)
{
    Operator op = node->u.binary.op;
    if (op == BINARY_COMMA) {
        if (failure)
            *failure = SEMA_INTEGER_CONSTANT_INVALID_OPERANDS;
        return false;
    }
    Constant lhs;
    if (!eval_operand(result, node->u.binary.lhs, &lhs, failure))
        return false;
    if (op == BINARY_LOGAND || op == BINARY_LOGOR) {
        bool truth = constant_truth(lhs);
        if ((op == BINARY_LOGAND && truth) || (op == BINARY_LOGOR && !truth)) {
            Constant rhs;
            if (!eval_operand(result, node->u.binary.rhs, &rhs, failure))
                return false;
            truth = constant_truth(rhs);
        }
        *out = integer_bits(type, truth);
        return true;
    }

    Constant rhs;
    if (!eval_operand(result, node->u.binary.rhs, &rhs, failure))
        return false;
    return type_is_integer(lhs.type) && type_is_integer(rhs.type)
               ? eval_integer_binary(op, lhs, rhs, type, out)
               : eval_floating_binary(op, lhs, rhs, type, out);
}

// Evaluate a typed arithmetic tree. Operand calls descend to proper AST
// children and the selected conditional operand alone contributes a value.
// The optional failure output distinguishes a reached forbidden operator.
static bool eval_arithmetic(const SemaResult *result, const Node *node, Constant *out,
                            SemaIntegerConstantStatus *failure)
{
    const SemaExprInfo *info = sema_lookup_expression(result, node);
    const SemaType *type = info->original.type;
    if (!type_is_arithmetic(type))
        return false;
    switch (node->type) {
    case ND_NUM:
        if (type_is_integer(type)) {
            *out = integer_bits(type, (uint64_t)node->u.num.value.integer);
            return true;
        }
        return eval_constant_conversion(
            (Constant){ .type = type, .real_or_complex = node->u.num.value.floating }, type, out);
    case ND_SIZEOF_TYPE:
        *out = integer_bits(type, info->u.sizeof_value);
        return true;
    case ND_ALIGNOF_TYPE:
        *out = integer_bits(type, info->u.alignof_value);
        return true;
    case ND_IDENTIFIER:
        if (info->u.symbol->kind != SEMA_SYMBOL_ENUMERATOR)
            return false;
        *out = integer_bits(type, info->u.symbol->u.enumerator_value);
        return true;
    case ND_UNARY:
        return eval_unary(result, node, type, out, failure);
    case ND_BINARY:
        return eval_binary(result, node, type, out, failure);
    case ND_CAST: {
        Constant operand;
        return eval_operand(result, node->u.cast.expr, &operand, failure) &&
               eval_constant_conversion(operand, type, out);
    }
    case ND_COND: {
        Constant control;
        if (!eval_operand(result, info->u.condition.expression, &control, failure))
            return false;
        const Node *selected = constant_truth(control) ? node->u.cond.then_expr
                                                       : node->u.cond.else_expr;
        return eval_operand(result, selected, out, failure) &&
               eval_constant_conversion(*out, type, out);
    }
    case ND_GENERIC_SELECTION:
        return eval_arithmetic(result, info->u.selected, out, failure);
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Address constant evaluation
// ---------------------------------------------------------------------------

// Recognize a designator rooted in static storage or a function. Member,
// subscript and indirection cases descend through proper AST operands.
static bool eval_address_designator(const SemaResult *result, const Node *node)
{
    const SemaExprInfo *info = sema_lookup_expression(result, node);
    switch (node->type) {
    case ND_IDENTIFIER: {
        const SemaSymbol *symbol = info->u.symbol;
        return symbol->kind == SEMA_SYMBOL_FUNCTION ||
               (symbol->kind == SEMA_SYMBOL_OBJECT && symbol->entity &&
                symbol->entity->u.object.duration == SEMA_DURATION_STATIC);
    }
    case ND_STRING:
        return true;
    case ND_COMPOUND_LITERAL:
        return info->u.static_duration;
    case ND_GENERIC_SELECTION:
        return eval_address_designator(result, info->u.selected);
    case ND_MEMBER:
        return node->u.member_access.kind == MEMBER_INDIRECT
            ? eval_address_constant(result, node->u.member_access.base)
            : eval_address_designator(result, node->u.member_access.base);
    case ND_SUBSCRIPT: {
        const Node *pointer = node->u.subscript.base;
        const Node *index = node->u.subscript.index;
        const SemaExprInfo *index_info = sema_lookup_expression(result, index);
        if (sema_converted_properties(index_info).type->kind == SEMA_TYPE_POINTER) {
            pointer = index;
            index = node->u.subscript.base;
        }
        SemaIntegerConstant offset;
        return eval_address_constant(result, pointer) &&
               eval_integer_constant(result, index, &offset) == SEMA_INTEGER_CONSTANT_OK;
    }
    case ND_UNARY:
        return node->u.unary.op == UNARY_DEREF && eval_address_constant(result, node->u.unary.operand);
    default:
        return false;
    }
}

// Recognize an address constant. Recursive address, cast and additive forms
// descend through proper AST children.
static bool eval_address_constant(const SemaResult *result, const Node *node)
{
    const SemaExprInfo *info = sema_lookup_expression(result, node);
    if (info->kind == SEMA_EXPR_IDENTIFIER && info->u.symbol->kind == SEMA_SYMBOL_FUNCTION)
        return true;
    if (node->type == ND_UNARY && node->u.unary.op == UNARY_ADDR)
        return eval_address_designator(result, node->u.unary.operand);
    if (info->original.type->kind == SEMA_TYPE_ARRAY &&
        sema_converted_properties(info).type->kind == SEMA_TYPE_POINTER)
        return eval_address_designator(result, node);
    if (node->type == ND_UNARY && node->u.unary.op == UNARY_DEREF &&
        info->original.category == SEMA_FUNCTION_DESIGNATOR)
        return eval_address_constant(result, node->u.unary.operand);
    if (node->type == ND_GENERIC_SELECTION)
        return eval_address_constant(result, info->u.selected);
    if (info->original.type->kind != SEMA_TYPE_POINTER)
        return false;
    if (node->type == ND_CAST) {
        SemaIntegerConstant value;
        return eval_integer_constant(result, node->u.cast.expr, &value) == SEMA_INTEGER_CONSTANT_OK ||
               eval_address_constant(result, node->u.cast.expr);
    }
    if (node->type == ND_BINARY &&
        (node->u.binary.op == BINARY_ADD || node->u.binary.op == BINARY_SUB)) {
        const Node *a = node->u.binary.lhs;
        const Node *b = node->u.binary.rhs;
        SemaIntegerConstant offset;
        const SemaExprInfo *left = sema_lookup_expression(result, a);
        if (sema_converted_properties(left).type->kind == SEMA_TYPE_POINTER)
            return eval_address_constant(result, a) &&
                   eval_integer_constant(result, b, &offset) == SEMA_INTEGER_CONSTANT_OK;
        return eval_integer_constant(result, a, &offset) == SEMA_INTEGER_CONSTANT_OK &&
               eval_address_constant(result, b);
    }
    if (node->type == ND_COND) {
        Constant control;
        if (!check_constant_operands(result, node->u.cond.cond, false) ||
            !eval_arithmetic(result, node->u.cond.cond, &control, NULL))
            return false;
        const Node *selected = constant_truth(control) ? node->u.cond.then_expr
                                                       : node->u.cond.else_expr;
        return eval_is_null_pointer_constant(result, selected) ||
               eval_address_constant(result, selected);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Constant evaluation interfaces
// ---------------------------------------------------------------------------

// Evaluate an integer constant expression independently of its enclosing use.
SemaIntegerConstantStatus eval_integer_constant(const SemaResult *result, const Node *node,
                                                SemaIntegerConstant *out)
{
    if (!check_constant_operands(result, node, true))
        return SEMA_INTEGER_CONSTANT_INVALID_OPERANDS;
    Constant value;
    SemaIntegerConstantStatus failure = SEMA_INTEGER_CONSTANT_EVALUATION_FAILED;
    if (!eval_arithmetic(result, node, &value, &failure))
        return failure;
    *out = (SemaIntegerConstant){ .type = value.type, .bits = value.bits };
    return SEMA_INTEGER_CONSTANT_OK;
}

// Evaluate a checked integer conversion using the target's representation.
uint64_t eval_integer_conversion(SemaIntegerConstant source, const SemaType *target)
{
    Constant value = { .type = source.type, .bits = source.bits };
    Constant converted;
    if (!eval_constant_conversion(value, target, &converted))
        unreachable();
    return converted.bits;
}

// Check range using the source integer's width and signedness.
bool check_integer_constant_range(SemaIntegerConstant constant, int64_t min, uint64_t max)
{
    WideInt value = integer_value((Constant){ .type = constant.type, .bits = constant.bits });
    return value >= min && value <= max;
}

// Recognize integer constant zero and the explicit cast of that form to void *.
bool eval_is_null_pointer_constant(const SemaResult *result, const Node *node)
{
    const SemaType *type = sema_lookup_expression(result, node)->original.type;
    if (node->type == ND_CAST && type->kind == SEMA_TYPE_POINTER &&
        type->u.pointee->kind == SEMA_TYPE_VOID && !type->u.pointee->qualifiers)
        node = node->u.cast.expr;
    SemaIntegerConstant value;
    return eval_integer_constant(result, node, &value) == SEMA_INTEGER_CONSTANT_OK && !value.bits;
}

// Check the source expression for a static initializer without reading a
// runtime object value or constructing backend relocation data.
bool eval_static_initializer(const SemaResult *result, const Node *node)
{
    Constant value;
    return (check_constant_operands(result, node, false) &&
            eval_arithmetic(result, node, &value, NULL)) || eval_address_constant(result, node);
}
