// Semantic types are immutable. Builtins are shared and constructed types
// belong to a translation unit's semantic result.

#include "sema_internal.h"

#define INTEGER(kind, rank, width, is_unsigned, unsigned_kind) \
    [SEMA_TYPE_##kind] = { { SEMA_TYPE_##kind, 0, {0} }, \
        { rank, width, is_unsigned, SEMA_TYPE_##unsigned_kind } }
#define OTHER(tag) [SEMA_TYPE_##tag] = { .type.kind = SEMA_TYPE_##tag }

typedef struct {
    SemaType type;
    IntegerTraits integer;
} BuiltinType;

// LP64 with 8-bit signed plain char and the project's scalar size choices.
static const BuiltinType g_builtin_types[] = {
    INTEGER(BOOL, 1, 1, true, BOOL),
    INTEGER(CHAR, 2, 8, false, UCHAR),
    INTEGER(SCHAR, 2, 8, false, UCHAR),
    INTEGER(UCHAR, 2, 8, true, UCHAR),
    INTEGER(SHORT, 3, 16, false, USHORT),
    INTEGER(USHORT, 3, 16, true, USHORT),
    INTEGER(INT, 4, 32, false, UINT),
    INTEGER(UINT, 4, 32, true, UINT),
    INTEGER(LONG, 5, 64, false, ULONG),
    INTEGER(ULONG, 5, 64, true, ULONG),
    INTEGER(LLONG, 6, 64, false, ULLONG),
    INTEGER(ULLONG, 6, 64, true, ULLONG),
    OTHER(FLOAT),
    OTHER(DOUBLE),
    OTHER(LDOUBLE),
    OTHER(FLOAT_COMPLEX),
    OTHER(DOUBLE_COMPLEX),
    OTHER(LDOUBLE_COMPLEX),
    OTHER(VOID),
};

#undef INTEGER
#undef OTHER

// Check a prototype against a declaration without parameter information.
static bool type_is_default_promoted_signature(const SemaType *type)
{
    if (type->u.function.form == SEMA_FUNCTION_VARIADIC)
        return false;
    for (size_t i = 0; i < type->u.function.parameter_count; ++i) {
        const SemaType *param = type->u.function.parameters[i];
        if (!type_is_compatible_unqualified(param, type_default_argument_promotion(param)))
            return false;
    }
    return true;
}

// Compare signatures after parameter adjustment. The recursive comparisons
// descend to return and parameter types in the finite type graph.
static bool type_is_compatible_function(const SemaType *lhs, const SemaType *rhs)
{
    if (!type_is_compatible(lhs->u.function.return_type, rhs->u.function.return_type))
        return false;
    bool left_prototype = type_has_prototype(lhs);
    bool right_prototype = type_has_prototype(rhs);
    if (!left_prototype || !right_prototype) {
        if (!left_prototype && !right_prototype)
            return true;
        const SemaType *prototype = left_prototype ? lhs : rhs;
        const SemaType *other = left_prototype ? rhs : lhs;
        if (other->u.function.form == SEMA_FUNCTION_EMPTY_DEFINITION)
            return !prototype->u.function.parameter_count &&
                   prototype->u.function.form == SEMA_FUNCTION_PROTOTYPE;
        return type_is_default_promoted_signature(prototype);
    }
    if (lhs->u.function.parameter_count != rhs->u.function.parameter_count ||
        lhs->u.function.form != rhs->u.function.form)
        return false;
    for (size_t i = 0; i < lhs->u.function.parameter_count; ++i)
        if (!type_is_compatible_unqualified(lhs->u.function.parameters[i],
                                            rhs->u.function.parameters[i]))
            return false;
    return true;
}

// Combine two compatible function signatures by descending their return and
// parameter types. Retain a prototype or the known empty defining list.
static const SemaType *new_composite_function_type(SemaResult *result,
                                                   const SemaType *lhs,
                                                   const SemaType *rhs)
{
    const SemaType *return_type = new_composite_type(
        result, lhs->u.function.return_type, rhs->u.function.return_type);
    bool left_prototype = type_has_prototype(lhs);
    bool right_prototype = type_has_prototype(rhs);
    const SemaType *signature = left_prototype || (!right_prototype &&
        lhs->u.function.form == SEMA_FUNCTION_EMPTY_DEFINITION) ? lhs : rhs;
    size_t count = signature->u.function.parameter_count;
    const SemaType **params = count ? xcalloc(count, sizeof(*params)) : NULL;
    for (size_t i = 0; i < count; ++i) {
        params[i] = left_prototype && right_prototype
                    ? new_composite_type(result, lhs->u.function.parameters[i],
                                          rhs->u.function.parameters[i])
                    : signature->u.function.parameters[i];
    }
    return new_function_type(result, return_type, params, count, signature->u.function.form);
}

// Detect const subobjects by descending through contained types, never pointers.
static bool type_has_const_subobject(const SemaType *type)
{
    if (type->qualifiers & TYPE_QUAL_CONST)
        return true;
    if (type->kind == SEMA_TYPE_ARRAY)
        return type_has_const_subobject(type->u.array.element);
    if (type->kind == SEMA_TYPE_STRUCT || type->kind == SEMA_TYPE_UNION)
        for (const SemaMember *member = type->u.tag->u.aggregate.members; member; member = member->next)
            if (type_has_const_subobject(member->type))
                return true;
    return false;
}

// Read the canonical unqualified builtin for the target.
const SemaType *type_builtin(SemaTypeKind kind)
{
    return &g_builtin_types[kind].type;
}

// Map a parser builtin specifier independently of its enum numbering.
const SemaType *type_builtin_specifier(TypeKind kind)
{
    static const SemaTypeKind g_specifier_types[] = {
        [TYPE_INT] = SEMA_TYPE_INT, [TYPE_UINT] = SEMA_TYPE_UINT,
        [TYPE_LONG] = SEMA_TYPE_LONG, [TYPE_ULONG] = SEMA_TYPE_ULONG,
        [TYPE_LLONG] = SEMA_TYPE_LLONG, [TYPE_ULLONG] = SEMA_TYPE_ULLONG,
        [TYPE_CHAR] = SEMA_TYPE_CHAR, [TYPE_SCHAR] = SEMA_TYPE_SCHAR,
        [TYPE_UCHAR] = SEMA_TYPE_UCHAR, [TYPE_SHORT] = SEMA_TYPE_SHORT,
        [TYPE_USHORT] = SEMA_TYPE_USHORT, [TYPE_BOOL] = SEMA_TYPE_BOOL,
        [TYPE_FLOAT] = SEMA_TYPE_FLOAT, [TYPE_DOUBLE] = SEMA_TYPE_DOUBLE,
        [TYPE_LDOUBLE] = SEMA_TYPE_LDOUBLE, [TYPE_VOID] = SEMA_TYPE_VOID,
        [TYPE_FLOAT_COMPLEX] = SEMA_TYPE_FLOAT_COMPLEX,
        [TYPE_DOUBLE_COMPLEX] = SEMA_TYPE_DOUBLE_COMPLEX,
        [TYPE_LDOUBLE_COMPLEX] = SEMA_TYPE_LDOUBLE_COMPLEX,
    };
    return type_builtin(g_specifier_types[kind]);
}

// Map the lexer's decoded scalar classification to a semantic builtin.
const SemaType *type_literal(LiteralType literal)
{
    static const SemaTypeKind g_literal_types[] = {
        [LITERAL_INT] = SEMA_TYPE_INT, [LITERAL_UINT] = SEMA_TYPE_UINT,
        [LITERAL_LONG] = SEMA_TYPE_LONG, [LITERAL_ULONG] = SEMA_TYPE_ULONG,
        [LITERAL_LLONG] = SEMA_TYPE_LLONG, [LITERAL_ULLONG] = SEMA_TYPE_ULLONG,
        [LITERAL_FLOAT] = SEMA_TYPE_FLOAT, [LITERAL_DOUBLE] = SEMA_TYPE_DOUBLE,
        [LITERAL_LDOUBLE] = SEMA_TYPE_LDOUBLE,
    };
    return type_builtin(g_literal_types[literal]);
}

// Return the target's size_t type for size queries.
const SemaType *type_size_t(void)
{
    return type_builtin(SEMA_TYPE_ULONG);
}

// Construct a pointer with qualifiers specific to its own level.
const SemaType *new_pointer_type(SemaResult *result, const SemaType *pointee,
                                 unsigned qualifiers)
{
    return attach_type(result, (SemaType){
        .kind = SEMA_TYPE_POINTER, .qualifiers = qualifiers, .u.pointee = pointee,
    });
}

// Construct a checked array, including a missing bound for an incomplete array.
const SemaType *new_array_type(SemaResult *result, const SemaType *element, uint64_t count)
{
    return attach_type(result, (SemaType){
        .kind = SEMA_TYPE_ARRAY, .u.array = { .element = element, .count = count },
    });
}

// Refer to a nominal type without replacing its identity when completed.
const SemaType *new_tag_type(SemaResult *result, const SemaTag *tag)
{
    return attach_type(result, (SemaType){ .kind = tag->kind, .u.tag = tag });
}

// Own a function signature and its final parameter vector.
const SemaType *new_function_type(SemaResult *result, const SemaType *return_type,
                                  const SemaType **parameters, size_t count,
                                  SemaFunctionForm form)
{
    return attach_type(result, (SemaType){
        .kind = SEMA_TYPE_FUNCTION, .u.function = {
            .return_type = return_type, .parameters = parameters,
            .parameter_count = count, .form = form,
        },
    });
}

// Requalify just the outer object type, retaining its existing derived type.
const SemaType *new_qualified_type(SemaResult *result, const SemaType *type,
                                   unsigned qualifiers)
{
    if (type->kind == SEMA_TYPE_ARRAY) {
        if (!qualifiers)
            return type;
        const SemaType *element = type->u.array.element;
        return new_array_type(result,
            new_qualified_type(result, element, element->qualifiers | qualifiers),
            type->u.array.count);
    }
    if (type->qualifiers == qualifiers)
        return type;
    if (type->kind == SEMA_TYPE_FUNCTION)
        unreachable();
    if (type->kind == SEMA_TYPE_POINTER)
        return new_pointer_type(result, type->u.pointee, qualifiers);
    if (!qualifiers && type->kind < SEMA_TYPE_POINTER)
        return type_builtin(type->kind);
    SemaType qualified = *type;
    qualified.qualifiers = qualifiers;
    return attach_type(result, qualified);
}

// Classify integer types by their target traits.
bool type_is_integer(const SemaType *type)
{
    return type->kind == SEMA_TYPE_ENUM ||
           (type->kind < SEMA_TYPE_POINTER && g_builtin_types[type->kind].integer.rank);
}

// Test whether the type belongs to either arithmetic domain.
bool type_is_arithmetic(const SemaType *type)
{
    return type->kind == SEMA_TYPE_ENUM ||
           (type->kind < SEMA_TYPE_POINTER && type->kind != SEMA_TYPE_VOID);
}

// Test whether the type is an integer or a real floating type.
bool type_is_real(const SemaType *type)
{
    return type_is_integer(type) || type->kind == SEMA_TYPE_FLOAT ||
           type->kind == SEMA_TYPE_DOUBLE || type->kind == SEMA_TYPE_LDOUBLE;
}

// Classify arithmetic types and pointers as scalar.
bool type_is_scalar(const SemaType *type)
{
    return type_is_arithmetic(type) || type->kind == SEMA_TYPE_POINTER;
}

// Classify object types, including the incomplete void type.
bool type_is_object(const SemaType *type)
{
    return type->kind != SEMA_TYPE_FUNCTION;
}

// Identify function types whose parameter list constrains calls through this type.
bool type_has_prototype(const SemaType *type)
{
    return type->kind == SEMA_TYPE_FUNCTION &&
           (type->u.function.form == SEMA_FUNCTION_PROTOTYPE ||
            type->u.function.form == SEMA_FUNCTION_VARIADIC);
}

// Query completeness at the current source point, including completed tags.
bool type_is_complete_object(const SemaType *type)
{
    switch (type->kind) {
    case SEMA_TYPE_VOID:
    case SEMA_TYPE_FUNCTION: return false;
    case SEMA_TYPE_ARRAY: return type->u.array.count != 0;
    case SEMA_TYPE_STRUCT:
    case SEMA_TYPE_UNION:
    case SEMA_TYPE_ENUM: return type->u.tag->is_complete;
    default: return true;
    }
}

// Identify aggregates subject to the flexible-array containment restriction.
bool type_has_flexible_array(const SemaType *type)
{
    return (type->kind == SEMA_TYPE_STRUCT || type->kind == SEMA_TYPE_UNION) &&
           (type->u.tag->u.aggregate.flags & SEMA_AGGREGATE_CONTAINS_FLEXIBLE_ARRAY);
}

// Read the rank, range description and unsigned counterpart of an integer type.
IntegerTraits type_integer_traits(const SemaType *type)
{
    if (type->kind == SEMA_TYPE_ENUM)
        type = type->u.tag->u.integer_type;
    return g_builtin_types[type->kind].integer;
}

// Apply C11 integer promotions to a type whose value has already been read.
const SemaType *type_integer_promotion(const SemaType *type)
{
    if (!type_is_integer(type))
        return type;
    IntegerTraits source = type_integer_traits(type);
    IntegerTraits integer = type_integer_traits(type_builtin(SEMA_TYPE_INT));
    if (type->kind == SEMA_TYPE_ENUM)
        return type->u.tag->u.integer_type;
    if (source.rank >= integer.rank)
        return type;
    return type_builtin(source.is_unsigned && source.width >= integer.width
                        ? SEMA_TYPE_UINT : SEMA_TYPE_INT);
}

// Apply default argument promotions to an unqualified value type.
const SemaType *type_default_argument_promotion(const SemaType *type)
{
    return type->kind == SEMA_TYPE_FLOAT ? type_builtin(SEMA_TYPE_DOUBLE)
                                        : type_integer_promotion(type);
}

// Compare types at all deeper levels. Pointee descent terminates at a builtin
// or a compatible-function check on strictly smaller signatures.
bool type_is_compatible_unqualified(const SemaType *lhs, const SemaType *rhs)
{
    if (lhs == rhs)
        return true;
    if ((lhs->qualifiers ^ rhs->qualifiers) & TYPE_QUAL_ATOMIC)
        return false;
    if (lhs->kind == SEMA_TYPE_ENUM && rhs->kind != SEMA_TYPE_ENUM)
        return lhs->u.tag->u.integer_type && lhs->u.tag->u.integer_type->kind == rhs->kind;
    if (rhs->kind == SEMA_TYPE_ENUM && lhs->kind != SEMA_TYPE_ENUM)
        return rhs->u.tag->u.integer_type && rhs->u.tag->u.integer_type->kind == lhs->kind;
    if (lhs->kind != rhs->kind)
        return false;
    switch (lhs->kind) {
    case SEMA_TYPE_POINTER:
        return type_is_compatible(lhs->u.pointee, rhs->u.pointee);
    case SEMA_TYPE_FUNCTION:
        return type_is_compatible_function(lhs, rhs);
    case SEMA_TYPE_ARRAY:
        return (!lhs->u.array.count || !rhs->u.array.count ||
                lhs->u.array.count == rhs->u.array.count) &&
               type_is_compatible(lhs->u.array.element, rhs->u.array.element);
    case SEMA_TYPE_STRUCT:
    case SEMA_TYPE_UNION:
    case SEMA_TYPE_ENUM:
        return lhs->u.tag == rhs->u.tag;
    default:
        return true;
    }
}

// Check compatibility including this level's qualifiers.
bool type_is_compatible(const SemaType *lhs, const SemaType *rhs)
{
    return lhs->qualifiers == rhs->qualifiers &&
           type_is_compatible_unqualified(lhs, rhs);
}

// Compare exact types, descending through pointers and signature components.
bool type_is_identical(const SemaType *lhs, const SemaType *rhs)
{
    if (lhs == rhs)
        return true;
    if (lhs->kind != rhs->kind || lhs->qualifiers != rhs->qualifiers)
        return false;
    if (lhs->kind == SEMA_TYPE_POINTER)
        return type_is_identical(lhs->u.pointee, rhs->u.pointee);
    if (lhs->kind == SEMA_TYPE_ARRAY)
        return lhs->u.array.count == rhs->u.array.count &&
               type_is_identical(lhs->u.array.element, rhs->u.array.element);
    if (lhs->kind == SEMA_TYPE_STRUCT || lhs->kind == SEMA_TYPE_UNION ||
        lhs->kind == SEMA_TYPE_ENUM)
        return lhs->u.tag == rhs->u.tag;
    if (lhs->kind != SEMA_TYPE_FUNCTION)
        return true;
    if (lhs->u.function.form != rhs->u.function.form ||
        lhs->u.function.parameter_count != rhs->u.function.parameter_count ||
        !type_is_identical(lhs->u.function.return_type, rhs->u.function.return_type))
        return false;
    for (size_t i = 0; i < lhs->u.function.parameter_count; ++i)
        if (!type_is_identical(lhs->u.function.parameters[i], rhs->u.function.parameters[i]))
            return false;
    return true;
}

// Construct the composite of already compatible types. Recursion descends
// only through pointees and signature components until reaching builtins.
const SemaType *new_composite_type(SemaResult *result, const SemaType *lhs,
                                   const SemaType *rhs)
{
    if (lhs == rhs)
        return lhs;
    if (rhs->kind == SEMA_TYPE_ENUM)
        return rhs;
    switch (lhs->kind) {
    case SEMA_TYPE_POINTER:
        return new_pointer_type(result, new_composite_type(result, lhs->u.pointee,
                                                             rhs->u.pointee),
                                lhs->qualifiers);
    case SEMA_TYPE_FUNCTION:
        return new_composite_function_type(result, lhs, rhs);
    case SEMA_TYPE_ARRAY:
        return new_array_type(result,
            new_composite_type(result, lhs->u.array.element, rhs->u.array.element),
            lhs->u.array.count ? lhs->u.array.count : rhs->u.array.count);
    default:
        return lhs;
    }
}

// Check the type constraints on a modifiable lvalue, including contained const members.
bool type_is_modifiable_object(const SemaType *type)
{
    return type_is_complete_object(type) && type->kind != SEMA_TYPE_ARRAY &&
           !type_has_const_subobject(type);
}
