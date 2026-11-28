// Own and index semantic facts at stable addresses for the lifetime of a result.

#include "sema_internal.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    ENTRY_SYMBOL,
    ENTRY_EXPRESSION,
    ENTRY_JUMP_TARGET,
    ENTRY_SWITCH_LABEL,
    ENTRY_CONDITION,
} EntryKind;

typedef struct ResultEntry {
    EntryKind kind;
    struct ResultEntry *previous_symbol;
    union {
        SemaSymbol symbol;
        SemaExprInfo expression;
        const Node *jump_target;
        SemaSwitchLabelInfo switch_label;
        SemaConditionInfo condition;
    } u;
} ResultEntry;

typedef struct {
    SemaUpdateInfo info;
    SemaConversion steps[];
} OwnedUpdate;

typedef struct OwnedType {
    SemaType type;
    struct OwnedType *next;
} OwnedType;

typedef struct OwnedEntity {
    SemaEntity entity; // First member so entity-based writes can recover the owner.
    SemaEntityAnalysis analysis;
    struct OwnedEntity *next;
} OwnedEntity;

typedef struct OwnedTag {
    SemaTag tag;
    struct OwnedTag *next;
} OwnedTag;

typedef struct OwnedMember {
    SemaMember member;
    struct OwnedMember *next;
} OwnedMember;

struct SemaResult {
    HashMap by_node;
    HashMap initializers;
    OwnedType *types;
    OwnedEntity *entities;
    OwnedTag *tags;
    OwnedMember *members;
};

// Transfer a completed record and give it a stable address across table growth.
static ResultEntry *attach_entry(SemaResult *result, const Node *node,
                                 ResultEntry value)
{
    if (!node || hashmap_get_pointer(&result->by_node, node))
        unreachable();
    ResultEntry *entry = xmalloc(sizeof(*entry));
    *entry = value;
    hashmap_put_pointer(&result->by_node, node, entry);
    return entry;
}

// Release a result record and the variant-specific storage it owns.
static void free_entry(void *value)
{
    ResultEntry *entry = value;
    if (entry->kind == ENTRY_EXPRESSION) {
        const SemaExprInfo *expression = &entry->u.expression;
        free((void *)expression->conversions.steps);
        if (expression->kind == SEMA_EXPR_UPDATE)
            free((OwnedUpdate *)expression->u.update);
    }
    while (entry) {
        ResultEntry *previous = entry->previous_symbol;
        free(entry);
        entry = previous;
    }
}

// Release the owned steps followed by their initializer record.
static void free_initializer(void *value)
{
    SemaInitializer *initializer = value;
    free((void *)initializer->steps);
    free(initializer);
}

// Store a complete variant with its intrinsic properties and empty conversion chain.
static void attach_expression_info(SemaResult *result, const Node *node, SemaExprInfo info)
{
    attach_entry(result, node, (ResultEntry){
        .kind = ENTRY_EXPRESSION, .u.expression = info,
    });
}

// Copy one chain into the update's storage and advance the destination.
static void attach_update_sequence(SemaConversionSequence *sequence,
                                   SemaConversion **destination)
{
    if (!sequence->count) {
        sequence->steps = NULL;
        return;
    }
    memcpy(*destination, sequence->steps, sequence->count * sizeof(**destination));
    sequence->steps = *destination;
    *destination += sequence->count;
}

// Construct an empty result owned by the semantic entry point.
SemaResult *new_sema_result(void)
{
    SemaResult *result = xcalloc(1, sizeof(*result));
    hashmap_init(&result->by_node, HASHMAP_KEY_POINTER);
    hashmap_init(&result->initializers, HASHMAP_KEY_POINTER);
    return result;
}

// Store a complete type at a stable address and own its parameter vector.
const SemaType *attach_type(SemaResult *result, SemaType type)
{
    OwnedType *owned = xmalloc(sizeof(*owned));
    *owned = (OwnedType){ .type = type, .next = result->types };
    result->types = owned;
    return &owned->type;
}

// Store a source-level entity independently of all its declaration records.
SemaEntity *attach_entity(SemaResult *result, SemaEntity entity)
{
    OwnedEntity *owned = xmalloc(sizeof(*owned));
    *owned = (OwnedEntity){ .entity = entity, .next = result->entities };
    result->entities = owned;
    return &owned->entity;
}

// Update the entity in place while retaining its name and linkage index keys.
void update_entity(SemaEntity *entity, SemaEntity value)
{
    entity->definition = value.definition;
    entity->type = value.type;
    entity->definition_node = value.definition_node;
    entity->u = value.u;
}

