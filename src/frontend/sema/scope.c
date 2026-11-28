// Manage lexical bindings, translation-unit linkage and member lookup.

#include "sema_internal.h"

#include <stdlib.h>

typedef struct {
    SemaEntity *entity;
    unsigned failed_file_declarations;
    SemaFailedObjectDefinition *failed_objects;
} SemaLinkageBinding;

// Initialize an unlinked frame with an empty ordinary-name table.
static void init_sema_scope(SemaScope *scope, SemaScope *parent)
{
    *scope = (SemaScope){ .parent = parent };
    hashmap_init(&scope->names, HASHMAP_KEY_STRING_VIEW);
    hashmap_init(&scope->tags, HASHMAP_KEY_STRING_VIEW);
}

// Read a slot in one linkage namespace, including slots without an accepted entity.
static SemaLinkageBinding *lookup_linkage_binding(const SemaLinkageIndex *index,
                                                   SemaLinkage linkage, StringView name)
{
    switch (linkage) {
    case SEMA_LINKAGE_NONE: return NULL;
    case SEMA_LINKAGE_INTERNAL: return hashmap_get(&index->internal_bindings, name);
    case SEMA_LINKAGE_EXTERNAL: return hashmap_get(&index->external_bindings, name);
    }
    unreachable();
}

// Reserve a slot in the selected linkage namespace, keeping any existing facts.
static SemaLinkageBinding *bind_linkage_slot(SemaLinkageIndex *index, SemaLinkage linkage,
                                               StringView name)
{
    if (linkage == SEMA_LINKAGE_NONE)
        unreachable();
    SemaLinkageBinding *binding = lookup_linkage_binding(index, linkage, name);
    if (!binding) {
        HashMap *map = linkage == SEMA_LINKAGE_INTERNAL
            ? &index->internal_bindings : &index->external_bindings;
        binding = xcalloc(1, sizeof(*binding));
        hashmap_put(map, name, binding);
    }
    return binding;
}

// Release a linkage slot and its failed declarations, which borrow result-owned types.
static void free_linkage_binding(void *value)
{
    SemaLinkageBinding *binding = value;
    SemaFailedObjectDefinition *failed = binding->failed_objects;
    while (failed) {
        SemaFailedObjectDefinition *next = failed->next;
        free(failed);
        failed = next;
    }
    free(binding);
}

// Return the route length, or zero on miss. A supplied buffer must hold the
// maximum path depth. Recursion descends only into contained anonymous types.
static size_t lookup_member_path(const SemaMember *members, StringView name,
                                 const SemaMember **path)
{
    for (const SemaMember *member = members; member; member = member->next) {
        size_t depth = 0;
        if (member->kind == SEMA_MEMBER_ANONYMOUS) {
            depth = lookup_member_path(member->type->u.tag->u.aggregate.members, name,
                                       path ? path + 1 : NULL);
            if (!depth)
                continue;
        } else if (!string_view_length(member->name) || !string_view_equal(member->name, name)) {
            continue;
        }
        if (path)
            *path = member;
        return 1 + depth;
    }
    return 0;
}

// Initialize the borrowed-name indices for the translation unit.
void init_linkage_index(SemaLinkageIndex *index)
{
    hashmap_init(&index->internal_bindings, HASHMAP_KEY_STRING_VIEW);
    hashmap_init(&index->external_bindings, HASHMAP_KEY_STRING_VIEW);
}

// Release the index-owned slots. Semantic entities remain result-owned.
void free_linkage_index(SemaLinkageIndex *index)
{
    hashmap_foreach_value(&index->external_bindings, free_linkage_binding);
    hashmap_foreach_value(&index->internal_bindings, free_linkage_binding);
    hashmap_free(&index->external_bindings);
    hashmap_free(&index->internal_bindings);
}

// Read the accepted entity for a name without treating recovery facts as a declaration.
SemaEntity *lookup_linked_entity(const SemaLinkageIndex *index, SemaLinkage linkage,
                                StringView name)
{
    const SemaLinkageBinding *binding = lookup_linkage_binding(index, linkage, name);
    return binding ? binding->entity : NULL;
}

// Index an already accepted entity while retaining failed file declaration facts.
void bind_linked_entity(SemaLinkageIndex *index, SemaEntity *entity)
{
    bind_linkage_slot(index, entity->linkage, entity->name)->entity = entity;
}

// Read the recorded kinds of failed file-scope declarations for this linkage name.
unsigned lookup_linkage_recovery(const SemaLinkageIndex *index, SemaLinkage linkage,
                                  StringView name)
{
    const SemaLinkageBinding *binding = lookup_linkage_binding(index, linkage, name);
    return binding ? binding->failed_file_declarations : 0;
}

