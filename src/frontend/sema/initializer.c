// Check initializer placement and record sparse, typed subobject writes.

#include "sema_internal.h"

#include "util/diagnostic.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    INITIALIZER_EXCESS,
    INITIALIZER_BAD_DESIGNATOR,
    INITIALIZER_BAD_INDEX,
    INITIALIZER_INDEX_BOUNDS,
    INITIALIZER_BAD_OBJECT,
    INITIALIZER_STRING_TOO_LONG,
    INITIALIZER_NONCONSTANT,
    INITIALIZER_ARRAY_SIZE,
} InitializerIssue;

typedef struct {
    const SemaType *type;
    const SemaMember *member;
    uint64_t offset;
    uint64_t index;
    size_t depth;
} InitializerObject;

typedef struct {
    InitializerObject *objects;
    size_t count;
    size_t capacity;
} InitializerPath;

typedef struct {
    InitializerObject object;
    const SemaMember *active;
} InitializerUnion;

typedef struct {
    SemaContext *ctx;
    const Node *owner;
    SemaInitializerStep *steps;
    size_t count;
    size_t capacity;
    InitializerUnion *unions;
    size_t union_count;
    size_t union_capacity;
    bool constant_required;
} InitializerBuilder;

static SemaStatus analyze_initializer_object(InitializerBuilder *builder, InitializerObject object,
                                              const Node *initializer, const SemaType **out);

// ---------------------------------------------------------------------------
// Initializer buffers and object placement
// ---------------------------------------------------------------------------

// Replace the owned buffer and update its capacity while preserving live elements.
static void *grow_initializer_buffer(void *old, size_t count, size_t *capacity, size_t stride)
{
    size_t next = *capacity ? *capacity * 2 : 8;
    if (next <= *capacity || next > SIZE_MAX / stride)
        fatal("initializer is too large");
    void *buffer = xcalloc(next, stride);
    if (count)
        memcpy(buffer, old, count * stride);
    free(old);
    *capacity = next;
    return buffer;
}

// Append a checked write to the builder in initializer override order.
static void attach_initializer_step(InitializerBuilder *builder, SemaInitializerStep step)
{
    if (builder->count == builder->capacity)
        builder->steps = grow_initializer_buffer(builder->steps, builder->count,
                                                 &builder->capacity, sizeof(*builder->steps));
    builder->steps[builder->count++] = step;
}

// Push a checked child subobject in the current brace's placement path.
static void push_initializer_object(InitializerPath *path, InitializerObject object)
{
    if (path->count == path->capacity)
        path->objects = grow_initializer_buffer(path->objects, path->count,
                                                &path->capacity, sizeof(*path->objects));
    path->objects[path->count++] = object;
}

// Check whether this type has positional initializer children.
static bool check_initializer_aggregate(const SemaType *type)
{
    return type->kind == SEMA_TYPE_ARRAY || type->kind == SEMA_TYPE_STRUCT ||
           type->kind == SEMA_TYPE_UNION;
}

// Check whether a string can initialize this entire character array.
static bool check_initializer_string(const SemaType *type, const Node *expression)
{
    if (expression->type != ND_STRING || type->kind != SEMA_TYPE_ARRAY)
        return false;
    SemaTypeKind kind = type->u.array.element->kind;
    return kind == SEMA_TYPE_CHAR || kind == SEMA_TYPE_SCHAR || kind == SEMA_TYPE_UCHAR;
}

// Find the next positional member, excluding unnamed bit-fields and flexible arrays.
static const SemaMember *resolve_initializer_positional_member(const SemaMember *member)
{
    while (member && ((member->kind != SEMA_MEMBER_ANONYMOUS && !string_view_length(member->name)) ||
                      !type_is_complete_object(member->type)))
        member = member->next;
    return member;
}