// Record that an accepted file-scope declaration requires an external definition.
void attach_external_function_declaration(SemaEntity *function)
{
    OwnedEntity *owned = (OwnedEntity *)function;
    owned->analysis.has_external_function_declaration = true;
}

// Retain the first reference to an internal-linkage name in this definition.
void attach_inline_reference(SemaEntity *function, const SourceSpan *span)
{
    OwnedEntity *owned = (OwnedEntity *)function;
    if (!owned->analysis.inline_internal_reference)
        owned->analysis.inline_internal_reference = span;
}

// Retain the first modifiable static or thread-local definition in this body.
void attach_inline_static(SemaEntity *function, const SourceSpan *span)
{
    OwnedEntity *owned = (OwnedEntity *)function;
    if (!owned->analysis.inline_modifiable_static)
        owned->analysis.inline_modifiable_static = span;
}

// Visit each entity and borrow its analysis facts for the duration of the callback.
void visit_sema_entities(SemaResult *result,
                          void (*visit)(SemaEntity *entity, const SemaEntityAnalysis *analysis, void *context),
                          void *context)
{
    for (OwnedEntity *owned = result->entities; owned; owned = owned->next)
        visit(&owned->entity, &owned->analysis, context);
}

// Attach a declaration record with stable identity and borrowed semantic type.
const SemaSymbol *attach_symbol(SemaResult *result, SemaSymbol value)
{
    const ResultEntry *entry = attach_entry(result, value.declaration, (ResultEntry){
        .kind = ENTRY_SYMBOL, .u.symbol = value,
    });
    return &entry->u.symbol;
}

// Publish a snapshot with the completed type while keeping earlier references valid.
const SemaSymbol *attach_symbol_revision(SemaResult *result, const SemaSymbol *initial,
                                         const SemaType *completed_type)
{
    ResultEntry *previous = hashmap_get_pointer(&result->by_node, initial->declaration);
    if (!previous || previous->kind != ENTRY_SYMBOL || &previous->u.symbol != initial)
        unreachable();
    ResultEntry *entry = xmalloc(sizeof(*entry));
    *entry = (ResultEntry){ .kind = ENTRY_SYMBOL, .previous_symbol = previous,
                            .u.symbol = *initial };
    entry->u.symbol.type = completed_type;
    hashmap_put_pointer(&result->by_node, initial->declaration, entry);
    return &entry->u.symbol;
}

// Take ownership of a checked initializer and its final step buffer.
void attach_initializer(SemaResult *result, const Node *owner, SemaInitializer *initializer)
{
    if (hashmap_get_pointer(&result->initializers, owner))
        unreachable();
    SemaInitializer *record = xmalloc(sizeof(*record));
    *record = *initializer;
    *initializer = (SemaInitializer){0};
    hashmap_put_pointer(&result->initializers, owner, record);
}

// Store an incomplete tag under a stable identity for self-references.
SemaTag *attach_tag(SemaResult *result, SemaTypeKind kind, const SourceSpan *declaration)
{
    SemaTag tag = { .declaration = declaration, .kind = kind, .u.aggregate = {0} };
    if (kind == SEMA_TYPE_ENUM)
        tag.u.integer_type = NULL;
    OwnedTag *owned = xmalloc(sizeof(*owned));
    *owned = (OwnedTag){ .tag = tag, .next = result->tags };
    result->tags = owned;
    return &owned->tag;
}

// Retain the definition span while the tag is still being analyzed.
void attach_tag_definition(SemaTag *tag, const SourceSpan *definition)
{
    tag->definition = definition;
}

// Complete a struct or union with its checked members and target layout.
void attach_tag_aggregate(SemaTag *tag, SemaAggregateInfo aggregate)
{
    tag->u.aggregate = aggregate;
    tag->is_complete = true;
}

// Complete an enum with the integer type that defines its target representation.
void attach_tag_integer_type(SemaTag *tag, const SemaType *integer_type)
{
    tag->u.integer_type = integer_type;
    tag->is_complete = true;
}

// Own one checked member and link it after the preceding member, if any.
void attach_tag_member(SemaResult *result, SemaMember *last, SemaMember member,
                       SemaMember **out)
{
    OwnedMember *owned = xmalloc(sizeof(*owned));
    *owned = (OwnedMember){ .member = member, .next = result->members };
    result->members = owned;
    if (last)
        last->next = &owned->member;
    *out = &owned->member;
}