// Retain failed file declaration facts even if no accepted entity exists yet.
void attach_linkage_recovery(SemaLinkageIndex *index, SemaLinkage linkage, StringView name,
                              unsigned facts)
{
    bind_linkage_slot(index, linkage, name)->failed_file_declarations |= facts;
}

// Borrow the candidate definitions recorded under this linkage name.
const SemaFailedObjectDefinition *lookup_failed_object_definitions(const SemaLinkageIndex *index,
                                                                     SemaLinkage linkage, StringView name)
{
    const SemaLinkageBinding *binding = lookup_linkage_binding(index, linkage, name);
    return binding ? binding->failed_objects : NULL;
}

// Store an uncommitted definition's reason and type for the end-of-file check.
void attach_failed_object_definition(SemaLinkageIndex *index, SemaLinkage linkage, StringView name,
                                       const SemaType *type, SemaObjectDefinitionFailure reason)
{
    SemaLinkageBinding *binding = bind_linkage_slot(index, linkage, name);
    SemaFailedObjectDefinition *failed = xmalloc(sizeof(*failed));
    *failed = (SemaFailedObjectDefinition){ .type = type, .reason = reason,
                                           .next = binding->failed_objects };
    binding->failed_objects = failed;
}

// Enter a scope frame owned by the caller for the duration of this traversal.
void push_scope(SemaContext *ctx, SemaScope *scope)
{
    init_sema_scope(scope, ctx->scope);
    ctx->scope = scope;
}

// Restore the parent frame and release this frame's name index.
void pop_scope(SemaContext *ctx)
{
    SemaScope *scope = ctx->scope;
    ctx->scope = scope->parent;
    hashmap_foreach_value(&scope->names, free);
    hashmap_free(&scope->names);
    hashmap_free(&scope->tags);
}

// Select an already initialized frame without changing either frame's lifetime.
void update_current_scope(SemaContext *ctx, SemaScope *scope)
{
    ctx->scope = scope;
}

// Stop name-dependent analysis in a frame whose declarations could not be resolved.
void mark_scope_visibility_unknown(SemaScope *scope)
{
    scope->visibility_unknown = true;
}

// Read a binding from this scope only, for same-scope declaration checks.
const SemaBinding *lookup_current_binding(const SemaScope *scope, StringView name)
{
    return hashmap_get(&scope->names, name);
}

// Find the nearest visible ordinary-name binding.
const SemaBinding *lookup_visible_binding(const SemaScope *scope, StringView name)
{
    for (; scope; scope = scope->parent) {
        const SemaBinding *binding = lookup_current_binding(scope, name);
        if (binding)
            return binding;
    }
    return NULL;
}

// Store a checked or failed binding in a scope-owned slot, borrowing symbols and keys.
void bind_scope_name(SemaScope *scope, StringView name, SemaBinding binding)
{
    SemaBinding *slot = hashmap_get(&scope->names, name);
    if (!slot) {
        slot = xmalloc(sizeof(*slot));
        hashmap_put(&scope->names, name, slot);
    }
    *slot = binding;
}

// Read a tag from this scope only, including an incomplete tag declaration.
SemaTag *lookup_current_tag(const SemaScope *scope, StringView name)
{
    return hashmap_get(&scope->tags, name);
}

// Find the nearest tag in the shared struct, union and enum namespace.
SemaTag *lookup_visible_tag(const SemaScope *scope, StringView name)
{
    for (; scope; scope = scope->parent) {
        SemaTag *tag = lookup_current_tag(scope, name);
        if (tag)
            return tag;
    }
    return NULL;
}

// Index a checked tag while borrowing the source spelling.
void bind_tag(SemaScope *scope, StringView name, SemaTag *tag)
{
    hashmap_put(&scope->tags, name, tag);
}

// Check the member namespace, including names promoted by anonymous aggregates.
bool check_member_exists(const SemaMember *members, StringView name)
{
    return lookup_member_path(members, name, NULL) != 0;
}

// Release and clear the owned route buffer while leaving its members borrowed.
void free_member_path(SemaMemberPath *path)
{
    free(path->members);
    *path = (SemaMemberPath){0};
}

// Return a caller-owned route to a named member, or an empty route on miss.
SemaMemberPath new_member_path(const SemaAggregateInfo *aggregate, StringView name)
{
    if (!aggregate->member_path_depth)
        return (SemaMemberPath){0};

    SemaMemberPath path = {
        .members = xcalloc(aggregate->member_path_depth, sizeof(*path.members)),
    };
    path.count = lookup_member_path(aggregate->members, name, path.members);
    if (!path.count)
        free_member_path(&path);
    return path;
}