// Resolve an array element without requiring a known outermost bound.
static bool resolve_initializer_element(InitializerObject parent, uint64_t index,
                                        InitializerObject *out)
{
    const SemaType *type = parent.type;
    if ((type->u.array.count && index >= type->u.array.count) || index == UINT64_MAX ||
        !check_array_size(type->u.array.element, index + 1))
        return false;
    *out = (InitializerObject){
        .type = type->u.array.element, .index = index, .depth = parent.depth + 1,
        .offset = parent.offset + index * type_size(type->u.array.element),
    };
    return true;
}

// Resolve the target facts for a member of an already selected aggregate.
static InitializerObject resolve_initializer_member(InitializerObject parent,
                                                     const SemaMember *member)
{
    return (InitializerObject){
        .type = member->type, .member = member, .depth = parent.depth + 1,
        .offset = parent.offset + member->offset,
    };
}

// Find the first eligible child of an aggregate for positional initialization.
static bool resolve_initializer_first(InitializerObject parent, InitializerObject *out)
{
    if (parent.type->kind == SEMA_TYPE_ARRAY)
        return resolve_initializer_element(parent, 0, out);
    const SemaMember *member = resolve_initializer_positional_member(parent.type->u.tag->u.aggregate.members);
    if (!member)
        return false;
    *out = resolve_initializer_member(parent, member);
    return true;
}