// Attach the common facts of an expression that has no specialized payload.
void attach_expression(SemaResult *result, const Node *node, SemaExprProperties original)
{
    attach_expression_info(result, node, (SemaExprInfo){ .original = original, .kind = SEMA_EXPR_PLAIN });
}

// Store a resolved identifier and its immutable declaration snapshot.
void attach_identifier_expression(SemaResult *result, const Node *node,
                                    SemaExprProperties value, const SemaSymbol *symbol)
{
    attach_expression_info(result, node, (SemaExprInfo){
        .original = value, .kind = SEMA_EXPR_IDENTIFIER, .u.symbol = symbol,
    });
}

// Attach the type and target value of a completed sizeof query.
void attach_sizeof_expression(SemaResult *result, const Node *node,
                              SemaExprProperties value, size_t size)
{
    attach_expression_info(result, node, (SemaExprInfo){
        .original = value, .kind = SEMA_EXPR_SIZEOF, .u.sizeof_value = size,
    });
}

// Store the supplied type and target alignment of an alignment query.
void attach_alignof_expression(SemaResult *result, const Node *node,
                               SemaExprProperties value, size_t alignment)
{
    attach_expression_info(result, node, (SemaExprInfo){
        .original = value, .kind = SEMA_EXPR_ALIGNOF, .u.alignof_value = alignment,
    });
}

// Store the type and storage duration of an unnamed compound-literal object.
void attach_compound_literal(SemaResult *result, const Node *node,
                              SemaExprProperties value, bool static_duration)
{
    attach_expression_info(result, node, (SemaExprInfo){
        .original = value, .kind = SEMA_EXPR_COMPOUND_LITERAL, .u.static_duration = static_duration,
    });
}

// Store the selected expression and its inherited type and value category.
void attach_generic_selection(SemaResult *result, const Node *node,
                               SemaExprProperties value, const Node *selected)
{
    attach_expression_info(result, node, (SemaExprInfo){
        .original = value, .kind = SEMA_EXPR_GENERIC_SELECTION, .u.selected = selected,
    });
}

// Store the result and ordering requirement of a checked simple assignment.
void attach_assignment_expression(SemaResult *result, const Node *node,
                                   SemaExprProperties value, bool atomic_store)
{
    attach_expression_info(result, node, (SemaExprInfo){
        .original = value, .kind = SEMA_EXPR_ASSIGNMENT, .u.atomic_store = atomic_store,
    });
}

// Store the member and target byte offset resolved by expression analysis.
void attach_member_expression(SemaResult *result, const Node *node,
                              SemaExprProperties value, const SemaMember *member, uint64_t offset)
{
    attach_expression_info(result, node, (SemaExprInfo){
        .original = value, .kind = SEMA_EXPR_MEMBER, .u.member = { .field = member, .offset = offset },
    });
}

// Store the branch test alongside the conditional expression's value.
void attach_conditional_expression(SemaResult *result, const Node *node,
                                   SemaExprProperties value, SemaConditionInfo condition)
{
    attach_expression_info(result, node, (SemaExprInfo){
        .original = value, .kind = SEMA_EXPR_CONDITIONAL, .u.condition = condition,
    });
}

// Own a completed update and its conversion chains with one result allocation.
void attach_update_expression(SemaResult *result, const Node *node,
                               SemaExprProperties value, SemaUpdateInfo update)
{
    size_t count = update.target_read.count + update.implicit_rhs.count +
                   update.writeback.count;
    OwnedUpdate *owned = xmalloc(sizeof(*owned) + count * sizeof(*owned->steps));
    SemaConversion *next = owned->steps;
    attach_update_sequence(&update.target_read, &next);
    attach_update_sequence(&update.implicit_rhs, &next);
    attach_update_sequence(&update.writeback, &next);
    owned->info = update;
    attach_expression_info(result, node, (SemaExprInfo){
        .original = value, .kind = SEMA_EXPR_UPDATE, .u.update = &owned->info,
    });
}

// Append a completed chain and copy its steps into result-owned storage.
void attach_conversion_sequence(SemaResult *result, const Node *node,
                                SemaConversionSequence sequence)
{
    if (!sequence.count)
        return;
    ResultEntry *entry = hashmap_get_pointer(&result->by_node, node);
    if (!entry || entry->kind != ENTRY_EXPRESSION)
        unreachable();
    SemaExprInfo *info = &entry->u.expression;
    SemaExprProperties converted = sema_converted_properties(info);
    for (size_t i = 0; i < sequence.count; ++i) {
        const SemaConversion *step = &sequence.steps[i];
        if (converted.type != step->from.type || converted.category != step->from.category ||
            converted.bit_field != step->from.bit_field ||
            converted.register_object != step->from.register_object)
            unreachable();
        converted = step->to;
    }
    size_t count = info->conversions.count;
    SemaConversion *steps = xcalloc(count + sequence.count, sizeof(*steps));
    if (count)
        memcpy(steps, info->conversions.steps, count * sizeof(*steps));
    memcpy(steps + count, sequence.steps, sequence.count * sizeof(*steps));
    free((void *)info->conversions.steps);
    info->conversions = (SemaConversionSequence){ .steps = steps, .count = count + sequence.count };
}

