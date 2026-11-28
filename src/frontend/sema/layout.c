// Sizes and alignments for the x86-64 System V LP64 target.

#include "sema_internal.h"

typedef struct {
    unsigned size;
    unsigned alignment;
} ScalarLayout;

static const ScalarLayout g_scalar_layouts[] = {
    [SEMA_TYPE_BOOL] = { 1, 1 },
    [SEMA_TYPE_CHAR] = { 1, 1 },
    [SEMA_TYPE_SCHAR] = { 1, 1 },
    [SEMA_TYPE_UCHAR] = { 1, 1 },
    [SEMA_TYPE_SHORT] = { 2, 2 },
    [SEMA_TYPE_USHORT] = { 2, 2 },
    [SEMA_TYPE_INT] = { 4, 4 },
    [SEMA_TYPE_UINT] = { 4, 4 },
    [SEMA_TYPE_LONG] = { 8, 8 },
    [SEMA_TYPE_ULONG] = { 8, 8 },
    [SEMA_TYPE_LLONG] = { 8, 8 },
    [SEMA_TYPE_ULLONG] = { 8, 8 },
    [SEMA_TYPE_FLOAT] = { 4, 4 },
    [SEMA_TYPE_DOUBLE] = { 8, 8 },
    [SEMA_TYPE_LDOUBLE] = { 16, 16 },
    [SEMA_TYPE_FLOAT_COMPLEX] = { 8, 4 },
    [SEMA_TYPE_DOUBLE_COMPLEX] = { 16, 8 },
    [SEMA_TYPE_LDOUBLE_COMPLEX] = { 32, 16 },
    [SEMA_TYPE_POINTER] = { 8, 8 },
};

// Query the target size before applying the outer atomic representation.
static size_t type_nonatomic_size(const SemaType *type)
{
    switch (type->kind) {
    case SEMA_TYPE_ARRAY:
        return type->u.array.count * type_size(type->u.array.element);
    case SEMA_TYPE_STRUCT:
    case SEMA_TYPE_UNION:
        return type->u.tag->u.aggregate.size;
    case SEMA_TYPE_ENUM: {
        const SemaType *integer = type->u.tag->u.integer_type;
        return integer ? g_scalar_layouts[integer->kind].size : 0;
    }
    case SEMA_TYPE_VOID:
    case SEMA_TYPE_FUNCTION:
        unreachable();
    default:
        return g_scalar_layouts[type->kind].size;
    }
}

// Select the storage unit used for an atomic scalar or small atomic aggregate.
static unsigned type_atomic_unit(size_t size)
{
    if (!size || size > 16)
        return 0;
    unsigned unit = 1;
    while (unit < size)
        unit *= 2;
    return unit;
}

// Query alignment before applying the outer atomic representation.
static unsigned type_nonatomic_alignment(const SemaType *type)
{
    switch (type->kind) {
    case SEMA_TYPE_STRUCT:
    case SEMA_TYPE_UNION:
        return type->u.tag->u.aggregate.alignment;
    case SEMA_TYPE_ENUM: {
        const SemaType *integer = type->u.tag->u.integer_type;
        return integer ? g_scalar_layouts[integer->kind].alignment : 0;
    }
    case SEMA_TYPE_VOID:
    case SEMA_TYPE_FUNCTION:
        unreachable();
    default:
        return g_scalar_layouts[type->kind].alignment;
    }
}

// Compute byte size, including any storage padding for an atomic object.
size_t type_size(const SemaType *type)
{
    size_t size = type_nonatomic_size(type);
    unsigned unit = type->qualifiers & TYPE_QUAL_ATOMIC ? type_atomic_unit(size) : 0;
    return unit ? unit : size;
}

// Query storage alignment, including the element alignment of unbounded arrays.
unsigned type_alignment(const SemaType *type)
{
    while (type->kind == SEMA_TYPE_ARRAY)
        type = type->u.array.element;
    unsigned alignment = type_nonatomic_alignment(type);
    if (type->qualifiers & TYPE_QUAL_ATOMIC) {
        unsigned unit = type_atomic_unit(type_nonatomic_size(type));
        if (unit > alignment)
            return unit;
    }
    return alignment;
}

// Check the supported fundamental and extended alignments for this target.
bool check_type_alignment(uint64_t alignment)
{
    return alignment <= (UINT64_C(1) << 28) && !(alignment & (alignment - 1));
}

// Reject a bound for which no complete target object can be laid out.
bool check_array_size(const SemaType *element, uint64_t count)
{
    size_t stride = type_size(element);
    return stride && count <= INT64_MAX / stride;
}

// Place a checked struct or union member in target storage units. On success
// update the draft offsets and cumulative layout. On overflow, preserve both.
bool type_layout_member(SemaAggregateLayout *layout, SemaMember *member, SemaTypeKind kind)
{
    bool bit_field = member->kind == SEMA_MEMBER_BIT_FIELD;
    unsigned alignment = type_alignment(member->type);
    if (!bit_field && member->u.alignment > alignment)
        alignment = member->u.alignment;
    uint64_t unit = (uint64_t)alignment * 8;
    uint64_t offset = kind == SEMA_TYPE_UNION ? 0 : layout->bits;
    uint64_t width;
    if (bit_field) {
        width = member->u.bit_field.width;
    } else {
        uint64_t bytes = type_size(member->type);
        if (bytes > UINT64_MAX / 8)
            return false;
        width = bytes * 8;
    }

    uint64_t used = offset % unit;
    if (used && (!bit_field || !width || width > unit - used)) {
        uint64_t padding = unit - used;
        if (offset > UINT64_MAX - padding)
            return false;
        offset += padding;
    }
    if (offset > UINT64_MAX - width)
        return false;
    uint64_t end = offset + width;
    if (end > layout->bits)
        layout->bits = end;
    if ((!bit_field || string_view_length(member->name)) &&
        alignment > layout->alignment)
        layout->alignment = alignment;
    member->offset = offset / 8;
    if (bit_field)
        member->u.bit_field.offset = (uint8_t)(offset % 8);
    return true;
}

// Round the footprint to the aggregate alignment, preserving size on overflow.
bool type_layout_aggregate(SemaAggregateLayout layout, uint64_t *size)
{
    uint64_t bytes = layout.bits / 8 + (layout.bits % 8 != 0);
    uint64_t remainder = bytes % layout.alignment;
    uint64_t padding = remainder ? layout.alignment - remainder : 0;
    if (bytes > INT64_MAX - padding)
        return false;
    *size = bytes + padding;
    return true;
}