// Find the next subobject and return how much of the enclosing path to retain.
static size_t resolve_initializer_next(const InitializerPath *path, InitializerObject *next)
{
    for (size_t count = path->count; count > 1; --count) {
        InitializerObject child = path->objects[count - 1];
        InitializerObject parent = path->objects[count - 2];
        if (parent.type->kind == SEMA_TYPE_ARRAY) {
            if (!resolve_initializer_element(parent, child.index + 1, next))
                continue;
        } else if (parent.type->kind == SEMA_TYPE_STRUCT) {
            const SemaMember *member = resolve_initializer_positional_member(child.member->next);
            if (!member)
                continue;
            *next = resolve_initializer_member(parent, member);
        } else {
            continue;
        }
        return count - 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Diagnostics, union selection and designators
// ---------------------------------------------------------------------------

// Anchor an initializer constraint at the responsible source expression or designator.
static SemaStatus report_initializer_issue(const SourceSpan *span, InitializerIssue issue)
{
    static const char *const g_messages[] = {
        [INITIALIZER_EXCESS] = "excess elements in initializer",
        [INITIALIZER_BAD_DESIGNATOR] = "designator does not name a valid subobject",
        [INITIALIZER_BAD_INDEX] = "array designator index must be a nonnegative integer constant expression",
        [INITIALIZER_INDEX_BOUNDS] = "array designator index exceeds the array or target size limit",
        [INITIALIZER_BAD_OBJECT] = "invalid initializer for this type",
        [INITIALIZER_STRING_TOO_LONG] = "initializer string is too long for the array",
        [INITIALIZER_NONCONSTANT] = "initializer is not a constant expression",
        [INITIALIZER_ARRAY_SIZE] = "cannot determine a valid array size from the initializer",
    };
    error_at(span, "%s", g_messages[issue]);
    return SEMA_INVALID;
}

// Read a union's current member within this object's initializer.
static size_t lookup_initializer_union(const InitializerBuilder *builder, InitializerObject object)
{
    for (size_t i = 0; i < builder->union_count; ++i) {
        InitializerObject prior = builder->unions[i].object;
        if (prior.offset == object.offset && prior.depth == object.depth &&
            prior.type->u.tag == object.type->u.tag)
            return i;
    }
    return builder->union_count;
}

// Discard selection facts covered by a zero or whole-object write.
static void update_initializer_unions(InitializerBuilder *builder, InitializerObject object)
{
    uint64_t size = type_size(object.type);
    size_t output = 0;
    for (size_t i = 0; i < builder->union_count; ++i) {
        InitializerObject prior = builder->unions[i].object;
        bool covered = prior.depth >= object.depth && prior.offset >= object.offset &&
                       (!size || prior.offset - object.offset < size);
        if (!covered)
            builder->unions[output++] = builder->unions[i];
    }
    builder->union_count = output;
}

// Commit the checked active member after a union selection change.
static void attach_initializer_union(InitializerBuilder *builder, InitializerObject object,
                                    const SemaMember *member)
{
    if (builder->union_count == builder->union_capacity)
        builder->unions = grow_initializer_buffer(builder->unions, builder->union_count,
            &builder->union_capacity, sizeof(*builder->unions));
    builder->unions[builder->union_count++] = (InitializerUnion){ object, member };
}

// Record a zero baseline at the checked subobject offset.
static void attach_initializer_zero(InitializerBuilder *builder, InitializerObject object)
{
    attach_initializer_step(builder, (SemaInitializerStep){
        .kind = SEMA_INITIALIZER_ZERO, .type = object.type, .offset = object.offset,
    });
}

// Reset and bind each union whose active member changes along this path.
static void analyze_initializer_unions(InitializerBuilder *builder, const InitializerPath *path)
{
    for (size_t i = 1; i < path->count; ++i) {
        InitializerObject parent = path->objects[i - 1];
        if (parent.type->kind != SEMA_TYPE_UNION)
            continue;
        const SemaMember *member = path->objects[i].member;
        size_t index = lookup_initializer_union(builder, parent);
        if (index < builder->union_count && builder->unions[index].active == member)
            continue;
        update_initializer_unions(builder, parent);
        attach_initializer_zero(builder, parent);
        attach_initializer_union(builder, parent, member);
    }
}

// Select a complete designated member, retaining its anonymous enclosing objects.
static bool analyze_initializer_member_path(InitializerPath *path, StringView name)
{
    InitializerObject parent = path->objects[path->count - 1];
    SemaMemberPath member_path = new_member_path(&parent.type->u.tag->u.aggregate, name);
    if (!member_path.count)
        return false;
    const SemaMember *target = member_path.members[member_path.count - 1];
    bool valid = type_is_complete_object(target->type);
    if (valid) {
        for (size_t i = 0; i < member_path.count; ++i) {
            parent = resolve_initializer_member(parent, member_path.members[i]);
            push_initializer_object(path, parent);
        }
    }
    free_member_path(&member_path);
    return valid;
}

// Check the index independently, then check bounds and select an element when a target is known.
static SemaStatus analyze_initializer_index(SemaContext *ctx, InitializerPath *path,
                                             const Node *designator)
{
    const Node *expression = designator->u.index_designator.index;
    SemaStatus status = analyze_expression(ctx, expression);
    if (status != SEMA_OK)
        return status;
    SemaIntegerConstant value;
    if (eval_integer_constant(ctx->result, expression, &value) != SEMA_INTEGER_CONSTANT_OK ||
        !check_integer_constant_range(value, 0, UINT64_MAX))
        return report_initializer_issue(&designator->span, INITIALIZER_BAD_INDEX);
    if (!path)
        return SEMA_OK;
    InitializerObject object;
    if (!resolve_initializer_element(path->objects[path->count - 1], value.bits, &object))
        return report_initializer_issue(&designator->span, INITIALIZER_INDEX_BOUNDS);
    push_initializer_object(path, object);
    return SEMA_OK;
}

// Check one designator, including its independent index expression if placement is unavailable.
static SemaStatus analyze_initializer_designator(SemaContext *ctx, InitializerPath *path,
                                                  const Node *designator)
{
    if (designator->type == ND_INDEX_DESIGNATOR) {
        if (path && path->objects[path->count - 1].type->kind == SEMA_TYPE_ARRAY)
            return analyze_initializer_index(ctx, path, designator);
        SemaStatus status = path ? report_initializer_issue(&designator->span, INITIALIZER_BAD_DESIGNATOR)
                                 : SEMA_OK;
        return merge_sema_status(status, analyze_initializer_index(ctx, NULL, designator));
    }
    if (path) {
        SemaTypeKind kind = path->objects[path->count - 1].type->kind;
        if ((kind != SEMA_TYPE_STRUCT && kind != SEMA_TYPE_UNION) ||
            !analyze_initializer_member_path(path, designator->u.member.name))
            return report_initializer_issue(&designator->span, INITIALIZER_BAD_DESIGNATOR);
    }
    return SEMA_OK;
}

// Start at this brace's root when typed, then visit remaining indices after a failed selection.
static SemaStatus analyze_initializer_designation(SemaContext *ctx, InitializerPath *path,
                                                   const Node *designator)
{
    if (path)
        path->count = 1;
    SemaStatus status = SEMA_OK;
    for (; designator && !ctx->scope->visibility_unknown; designator = designator->next) {
        SemaStatus current = analyze_initializer_designator(ctx, status == SEMA_OK ? path : NULL,
                                                            designator);
        status = merge_sema_status(status, current);
    }
    return status;
}

// Visit the expressions of the remaining entries without assuming their positions or types.
static SemaStatus analyze_untyped_initializer_list(SemaContext *ctx, const Node *item)
{
    SemaStatus status = SEMA_OK;
    for (; item && !ctx->scope->visibility_unknown; item = item->next)
        status = merge_sema_status(status, analyze_untyped_initializer(ctx, item));
    return status;
}

// ---------------------------------------------------------------------------
// Typed initializer analysis
// ---------------------------------------------------------------------------

// Check placement and record the bytes of an already analyzed string literal.
static SemaStatus analyze_typed_initializer_string(InitializerBuilder *builder,
                                                  InitializerObject object,
                                                  const Node *expression, const SemaType **out)
{
    uint64_t length = (uint64_t)expression->u.string.length;
    uint64_t count = object.type->u.array.count;
    if (count && count < length)
        return report_initializer_issue(&expression->span, INITIALIZER_STRING_TOO_LONG);
    if (!count) {
        count = length + 1;
        object.type = new_array_type(builder->ctx->result, object.type->u.array.element, count);
    }
    update_initializer_unions(builder, object);
    attach_initializer_zero(builder, object);
    attach_initializer_step(builder, (SemaInitializerStep){
        .kind = SEMA_INITIALIZER_STRING, .type = object.type, .offset = object.offset,
        .u.string = { .literal = expression, .bytes = count == length ? length : length + 1 },
    });
    *out = object.type;
    return SEMA_OK;
}

// Analyze a string initializer without applying array decay to its operand.
static SemaStatus analyze_initializer_string(InitializerBuilder *builder, InitializerObject object,
                                              const Node *expression, const SemaType **out)
{
    SemaStatus status = analyze_expression(builder->ctx, expression);
    return status == SEMA_OK
        ? analyze_typed_initializer_string(builder, object, expression, out) : status;
}

// Check conversion and constant constraints for an already analyzed operand.
static SemaStatus analyze_initializer_expression(InitializerBuilder *builder, InitializerObject object,
                                                  const Node *expression)
{
    if (object.type->kind == SEMA_TYPE_ARRAY)
        return report_initializer_issue(&expression->span, INITIALIZER_BAD_OBJECT);
    SemaStatus status = analyze_typed_assignment_value(
        builder->ctx, expression, builder->owner, object.type);
    if (status != SEMA_OK)
        return status;
    if (builder->constant_required && !eval_static_initializer(builder->ctx->result, expression))
        return report_initializer_issue(&expression->span, INITIALIZER_NONCONSTANT);
    if (check_initializer_aggregate(object.type))
        update_initializer_unions(builder, object);
    attach_initializer_step(builder, (SemaInitializerStep){
        .kind = SEMA_INITIALIZER_EXPRESSION, .type = object.type, .offset = object.offset,
        .u.expression = {
            .operand = expression,
            .bit_field = object.member && object.member->kind == SEMA_MEMBER_BIT_FIELD
                ? object.member : NULL,
        },
    });
    return SEMA_OK;
}

// Determine whether an expression supplies the current object as a whole.
static bool check_initializer_whole(InitializerObject object, const Node *expression,
                                    const SemaType *source)
{
    if (!check_initializer_aggregate(object.type) || check_initializer_string(object.type, expression))
        return true;
    return source && object.type->kind != SEMA_TYPE_ARRAY &&
           type_is_compatible_unqualified(object.type, source);
}

// Analyze one item and retain its position when brace elision can still be determined.
static SemaStatus analyze_initializer_item(InitializerBuilder *builder, InitializerPath *path,
                                            const Node *item, bool *position_known)
{
    InitializerObject object = path->objects[path->count - 1];
    const SemaType *completed;
    if (item->type == ND_INIT_LIST || check_initializer_string(object.type, item)) {
        analyze_initializer_unions(builder, path);
        return analyze_initializer_object(builder, object, item, &completed);
    }

    SemaStatus status = analyze_expression(builder->ctx, item);
    if (builder->ctx->scope->visibility_unknown)
        return status;
    const SemaType *source = status == SEMA_OK
        ? sema_lookup_expression(builder->ctx->result, item)->original.type : NULL;
    while (!check_initializer_whole(object, item, source)) {
        if ((!source && object.type->kind != SEMA_TYPE_ARRAY) ||
            !resolve_initializer_first(object, &object)) {
            *position_known = false;
            return status == SEMA_OK ? report_initializer_issue(&item->span, INITIALIZER_EXCESS) : status;
        }
        push_initializer_object(path, object);
    }
    if (status != SEMA_OK)
        return status;
    analyze_initializer_unions(builder, path);
    if (check_initializer_string(object.type, item))
        return analyze_typed_initializer_string(builder, object, item, &completed);
    return analyze_initializer_expression(builder, object, item);
}

// Walk one aggregate brace scope, preserving designator position and largest index.
static SemaStatus analyze_initializer_aggregate(InitializerBuilder *builder, InitializerObject root,
                                                 const Node *list, const SemaType **out)
{
    InitializerPath path = {0};
    push_initializer_object(&path, root);
    InitializerObject first;
    if (resolve_initializer_first(root, &first))
        push_initializer_object(&path, first);
    size_t zero = builder->count;
    update_initializer_unions(builder, root);
    attach_initializer_zero(builder, root);

    SemaStatus status = SEMA_OK;
    uint64_t count = 0;
    bool position_known = true;
    SemaContext *ctx = builder->ctx;
    for (const Node *entry = list->u.initializer_list.initializers;
         entry && !ctx->scope->visibility_unknown; entry = entry->next) {
        const Node *item = entry;
        if (entry->type == ND_DESIGNATED_INIT) {
            SemaStatus designation = analyze_initializer_designation(ctx, &path, entry->u.designated.designators);
            status = merge_sema_status(status, designation);
            if (ctx->scope->visibility_unknown)
                break;
            position_known = designation == SEMA_OK;
            item = entry->u.designated.value;
        }
        if (position_known && path.count == 1) {
            status = merge_sema_status(status, report_initializer_issue(&item->span, INITIALIZER_EXCESS));
            position_known = false;
        }
        if (!position_known) {
            status = merge_sema_status(status, analyze_untyped_initializer(ctx, item));
            continue;
        }
        status = merge_sema_status(status, analyze_initializer_item(builder, &path, item, &position_known));
        if (!position_known || ctx->scope->visibility_unknown)
            continue;
        if (root.type->kind == SEMA_TYPE_ARRAY && count <= path.objects[1].index)
            count = path.objects[1].index + 1;
        InitializerObject next;
        size_t enclosing = resolve_initializer_next(&path, &next);
        path.count = enclosing ? enclosing : 1;
        if (enclosing)
            push_initializer_object(&path, next);
    }
    free(path.objects);

    if (status != SEMA_OK)
        return status;
    if (root.type->kind == SEMA_TYPE_ARRAY && !root.type->u.array.count) {
        if (!count)
            return report_initializer_issue(&list->span, INITIALIZER_ARRAY_SIZE);
        root.type = new_array_type(builder->ctx->result, root.type->u.array.element, count);
        builder->steps[zero].type = root.type;
    }
    *out = root.type;
    return SEMA_OK;
}

// Check the first scalar or string initializer, then visit the excess entries.
// Recursive analysis is confined to children of the enclosing brace list.
static SemaStatus analyze_initializer_excess(InitializerBuilder *builder, InitializerObject object,
                                              const Node *first)
{
    const SemaType *completed;
    SemaStatus status = analyze_initializer_object(builder, object, first, &completed);
    SemaContext *ctx = builder->ctx;
    if (ctx->scope->visibility_unknown)
        return status;
    status = merge_sema_status(status, report_initializer_issue(&first->next->span, INITIALIZER_EXCESS));
    return merge_sema_status(status, analyze_untyped_initializer_list(ctx, first->next));
}

// Analyze a whole object. Recursion only consumes nested initializer AST children.
static SemaStatus analyze_initializer_object(InitializerBuilder *builder, InitializerObject object,
                                              const Node *initializer, const SemaType **out)
{
    if (check_initializer_string(object.type, initializer))
        return analyze_initializer_string(builder, object, initializer, out);
    while (initializer->type == ND_INIT_LIST) {
        const Node *item = initializer->u.initializer_list.initializers;
        if (item && check_initializer_string(object.type, item)) {
            if (item->next)
                return analyze_initializer_excess(builder, object, item);
            return analyze_initializer_string(builder, object, item, out);
        }
        if (check_initializer_aggregate(object.type))
            return analyze_initializer_aggregate(builder, object, initializer, out);
        if (!item || item->type == ND_DESIGNATED_INIT) {
            SemaStatus status = report_initializer_issue(&initializer->span, INITIALIZER_BAD_OBJECT);
            return merge_sema_status(status, analyze_untyped_initializer_list(builder->ctx, item));
        }
        if (item->next)
            return analyze_initializer_excess(builder, object, item);
        initializer = item;
    }
    SemaStatus status = analyze_expression(builder->ctx, initializer);
    if (status != SEMA_OK)
        return status;
    *out = object.type;
    return analyze_initializer_expression(builder, object, initializer);
}

// ---------------------------------------------------------------------------
// Initializer entry points
// ---------------------------------------------------------------------------

// Return the completed type and owned steps for the caller to publish or release.
SemaStatus analyze_initializer(SemaContext *ctx, const Node *owner, const Node *initializer,
                                const SemaType *type, bool constant_required, SemaInitializer *out)
{
    InitializerBuilder builder = { .ctx = ctx, .owner = owner, .constant_required = constant_required };
    const SemaType *completed;
    SemaStatus status = analyze_initializer_object(&builder, (InitializerObject){ .type = type },
                                                    initializer, &completed);
    if (status == SEMA_OK) {
        *out = (SemaInitializer){ .type = completed, .steps = builder.steps, .count = builder.count };
        builder.steps = NULL;
    }
    free(builder.steps);
    free(builder.unions);
    return status;
}

// Check source expressions without a destination. Recursion visits only list entries and designated values.
SemaStatus analyze_untyped_initializer(SemaContext *ctx, const Node *initializer)
{
    switch (initializer->type) {
    case ND_INIT_LIST:
        return analyze_untyped_initializer_list(ctx, initializer->u.initializer_list.initializers);
    case ND_DESIGNATED_INIT: {
        SemaStatus status = analyze_initializer_designation(ctx, NULL, initializer->u.designated.designators);
        if (!ctx->scope->visibility_unknown)
            status = merge_sema_status(status, analyze_untyped_initializer(ctx, initializer->u.designated.value));
        return status;
    }
    default:
        return analyze_expression(ctx, initializer);
    }
}