// Store the source node chosen for a checked goto, break or continue.
void attach_jump_target(SemaResult *result, const Node *node, const Node *target)
{
    attach_entry(result, node, (ResultEntry){ .kind = ENTRY_JUMP_TARGET, .u.jump_target = target });
}

// Store the checked label and its enclosing switch without owning AST nodes.
void attach_switch_label(SemaResult *result, const Node *node, SemaSwitchLabelInfo label)
{
    attach_entry(result, node, (ResultEntry){ .kind = ENTRY_SWITCH_LABEL, .u.switch_label = label });
}

// Store the condition selected by a checked selection or iteration statement.
void attach_condition(SemaResult *result, const Node *node, SemaConditionInfo condition)
{
    attach_entry(result, node, (ResultEntry){ .kind = ENTRY_CONDITION, .u.condition = condition });
}

// Release owned records and bucket storage while leaving the AST keys borrowed.
void sema_free(SemaResult *result)
{
    if (!result)
        return;
    hashmap_foreach_value(&result->by_node, free_entry);
    hashmap_free(&result->by_node);
    hashmap_foreach_value(&result->initializers, free_initializer);
    hashmap_free(&result->initializers);
    while (result->types) {
        OwnedType *owned = result->types;
        result->types = owned->next;
        if (owned->type.kind == SEMA_TYPE_FUNCTION)
            free((void *)owned->type.u.function.parameters);
        free(owned);
    }
    while (result->entities) {
        OwnedEntity *owned = result->entities;
        result->entities = owned->next;
        free(owned);
    }
    while (result->tags) {
        OwnedTag *owned = result->tags;
        result->tags = owned->next;
        free(owned);
    }
    while (result->members) {
        OwnedMember *owned = result->members;
        result->members = owned->next;
        free(owned);
    }
    free(result);
}

// Read a published declaration binding with identity stable until sema_free().
const SemaSymbol *sema_lookup_symbol(const SemaResult *result, const Node *node)
{
    const ResultEntry *entry = hashmap_get_pointer(&result->by_node, node);
    return entry && entry->kind == ENTRY_SYMBOL ? &entry->u.symbol : NULL;
}

// Read a published expression record, whose address also survives table growth.
const SemaExprInfo *sema_lookup_expression(const SemaResult *result,
                                          const Node *node)
{
    const ResultEntry *entry = hashmap_get_pointer(&result->by_node, node);
    return entry && entry->kind == ENTRY_EXPRESSION ? &entry->u.expression : NULL;
}

// Read the published target of a goto, break or continue.
const Node *sema_lookup_jump_target(const SemaResult *result, const Node *node)
{
    const ResultEntry *entry = hashmap_get_pointer(&result->by_node, node);
    return entry && entry->kind == ENTRY_JUMP_TARGET ? entry->u.jump_target : NULL;
}

// Read a case or default label's published switch association.
const SemaSwitchLabelInfo *sema_lookup_switch_label(const SemaResult *result, const Node *node)
{
    const ResultEntry *entry = hashmap_get_pointer(&result->by_node, node);
    return entry && entry->kind == ENTRY_SWITCH_LABEL ? &entry->u.switch_label : NULL;
}

// Read a statement or expression nonzero test, including an omitted for condition.
const SemaConditionInfo *sema_lookup_condition(const SemaResult *result, const Node *node)
{
    const ResultEntry *entry = hashmap_get_pointer(&result->by_node, node);
    if (!entry)
        return NULL;
    if (entry->kind == ENTRY_CONDITION)
        return &entry->u.condition;
    if (entry->kind == ENTRY_EXPRESSION && entry->u.expression.kind == SEMA_EXPR_CONDITIONAL)
        return &entry->u.expression.u.condition;
    return NULL;
}

// Read a published initializer associated with a declaration or compound literal.
const SemaInitializer *sema_lookup_initializer(const SemaResult *result, const Node *node)
{
    return hashmap_get_pointer(&result->initializers, node);
}
