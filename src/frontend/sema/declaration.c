// Resolve declaration syntax, apply declaration constraints and bind source entities.

#include "sema_internal.h"

#include "util/diagnostic.h"

#include <stdlib.h>

typedef struct {
    const Declarator *identifier;
    const SemaType *type;
    const SemaType *declared_type;
    const SemaType *return_type; // Independently typed return of a function definition.
    const SourceSpan *variable_bound;
    unsigned array_qualifiers;
    uint64_t minimum_array_count;
    bool is_vla;
    bool is_function; // Outer form when a variably modified type cannot be built.
} SemaDeclarator;

typedef struct {
    const SemaType **types;
    size_t count;
} SemaParameters;

typedef enum {
    ARRAY_BOUND_OMITTED,
    ARRAY_BOUND_FIXED,
    ARRAY_BOUND_RUNTIME,
    ARRAY_BOUND_STAR,
} ArrayBoundKind;

typedef struct {
    ArrayBoundKind kind;
    uint64_t count;
} SemaArrayBound;

typedef struct {
    const Declarator *syntax;
    SemaArrayBound bound;
    SemaParameters parameters;
    SemaStatus operand_status;
    SemaFunctionForm function_form;
} SemaDeclaratorLayer;

typedef struct {
    SemaTypeKind kind;
    SemaMember *first;
    SemaMember *last;
    size_t member_path_depth;
    SemaAggregateLayout layout;
    unsigned flags; // SEMA_AGGREGATE_* flags
    bool layout_known;
} SemaAggregateDefinition;

typedef struct {
    const Node *node;
    SemaDeclarator declarator;
    StorageClass storage;
    SemaDefinition definition;
    unsigned alignment;
    bool has_alignment;
    unsigned function_specifiers;
} SemaDeclaration;

typedef enum {
    DECLARATION_VALID,
    DECLARATION_INCOMPLETE_OBJECT,
    DECLARATION_BLOCK_FUNCTION_STORAGE,
    DECLARATION_REDECLARATION,
    DECLARATION_CONFLICTING_LINKAGE,
    DECLARATION_CONFLICTING_TYPES,
    DECLARATION_MULTIPLE_DEFINITIONS,
    DECLARATION_INVALID_ATOMIC,
    DECLARATION_INVALID_RESTRICT,
    DECLARATION_QUALIFIED_FUNCTION,
    DECLARATION_INVALID_TYPEDEF,
    DECLARATION_FAILED_TYPEDEF,
    DECLARATION_INVALID_VOID_PARAMETER,
    DECLARATION_UNNAMED_PARAMETER,
    DECLARATION_FUNCTION_RETURNING_FUNCTION,
    DECLARATION_INVALID_INITIALIZER,
    DECLARATION_NONCONSTANT_INITIALIZER,
    DECLARATION_NONOBJECT_FOR,
    DECLARATION_ALIGNMENT_TYPE,
    DECLARATION_ALIGNMENT_VALUE,
    DECLARATION_ALIGNAS_CONTEXT,
    DECLARATION_WEAK_ALIGNMENT,
    DECLARATION_CONFLICTING_ALIGNMENT,
    DECLARATION_FUNCTION_SPECIFIER,
    DECLARATION_MAIN_SPECIFIER,
    DECLARATION_THREAD_CONTEXT,
    DECLARATION_THREAD_REDECLARATION,
    DECLARATION_INVALID_ARRAY_ELEMENT,
    DECLARATION_ARRAY_BOUND_TYPE,
    DECLARATION_ARRAY_BOUND_VALUE,
    DECLARATION_ARRAY_BOUND_CONSTANT,
    DECLARATION_ARRAY_SIZE,
    DECLARATION_VM_SCOPE,
    DECLARATION_VM_LINKAGE,
    DECLARATION_VM_MEMBER,
    DECLARATION_VLA_STORAGE,
    DECLARATION_VLA_INITIALIZER,
    DECLARATION_UNSUPPORTED_VLA,
    DECLARATION_UNSUPPORTED_VM,
    DECLARATION_UNSUPPORTED_OLD_STYLE,
    DECLARATION_FUNCTION_RETURNING_ARRAY,
    DECLARATION_INCOMPLETE_RETURN,
    DECLARATION_TAG_KIND,
    DECLARATION_ENUM_INCOMPLETE,
    DECLARATION_DUPLICATE_TAG,
    DECLARATION_INVALID_ENUMERATOR,
    DECLARATION_DUPLICATE_ENUMERATOR,
    DECLARATION_MEMBER_TYPE,
    DECLARATION_DUPLICATE_MEMBER,
    DECLARATION_BIT_FIELD_TYPE,
    DECLARATION_BIT_FIELD_WIDTH,
    DECLARATION_BIT_FIELD_WIDTH_LIMIT,
    DECLARATION_AGGREGATE_SIZE,
} DeclarationIssue;

typedef struct {
    SemaStatus status;
    DeclarationIssue issue;
    const SemaType *type;
} DeclarationFailure;

typedef struct {
    SemaSymbolKind kind;
    SemaLinkage linkage;
    SemaBinding previous;
    const SemaType *prior_visible_type;
    SemaEntity *entity;
    const SemaEntity *opposite;
} DeclarationBinding;

static SemaStatus analyze_declarator(SemaContext *ctx, const Declarator *declarator,
                              const SemaType *base, bool definition,
                              SemaDeclarator *out);

static SemaStatus analyze_tag_specifier(SemaContext *ctx, const TypeSpecifier *spec,
                                        bool standalone, const SemaType **out);

// ---------------------------------------------------------------------------
// Declaration identity, binding and diagnostics
// ---------------------------------------------------------------------------

// Classify the entity from the completed type, including function typedefs.
static SemaSymbolKind declaration_kind(SemaDeclaration declaration)
{
    if (declaration.storage == STORAGE_TYPEDEF)
        return SEMA_SYMBOL_TYPEDEF;
    const SemaType *type = declaration.declarator.type;
    bool function = type ? type->kind == SEMA_TYPE_FUNCTION : declaration.declarator.is_function;
    return function ? SEMA_SYMBOL_FUNCTION : SEMA_SYMBOL_OBJECT;
}

// Determine linkage from the current context and the nearest visible name.
static SemaLinkage resolve_linkage(const SemaScope *scope, SemaSymbolKind kind,
                                   SemaDeclaration declaration, const SemaSymbol *visible)
{
    if (kind == SEMA_SYMBOL_TYPEDEF || declaration.node->type == ND_PARAMETER)
        return SEMA_LINKAGE_NONE;
    if (!scope->parent && (declaration.storage & STORAGE_STATIC))
        return SEMA_LINKAGE_INTERNAL;
    if ((declaration.storage & STORAGE_EXTERN) || kind == SEMA_SYMBOL_FUNCTION) {
        if (visible && visible->entity && visible->entity->linkage != SEMA_LINKAGE_NONE)
            return visible->entity->linkage;
        return SEMA_LINKAGE_EXTERNAL;
    }
    return scope->parent ? SEMA_LINKAGE_NONE : SEMA_LINKAGE_EXTERNAL;
}

// Interpret the lexical and linkage lookups at this point of declaration.
static DeclarationBinding resolve_declaration_binding(const SemaContext *ctx,
                                                       const SemaScope *scope,
                                                       SemaDeclaration declaration)
{
    StringView name = declaration.declarator.identifier->u.ident.name;
    const SemaBinding *current = lookup_current_binding(scope, name);
    const SemaBinding *nearest = current ? current : lookup_visible_binding(scope->parent, name);
    DeclarationBinding binding = { .kind = declaration_kind(declaration) };
    if (current)
        binding.previous = *current;
    const SemaSymbol *visible = nearest ? nearest->symbol : NULL;
    binding.linkage = resolve_linkage(scope, binding.kind, declaration, visible);
    if (binding.linkage != SEMA_LINKAGE_NONE) {
        if (visible && visible->entity && visible->entity->linkage == binding.linkage)
            binding.prior_visible_type = visible->type;
        SemaLinkage opposite = binding.linkage == SEMA_LINKAGE_EXTERNAL
                                   ? SEMA_LINKAGE_INTERNAL : SEMA_LINKAGE_EXTERNAL;
        binding.entity = lookup_linked_entity(&ctx->linkage, binding.linkage, name);
        binding.opposite = lookup_linked_entity(&ctx->linkage, opposite, name);
    }
    return binding;
}

// Check a redeclaration in one ordinary namespace. The typedef exception
// applies only to the very same type.
static bool check_redeclaration(const SemaSymbol *previous, SemaSymbolKind kind,
                                SemaLinkage linkage, const SemaType *type)
{
    if (previous->kind != kind)
        return false;
    if (kind == SEMA_SYMBOL_TYPEDEF)
        return type_is_identical(previous->type, type);
    return linkage != SEMA_LINKAGE_NONE && previous->entity->linkage == linkage &&
           type_is_compatible(previous->type, type);
}

// Allow incomplete declarations until their point of required completion.
static bool check_incomplete_object(SemaDeclaration declaration, SemaLinkage linkage)
{
    if (declaration.definition == SEMA_DECLARED)
        return true;
    if (declaration.definition == SEMA_TENTATIVE)
        return linkage == SEMA_LINKAGE_EXTERNAL &&
               declaration.declarator.type->kind != SEMA_TYPE_VOID;
    return declaration.node->type == ND_INIT_DECL && declaration.node->u.init_decl.init &&
           declaration.declarator.type->kind == SEMA_TYPE_ARRAY;
}

// Check the storage class, declaration attributes and their redeclarations.
static DeclarationIssue check_declaration_attributes(SemaDeclaration declaration,
                                                     DeclarationBinding binding)
{
    const SemaType *type = declaration.declarator.type;
    bool thread = (declaration.storage & STORAGE_THREAD_LOCAL) != 0;
    if (thread && binding.kind != SEMA_SYMBOL_OBJECT)
        return DECLARATION_THREAD_CONTEXT;
    if (binding.entity && binding.kind == SEMA_SYMBOL_OBJECT &&
        thread != (binding.entity->u.object.duration == SEMA_DURATION_THREAD))
        return DECLARATION_THREAD_REDECLARATION;
    if (declaration.function_specifiers) {
        if (binding.kind != SEMA_SYMBOL_FUNCTION)
            return DECLARATION_FUNCTION_SPECIFIER;
        if (string_view_equal_cstr(declaration.declarator.identifier->u.ident.name, "main"))
            return DECLARATION_MAIN_SPECIFIER;
    }
    if (declaration.has_alignment &&
        (binding.kind != SEMA_SYMBOL_OBJECT || declaration.node->type == ND_PARAMETER ||
         (declaration.storage & STORAGE_REGISTER)))
        return DECLARATION_ALIGNAS_CONTEXT;
    if (declaration.alignment &&
        (type_is_complete_object(type) || type->kind == SEMA_TYPE_ARRAY) &&
        declaration.alignment < type_alignment(type))
        return DECLARATION_WEAK_ALIGNMENT;
    if (binding.entity && binding.kind == SEMA_SYMBOL_OBJECT) {
        unsigned previous = binding.entity->u.object.alignment;
        if (previous && declaration.alignment && previous != declaration.alignment)
            return DECLARATION_CONFLICTING_ALIGNMENT;
        if ((binding.entity->definition == SEMA_DEFINED && declaration.alignment &&
             declaration.alignment != binding.entity->u.object.definition_alignment) ||
            (declaration.definition == SEMA_DEFINED && previous && previous != declaration.alignment))
            return DECLARATION_CONFLICTING_ALIGNMENT;
    }
    return DECLARATION_VALID;
}

// Check lexical and linkage constraints before preparing any change to an entity.
static DeclarationIssue check_declaration_binding(const SemaScope *scope,
                                                  SemaDeclaration declaration,
                                                  DeclarationBinding binding)
{
    const SemaType *type = declaration.declarator.type;
    if (binding.kind == SEMA_SYMBOL_OBJECT && !type_is_complete_object(type) &&
        !(binding.prior_visible_type && type_is_complete_object(binding.prior_visible_type)) &&
        !check_incomplete_object(declaration, binding.linkage))
        return DECLARATION_INCOMPLETE_OBJECT;
    if (binding.kind == SEMA_SYMBOL_FUNCTION && scope->parent &&
        declaration.storage != STORAGE_NONE && declaration.storage != STORAGE_EXTERN)
        return DECLARATION_BLOCK_FUNCTION_STORAGE;
    if (binding.previous.symbol &&
        !check_redeclaration(binding.previous.symbol, binding.kind, binding.linkage, type))
        return DECLARATION_REDECLARATION;
    if (binding.opposite)
        return DECLARATION_CONFLICTING_LINKAGE;
    if (binding.entity && !type_is_compatible(binding.entity->type, type))
        return DECLARATION_CONFLICTING_TYPES;
    if (binding.entity && binding.entity->definition == SEMA_DEFINED &&
        declaration.definition == SEMA_DEFINED)
        return DECLARATION_MULTIPLE_DEFINITIONS;
    return check_declaration_attributes(declaration, binding);
}

// Classify a checked entity's duration independently of lexical linkage.
static SemaStorageDuration resolve_entity_duration(DeclarationBinding binding, StorageClass storage)
{
    if (binding.kind != SEMA_SYMBOL_OBJECT)
        return SEMA_DURATION_NONE;
    if (storage & STORAGE_THREAD_LOCAL)
        return SEMA_DURATION_THREAD;
    if (binding.linkage != SEMA_LINKAGE_NONE || (storage & STORAGE_STATIC))
        return SEMA_DURATION_STATIC;
    return SEMA_DURATION_AUTOMATIC;
}

// Resolve the definition state together with the declaration that established it.
static SemaDefinition resolve_entity_definition(const SemaEntity *entity,
                                                 SemaDeclaration declaration,
                                                 const Node **definition_node)
{
    SemaDefinition previous = entity ? entity->definition : SEMA_DECLARED;
    if (declaration.definition == SEMA_DEFINED ||
        (declaration.definition == SEMA_TENTATIVE && previous == SEMA_DECLARED)) {
        *definition_node = declaration.node;
        return declaration.definition;
    }
    *definition_node = entity ? entity->definition_node : NULL;
    return previous;
}

// Commit entity data at a stable address and index it when first accepted.
static SemaEntity *bind_entity(SemaContext *ctx, DeclarationBinding binding,
                               SemaEntity *entity, SemaEntity value)
{
    if (entity)
        update_entity(entity, value);
    else
        entity = attach_entity(ctx->result, value);
    if (!binding.entity && value.linkage != SEMA_LINKAGE_NONE)
        bind_linked_entity(&ctx->linkage, entity);
    return entity;
}

// Commit a lexical declaration with its own immutable effective type.
static const SemaSymbol *bind_declaration(SemaResult *result, SemaScope *scope,
                                         SemaDeclaration declaration,
                                         SemaSymbolKind kind, const SemaType *type,
                                         const SemaEntity *entity)
{
    const Declarator *identifier = declaration.declarator.identifier;
    SemaSymbol value = {
        .declaration = declaration.node,
        .name_span = &identifier->span,
        .type = type,
        .declared_type = declaration.declarator.declared_type
            ? declaration.declarator.declared_type : declaration.declarator.type,
        .kind = kind,
        .storage = declaration.storage,
        .entity = entity,
    };
    if (kind == SEMA_SYMBOL_OBJECT) {
        value.u.object.minimum_array_count = declaration.declarator.minimum_array_count;
        value.u.object.alignment = declaration.alignment;
    } else if (kind == SEMA_SYMBOL_FUNCTION) {
        value.u.function_specifiers = declaration.function_specifiers;
    }
    const SemaSymbol *symbol = attach_symbol(result, value);
    bind_scope_name(scope, identifier->u.ident.name, (SemaBinding){ .symbol = symbol });
    return symbol;
}

// Report a same-scope conflict at the new name and cite the earlier declaration.
static SemaStatus report_redeclaration(const Declarator *identifier,
                                       const SemaSymbol *previous)
{
    StringView name = identifier->u.ident.name;
    error_at(&identifier->span,
             "redeclaration of '%.*s' (previous declaration at %u:%u)",
             (int)string_view_length(name), string_view_begin(name),
             previous->name_span->line, previous->name_span->column);
    return SEMA_INVALID;
}

// Render a declaration or type constraint at the syntax that introduced it.
static SemaStatus report_declaration_issue(const SourceSpan *span, DeclarationIssue issue)
{
    static const char *const g_messages[] = {
        [DECLARATION_INCOMPLETE_OBJECT] = "object has incomplete type",
        [DECLARATION_BLOCK_FUNCTION_STORAGE] = "invalid storage class for block-scope function",
        [DECLARATION_CONFLICTING_LINKAGE] = "conflicting linkage for declaration",
        [DECLARATION_CONFLICTING_TYPES] = "conflicting types for linked declaration",
        [DECLARATION_MULTIPLE_DEFINITIONS] = "multiple definitions of linked entity",
        [DECLARATION_INVALID_RESTRICT] = "invalid use of 'restrict'",
        [DECLARATION_QUALIFIED_FUNCTION] = "qualified function type",
        [DECLARATION_INVALID_TYPEDEF] = "type name does not denote a typedef",
        [DECLARATION_INVALID_VOID_PARAMETER] = "invalid void parameter",
        [DECLARATION_UNNAMED_PARAMETER] = "function definition parameter has no name",
        [DECLARATION_FUNCTION_RETURNING_FUNCTION] = "function cannot return a function",
        [DECLARATION_INVALID_INITIALIZER] = "initializer is invalid for this declaration",
        [DECLARATION_NONCONSTANT_INITIALIZER] = "initializer is not a constant expression",
        [DECLARATION_ALIGNMENT_TYPE] = "alignment requires a complete object type",
        [DECLARATION_ALIGNMENT_VALUE] = "alignment must be a supported integer constant",
        [DECLARATION_ALIGNAS_CONTEXT] = "alignment specifier is invalid for this declaration",
        [DECLARATION_WEAK_ALIGNMENT] = "alignment specifier reduces the required alignment",
        [DECLARATION_CONFLICTING_ALIGNMENT] = "conflicting alignment for object declaration",
        [DECLARATION_FUNCTION_SPECIFIER] = "function specifier applied to a non-function declaration",
        [DECLARATION_MAIN_SPECIFIER] = "function specifier applied to main",
        [DECLARATION_THREAD_CONTEXT] = "_Thread_local requires an object declaration",
        [DECLARATION_THREAD_REDECLARATION] = "thread-local declaration conflicts with a prior storage duration",
        [DECLARATION_NONOBJECT_FOR] = "for loop initial declaration must declare an object",
        [DECLARATION_INVALID_ATOMIC] = "invalid type for _Atomic",
        [DECLARATION_INVALID_ARRAY_ELEMENT] = "array element type must be a complete object type",
        [DECLARATION_ARRAY_BOUND_TYPE] = "array bound is not an integer",
        [DECLARATION_ARRAY_BOUND_VALUE] = "array bound must be greater than zero",
        [DECLARATION_ARRAY_BOUND_CONSTANT] = "array bound constant expression cannot be evaluated",
        [DECLARATION_ARRAY_SIZE] = "array size exceeds the target object limit",
        [DECLARATION_VM_SCOPE] = "variably modified type is not allowed at file scope",
        [DECLARATION_VM_LINKAGE] = "an identifier with variably modified type must have no linkage",
        [DECLARATION_VM_MEMBER] = "a structure or union member cannot have variably modified type",
        [DECLARATION_VLA_STORAGE] = "an object with static or thread storage duration cannot have variable length array type",
        [DECLARATION_VLA_INITIALIZER] = "a variable length array cannot be initialized",
        [DECLARATION_UNSUPPORTED_VLA] = "variable length arrays are not supported",
        [DECLARATION_UNSUPPORTED_VM] = "variably modified types are not supported",
        [DECLARATION_UNSUPPORTED_OLD_STYLE] = "K&R-style function definitions are not supported",
        [DECLARATION_FUNCTION_RETURNING_ARRAY] = "function cannot return an array",
        [DECLARATION_INCOMPLETE_RETURN] = "function definition has incomplete return type",
        [DECLARATION_TAG_KIND] = "tag refers to a different kind of type",
        [DECLARATION_ENUM_INCOMPLETE] = "enum type is not complete",
        [DECLARATION_DUPLICATE_TAG] = "redefinition of tag",
        [DECLARATION_INVALID_ENUMERATOR] = "enumerator must have a constant value representable as int",
        [DECLARATION_DUPLICATE_ENUMERATOR] = "redeclaration of enumerator",
        [DECLARATION_MEMBER_TYPE] = "invalid or incomplete member type",
        [DECLARATION_DUPLICATE_MEMBER] = "duplicate member name",
        [DECLARATION_BIT_FIELD_TYPE] = "invalid bit-field type",
        [DECLARATION_BIT_FIELD_WIDTH] = "bit-field width must be a nonnegative integer constant expression and nonzero when named",
        [DECLARATION_BIT_FIELD_WIDTH_LIMIT] = "bit-field width exceeds its type's width",
        [DECLARATION_AGGREGATE_SIZE] = "aggregate size exceeds the target object limit",
    };
    error_at(span, "%s", g_messages[issue]);
    return SEMA_INVALID;
}

// Decide whether a rejected object declaration could still supply definition facts.
static bool check_failed_object_candidate(const Node *node, DeclarationFailure failure)
{
    return node->type == ND_INIT_DECL && node->u.init_decl.init &&
           (failure.issue == DECLARATION_VALID || failure.issue == DECLARATION_INCOMPLETE_OBJECT);
}

// Preserve end-of-file facts a rejected declaration could have supplied to this name.
static void recover_file_declaration(SemaContext *ctx, const SemaScope *scope,
                                      const Node *node, const Declarator *identifier,
                                      const Declarator *layer, const DeclarationSpecifiers *spec,
                                      DeclarationFailure failure)
{
    if (scope->parent || spec->storage == STORAGE_TYPEDEF)
        return;
    bool function = node->type == ND_FUNCTION || (layer && layer->kind == DECLARATOR_FUNCTION);
    if (failure.type) {
        function = failure.type->kind == SEMA_TYPE_FUNCTION;
    } else if (!function && !layer && spec->type && spec->type->kind == TYPE_SPECIFIER_TYPEDEF_NAME) {
        const SemaBinding *alias = lookup_visible_binding(scope, spec->type->u.typedef_name);
        function = alias && alias->symbol && alias->symbol->type->kind == SEMA_TYPE_FUNCTION;
    }
    unsigned facts = 0;
    if (function) {
        if (node->type == ND_FUNCTION)
            facts |= SEMA_FAILED_FUNCTION_DEFINITION;
        if (!(spec->function_specifiers & FUNCTION_SPEC_INLINE) || (spec->storage & STORAGE_EXTERN))
            facts |= SEMA_FAILED_EXTERNAL_FUNCTION_DECLARATION;
    }
    bool object_candidate = !function && check_failed_object_candidate(node, failure);
    if (!facts && !object_candidate)
        return;
    StringView name = identifier->u.ident.name;
    const SemaBinding *visible = lookup_visible_binding(scope, name);
    SemaSymbolKind kind = function ? SEMA_SYMBOL_FUNCTION : SEMA_SYMBOL_OBJECT;
    SemaLinkage linkage = resolve_linkage(scope, kind,
        (SemaDeclaration){ .node = node, .storage = spec->storage }, visible ? visible->symbol : NULL);
    if (object_candidate) {
        SemaObjectDefinitionFailure reason = failure.type ? SEMA_OBJECT_DEFINITION_CONSTRAINT_FAILED
                                                          : SEMA_OBJECT_DEFINITION_TYPE_UNAVAILABLE;
        attach_failed_object_definition(&ctx->linkage, linkage, name, failure.type, reason);
    } else {
        attach_linkage_recovery(&ctx->linkage, linkage, name, facts);
    }
}

// Preserve lexical visibility and record only the relevant failed file declaration facts.
static void recover_declarator_binding(SemaContext *ctx, SemaScope *scope,
                                        const Node *node, const Declarator *syntax,
                                        const DeclarationSpecifiers *spec, DeclarationFailure failure)
{
    if (scope->visibility_unknown)
        return;
    const Declarator *layer = NULL;
    while (syntax && syntax->kind != DECLARATOR_IDENT) {
        layer = syntax;
        syntax = syntax->inner;
    }
    if (!syntax)
        return;
    StringView name = syntax->u.ident.name;
    recover_file_declaration(ctx, scope, node, syntax, layer, spec, failure);
    if (!lookup_current_binding(scope, name))
        bind_scope_name(scope, name, (SemaBinding){ .failure_status = failure.status });
}

// Select the supported constraint or feature diagnostic for a variable declarator.
static DeclarationIssue check_variable_declaration(const SemaScope *scope,
                                                      SemaDeclaration declaration,
                                                      DeclarationBinding binding)
{
    if (!scope->parent)
        return DECLARATION_VM_SCOPE;
    if (binding.linkage != SEMA_LINKAGE_NONE)
        return DECLARATION_VM_LINKAGE;
    if (declaration.declarator.is_vla && binding.kind == SEMA_SYMBOL_OBJECT) {
        SemaStorageDuration duration = resolve_entity_duration(binding, declaration.storage);
        if (duration == SEMA_DURATION_STATIC || duration == SEMA_DURATION_THREAD)
            return DECLARATION_VLA_STORAGE;
        if (declaration.node->type == ND_INIT_DECL && declaration.node->u.init_decl.init)
            return DECLARATION_VLA_INITIALIZER;
    }
    return declaration.declarator.is_vla ? DECLARATION_UNSUPPORTED_VLA : DECLARATION_UNSUPPORTED_VM;
}

// Construct the composite entity value from compatible declarations without committing the binding.
static SemaEntity new_composite_entity(SemaResult *result, SemaDeclaration declaration,
                                        DeclarationBinding binding)
{
    const SemaType *declared_type = declaration.declarator.type;
    const SemaEntity *previous = binding.entity;
    const Node *definition_node;
    SemaDefinition definition = resolve_entity_definition(previous, declaration, &definition_node);
    const SemaType *type = previous
        ? new_composite_type(result, previous->type, declared_type)
        : declared_type;
    SemaEntity entity = {
        .name = declaration.declarator.identifier->u.ident.name,
        .name_span = &declaration.declarator.identifier->span,
        .linkage = binding.linkage,
        .definition = definition,
        .type = type,
        .definition_node = definition_node,
    };
    if (binding.kind == SEMA_SYMBOL_FUNCTION) {
        entity.u.function.specifiers = declaration.function_specifiers |
            (previous ? previous->u.function.specifiers : 0);
    } else {
        entity.u.object.duration = resolve_entity_duration(binding, declaration.storage);
        entity.u.object.alignment = declaration.alignment ? declaration.alignment
            : previous ? previous->u.object.alignment : 0;
        entity.u.object.definition_alignment = declaration.definition == SEMA_DEFINED
            ? declaration.alignment : previous ? previous->u.object.definition_alignment : 0;
    }
    return entity;
}

// Anchor a binding conflict at the new declaration, with a note where available.
static SemaStatus report_declaration_binding(SemaDeclaration declaration, DeclarationBinding binding,
                                              DeclarationIssue issue)
{
    if (issue == DECLARATION_REDECLARATION)
        return report_redeclaration(declaration.declarator.identifier, binding.previous.symbol);
    const SourceSpan *span = issue == DECLARATION_NONOBJECT_FOR
        ? &declaration.declarator.identifier->span : &declaration.node->span;
    return report_declaration_issue(span, issue);
}

// Form the lexical type from a declaration and any compatible visible predecessor.
static const SemaType *analyze_declaration_type(SemaResult *result, SemaDeclaration declaration,
                                                DeclarationBinding binding)
{
    const SemaType *type = declaration.declarator.type;
    return binding.prior_visible_type ? new_composite_type(result, binding.prior_visible_type, type)
                                      : type;
}

// Prepare and commit an accepted declaration that needs no initializer.
static const SemaSymbol *analyze_checked_declaration(SemaContext *ctx, SemaScope *scope,
                                                     SemaDeclaration declaration,
                                                     DeclarationBinding binding, const SemaType *type)
{
    SemaEntity *entity = NULL;
    if (binding.kind != SEMA_SYMBOL_TYPEDEF) {
        SemaEntity value = new_composite_entity(ctx->result, declaration, binding);
        entity = bind_entity(ctx, binding, binding.entity, value);
        if (!scope->parent && binding.kind == SEMA_SYMBOL_FUNCTION &&
            (!(declaration.function_specifiers & FUNCTION_SPEC_INLINE) ||
             (declaration.storage & STORAGE_EXTERN)))
            attach_external_function_declaration(entity);
    }
    return bind_declaration(ctx->result, scope, declaration, binding.kind, type, entity);
}

// Check a declaration and then delegate publication to the binding helpers.
static SemaStatus analyze_declaration_binding(SemaContext *ctx, SemaScope *scope,
                                       SemaDeclaration declaration,
                                       const SemaSymbol **out)
{
    DeclarationBinding binding = resolve_declaration_binding(ctx, scope, declaration);
    DeclarationIssue issue = check_declaration_binding(scope, declaration, binding);
    if (issue != DECLARATION_VALID)
        return report_declaration_binding(declaration, binding, issue);

    const SemaType *type = analyze_declaration_type(ctx->result, declaration, binding);
    *out = analyze_checked_declaration(ctx, scope, declaration, binding, type);
    return SEMA_OK;
}

// ---------------------------------------------------------------------------
// Types, declarators and parameters
// ---------------------------------------------------------------------------

// Check the qualifiers at their declared level without constructing a type.
static DeclarationIssue check_qualifiers(const SemaType *base, unsigned qualifiers)
{
    if ((qualifiers & TYPE_QUAL_ATOMIC) &&
        (base->kind == SEMA_TYPE_ARRAY || base->kind == SEMA_TYPE_FUNCTION))
        return DECLARATION_INVALID_ATOMIC;
    if ((qualifiers & TYPE_QUAL_RESTRICT) &&
        (base->kind != SEMA_TYPE_POINTER || base->u.pointee->kind == SEMA_TYPE_FUNCTION))
        return DECLARATION_INVALID_RESTRICT;
    if (qualifiers && base->kind == SEMA_TYPE_FUNCTION)
        return DECLARATION_QUALIFIED_FUNCTION;
    return DECLARATION_VALID;
}

// Validate and construct qualifiers while preserving all deeper type levels.
static SemaStatus analyze_qualified_type(SemaContext *ctx, const SemaType *base,
                                         unsigned qualifiers, const SourceSpan *span,
                                         const SemaType **out)
{
    DeclarationIssue issue = check_qualifiers(base, qualifiers);
    if (issue != DECLARATION_VALID)
        return report_declaration_issue(span, issue);
    *out = new_qualified_type(ctx->result, base, base->qualifiers | qualifiers);
    return SEMA_OK;
}

// Interpret a visible name as a typedef, distinguishing an earlier failed binding.
static DeclarationIssue resolve_typedef_name(const SemaBinding *binding, const SemaType **out)
{
    if (binding && binding->failure_status != SEMA_OK)
        return DECLARATION_FAILED_TYPEDEF;
    const SemaSymbol *alias = binding ? binding->symbol : NULL;
    if (!alias || alias->kind != SEMA_SYMBOL_TYPEDEF)
        return DECLARATION_INVALID_TYPEDEF;
    *out = alias->type;
    return DECLARATION_VALID;
}

// Analyze a base type using the active semantic namespace.
static SemaStatus analyze_type_specifier(SemaContext *ctx, const TypeSpecifier *spec,
                                          unsigned qualifiers, bool standalone,
                                          const SemaType **out)
{
    const SemaType *base;
    switch (spec->kind) {
    case TYPE_SPECIFIER_BUILTIN:
        base = type_builtin_specifier(spec->u.builtin);
        break;
    case TYPE_SPECIFIER_TYPEDEF_NAME: {
        const SemaBinding *alias = lookup_visible_binding(ctx->scope, spec->u.typedef_name);
        DeclarationIssue issue = resolve_typedef_name(alias, &base);
        if (issue == DECLARATION_FAILED_TYPEDEF)
            return alias->failure_status;
        if (issue != DECLARATION_VALID)
            return report_declaration_issue(&spec->span, issue);
        break;
    }
    case TYPE_SPECIFIER_STRUCT:
    case TYPE_SPECIFIER_UNION:
    case TYPE_SPECIFIER_ENUM: {
        SemaStatus status = analyze_tag_specifier(ctx, spec, standalone, &base);
        if (status != SEMA_OK)
            return status;
        break;
    }
    case TYPE_SPECIFIER_ATOMIC: {
        SemaStatus status = analyze_type_name(ctx, spec->u.atomic_type, &base);
        if (status != SEMA_OK)
            return status;
        if (base->qualifiers || base->kind == SEMA_TYPE_ARRAY || base->kind == SEMA_TYPE_FUNCTION)
            return report_declaration_issue(&spec->span, DECLARATION_INVALID_ATOMIC);
        qualifiers |= TYPE_QUAL_ATOMIC;
        break;
    }
    default:
        return mark_unsupported(ctx, true);
    }
    return analyze_qualified_type(ctx, base, qualifiers, &spec->span, out);
}

// Analyze an explicit alignment using the same target facts as _Alignof.
static SemaStatus analyze_alignment_specifier(SemaContext *ctx, const AlignmentSpecifier *spec,
                                               unsigned *out)
{
    if (spec->kind == ALIGNMENT_TYPE) {
        const SemaType *type;
        SemaStatus status = analyze_type_name(ctx, spec->u.type_name, &type);
        if (status != SEMA_OK)
            return status;
        if (!type_is_complete_object(type))
            return report_declaration_issue(&spec->span, DECLARATION_ALIGNMENT_TYPE);
        *out = type_alignment(type);
        return SEMA_OK;
    }
    const Node *expression = spec->u.expression;
    SemaStatus status = analyze_expression(ctx, expression);
    if (status != SEMA_OK)
        return status;
    SemaIntegerConstant value;
    if (eval_integer_constant(ctx->result, expression, &value) != SEMA_INTEGER_CONSTANT_OK ||
        !check_integer_constant_range(value, 0, UINT64_MAX) || !check_type_alignment(value.bits))
        return report_declaration_issue(&spec->span, DECLARATION_ALIGNMENT_VALUE);
    *out = (unsigned)value.bits;
    return SEMA_OK;
}

// Analyze the base and alignment operands in source order so their tags and
// enumerators become visible at the same points as in the parser.
static SemaStatus analyze_declaration_specifiers(SemaContext *ctx,
                                                 const DeclarationSpecifiers *specifiers,
                                                 bool standalone, const SemaType **out,
                                                 unsigned *alignment)
{
    if (ctx->scope->visibility_unknown)
        return SEMA_UNSUPPORTED;
    const TypeSpecifier *type = specifiers->type;
    const AlignmentSpecifier *next = specifiers->alignments;
    SemaStatus status = SEMA_OK;
    *alignment = 0;
    while ((type || next) && !ctx->scope->visibility_unknown) {
        if (type && (!next ||
            string_view_begin(type->span.text) < string_view_begin(next->span.text))) {
            SemaStatus current = analyze_type_specifier(ctx, type, specifiers->qualifiers,
                                                        standalone, out);
            status = merge_sema_status(status, current);
            type = NULL;
        } else {
            unsigned requested;
            SemaStatus current = analyze_alignment_specifier(ctx, next, &requested);
            status = merge_sema_status(status, current);
            if (current == SEMA_OK && requested > *alignment)
                *alignment = requested;
            next = next->next;
        }
    }
    return status;
}

// Resolve and adjust one parameter, binding a name in the active prototype
// or definition scope. A lone anonymous void parameter is handled by its list.
static SemaStatus analyze_parameter(SemaContext *ctx, const Node *param,
                                    bool definition, bool lone,
                                    const SemaType **out)
{
    const DeclarationSpecifiers *spec = param->u.parameter.specifiers;
    const SemaType *base;
    unsigned alignment;
    SemaStatus status = analyze_declaration_specifiers(ctx, spec, false, &base, &alignment);
    if (status != SEMA_OK) {
        if (!ctx->scope->visibility_unknown)
            status = merge_sema_status(status,
                analyze_declarator(ctx, param->u.parameter.declarator, NULL, false, NULL));
        return status;
    }

    SemaDeclarator declarator;
    status = analyze_declarator(ctx, param->u.parameter.declarator, base, false, &declarator);
    if (status != SEMA_OK)
        return status;
    if (declarator.variable_bound)
        return report_declaration_issue(declarator.variable_bound, declarator.is_vla
            ? DECLARATION_UNSUPPORTED_VLA : DECLARATION_UNSUPPORTED_VM);
    const SemaType *type = declarator.type;
    if (type->kind == SEMA_TYPE_VOID) {
        if (!declarator.identifier && lone && !type->qualifiers) {
            *out = NULL;
            return SEMA_OK;
        }
        if (!declarator.identifier || definition)
            return report_declaration_issue(&param->span, DECLARATION_INVALID_VOID_PARAMETER);
    }
    if (type->kind == SEMA_TYPE_FUNCTION)
        declarator.type = new_pointer_type(ctx->result, type, 0);
    if (type->kind == SEMA_TYPE_ARRAY)
        declarator.type = new_pointer_type(ctx->result, type->u.array.element,
                                            declarator.array_qualifiers);
    if (declarator.type != type)
        declarator.declared_type = type;
    if (definition && !type_is_complete_object(declarator.type))
        return report_declaration_issue(&param->span, DECLARATION_INCOMPLETE_OBJECT);
    if (!declarator.identifier && definition)
        return report_declaration_issue(&param->span, DECLARATION_UNNAMED_PARAMETER);

    if (declarator.identifier) {
        const SemaSymbol *symbol;
        status = analyze_declaration_binding(ctx, ctx->scope, (SemaDeclaration){
            .node = param, .declarator = declarator, .storage = spec->storage,
            .definition = definition ? SEMA_DEFINED : SEMA_DECLARED,
            .alignment = alignment, .has_alignment = spec->alignments != NULL,
            .function_specifiers = spec->function_specifiers,
        }, &symbol);
        if (status != SEMA_OK)
            return status;
    }
    *out = new_qualified_type(ctx->result, declarator.type,
                              declarator.type->qualifiers & TYPE_QUAL_ATOMIC);
    return SEMA_OK;
}

// Analyze parameters in source order and retain their adjusted signature types.
static SemaStatus analyze_prototype(SemaContext *ctx, const Declarator *layer,
                                    bool definition, SemaParameters *out)
{
    const Node *first = layer->u.function.clause.prototype.parameters;
    size_t count = 0;
    for (const Node *param = first; param; param = param->next)
        ++count;
    const SemaType **types = count ? xcalloc(count, sizeof(*types)) : NULL;
    size_t i = 0;
    bool lone = count == 1 && !layer->u.function.clause.prototype.variadic;
    SemaStatus status = SEMA_OK;
    for (const Node *param = first; param && !ctx->scope->visibility_unknown; param = param->next, ++i) {
        SemaStatus current = analyze_parameter(ctx, param, definition, lone, &types[i]);
        status = merge_sema_status(status, current);
        if (current != SEMA_OK)
            recover_declarator_binding(ctx, ctx->scope, param, param->u.parameter.declarator,
                                        param->u.parameter.specifiers,
                                        (DeclarationFailure){ .status = current });
    }
    if (status != SEMA_OK) {
        free(types);
        return status;
    }
    if (count == 1 && !types[0]) {
        free(types);
        types = NULL;
        count = 0;
    }
    *out = (SemaParameters){ .types = types, .count = count };
    return SEMA_OK;
}

// Analyze a clause in its own prototype scope or in the future function body scope.
static SemaStatus analyze_parameter_clause(SemaContext *ctx, const Declarator *layer,
                                            bool definition, SemaParameters *out)
{
    if (layer->u.function.form != FUNCTION_PARAMS_PROTOTYPE) {
        if (layer->u.function.clause.old_style.identifiers) {
            if (definition)
                mark_scope_visibility_unknown(ctx->scope);
            return report_declaration_issue(&layer->suffix_span, DECLARATION_UNSUPPORTED_OLD_STYLE);
        }
        return SEMA_OK;
    }
    if (definition)
        return analyze_prototype(ctx, layer, true, out);

    SemaScope prototype;
    push_scope(ctx, &prototype);
    SemaStatus status = analyze_prototype(ctx, layer, false, out);
    pop_scope(ctx);
    return status;
}

// Classify a checked clause for function type construction and compatibility.
static SemaFunctionForm resolve_function_form(const Declarator *layer, bool definition)
{
    if (layer->u.function.form == FUNCTION_PARAMS_PROTOTYPE)
        return layer->u.function.clause.prototype.variadic
            ? SEMA_FUNCTION_VARIADIC : SEMA_FUNCTION_PROTOTYPE;
    return definition ? SEMA_FUNCTION_EMPTY_DEFINITION : SEMA_FUNCTION_UNPROTOTYPED;
}

// Analyze the size expression at its lexical position and classify the bound.
static SemaStatus analyze_array_bound(SemaContext *ctx, const Declarator *layer,
                                      SemaArrayBound *out)
{
    if (layer->u.array.form == ARRAY_DECLARATOR_STAR) {
        *out = (SemaArrayBound){ .kind = ARRAY_BOUND_STAR };
        return SEMA_OK;
    }
    const Node *size = layer->u.array.size;
    if (!size) {
        *out = (SemaArrayBound){ .kind = ARRAY_BOUND_OMITTED };
        return SEMA_OK;
    }
    SemaStatus status = analyze_value_expression(ctx, size, size);
    if (status != SEMA_OK)
        return status;
    const SemaExprInfo *info = sema_lookup_expression(ctx->result, size);
    if (!type_is_integer(sema_converted_properties(info).type))
        return report_declaration_issue(&size->span, DECLARATION_ARRAY_BOUND_TYPE);

    SemaIntegerConstant constant;
    SemaIntegerConstantStatus evaluation = eval_integer_constant(ctx->result, size, &constant);
    if (evaluation == SEMA_INTEGER_CONSTANT_INVALID_OPERANDS) {
        *out = (SemaArrayBound){ .kind = ARRAY_BOUND_RUNTIME };
        return SEMA_OK;
    }
    if (evaluation != SEMA_INTEGER_CONSTANT_OK)
        return report_declaration_issue(&size->span, DECLARATION_ARRAY_BOUND_CONSTANT);
    if (!check_integer_constant_range(constant, 1, UINT64_MAX))
        return report_declaration_issue(&size->span, DECLARATION_ARRAY_BOUND_VALUE);
    *out = (SemaArrayBound){ .kind = ARRAY_BOUND_FIXED, .count = constant.bits };
    return SEMA_OK;
}

// Analyze suffixes in source order and retain each operand status for type construction.
static SemaStatus analyze_declarator_operands(SemaContext *ctx, SemaDeclaratorLayer *layers,
                                              size_t count, bool definition)
{
    SemaScope *active = ctx->scope;
    SemaStatus status = SEMA_OK;
    for (size_t i = count; i > 0; --i) {
        SemaDeclaratorLayer *layer = &layers[i - 1];
        bool defining_clause = definition && i == count;
        SemaScope *scope = definition && !defining_clause ? active->parent : active;
        if (scope->visibility_unknown) {
            layer->operand_status = SEMA_UNSUPPORTED;
            status = merge_sema_status(status, layer->operand_status);
            continue;
        }
        update_current_scope(ctx, scope);
        SemaStatus current = SEMA_OK;
        switch (layer->syntax->kind) {
        case DECLARATOR_ARRAY:
            current = analyze_array_bound(ctx, layer->syntax, &layer->bound);
            break;
        case DECLARATOR_FUNCTION:
            current = analyze_parameter_clause(ctx, layer->syntax, defining_clause,
                                               &layer->parameters);
            if (current == SEMA_OK)
                layer->function_form = resolve_function_form(layer->syntax, defining_clause);
            break;
        default:
            break;
        }
        update_current_scope(ctx, active);
        layer->operand_status = current;
        status = merge_sema_status(status, current);
    }
    return status;
}

// Check the array element and its target footprint before type construction.
static DeclarationIssue check_array_layer(const SemaType *element, uint64_t count)
{
    if (!type_is_complete_object(element) || type_has_flexible_array(element))
        return DECLARATION_INVALID_ARRAY_ELEMENT;
    return check_array_size(element, count) ? DECLARATION_VALID : DECLARATION_ARRAY_SIZE;
}

// Check the declared return type independently of the parameter clause.
static DeclarationIssue check_function_return_type(const SemaType *type)
{
    if (type->kind == SEMA_TYPE_ARRAY)
        return DECLARATION_FUNCTION_RETURNING_ARRAY;
    if (type->kind == SEMA_TYPE_FUNCTION)
        return DECLARATION_FUNCTION_RETURNING_FUNCTION;
    return DECLARATION_VALID;
}

// Construct one derived type from the checked return type and suffix facts.
static SemaStatus analyze_declarator_layer(SemaContext *ctx, SemaDeclaratorLayer *layer,
                                           const SemaType **base)
{
    const Declarator *syntax = layer->syntax;
    switch (syntax->kind) {
    case DECLARATOR_POINTER:
        *base = new_pointer_type(ctx->result, *base, 0);
        return analyze_qualified_type(ctx, *base, syntax->u.pointer.qualifiers, &syntax->span, base);
    case DECLARATOR_ARRAY: {
        DeclarationIssue issue = check_array_layer(*base, layer->bound.count);
        if (issue != DECLARATION_VALID)
            return report_declaration_issue(&syntax->suffix_span, issue);
        *base = new_array_type(ctx->result, *base, layer->bound.count);
        return SEMA_OK;
    }
    case DECLARATOR_FUNCTION:
        *base = new_function_type(ctx->result, *base, layer->parameters.types,
            layer->parameters.count, layer->function_form);
        layer->parameters.types = NULL;
        return SEMA_OK;
    default:
        unreachable();
    }
}

// Derive the type or variable-type facts, retaining a valid return type when parameters fail.
static SemaStatus analyze_declarator_type(SemaContext *ctx, SemaDeclaratorLayer *layers,
                                           size_t count, const SemaType *base, bool definition,
                                           SemaDeclarator *out)
{
    for (size_t i = 0; i < count; ++i) {
        SemaDeclaratorLayer *layer = &layers[i];
        const Declarator *syntax = layer->syntax;
        if (!out->variable_bound && syntax->kind == DECLARATOR_FUNCTION) {
            DeclarationIssue issue = check_function_return_type(base);
            if (issue != DECLARATION_VALID)
                return report_declaration_issue(&syntax->suffix_span, issue);
            if (definition && i + 1 == count)
                out->return_type = base;
        }
        if (layer->operand_status != SEMA_OK)
            return layer->operand_status;
        bool array = syntax->kind == DECLARATOR_ARRAY;
        bool variable = array && (layer->bound.kind == ARRAY_BOUND_RUNTIME ||
                                  layer->bound.kind == ARRAY_BOUND_STAR);
        if (variable && !out->variable_bound) {
            DeclarationIssue issue = check_array_layer(base, 0);
            if (issue != DECLARATION_VALID)
                return report_declaration_issue(&syntax->suffix_span, issue);
            out->variable_bound = syntax->u.array.size ? &syntax->u.array.size->span : &syntax->suffix_span;
        }
        if (out->variable_bound) {
            out->is_vla = variable || (out->is_vla && array && layer->bound.kind != ARRAY_BOUND_OMITTED);
        } else {
            SemaStatus status = analyze_declarator_layer(ctx, layer, &base);
            if (status != SEMA_OK)
                return status;
        }
    }
    if (out->variable_bound) {
        out->is_function = layers[count - 1].syntax->kind == DECLARATOR_FUNCTION;
        return SEMA_OK;
    }
    out->type = base;
    if (base->kind == SEMA_TYPE_ARRAY && count && layers[count - 1].syntax->kind == DECLARATOR_ARRAY) {
        const SemaDeclaratorLayer *array = &layers[count - 1];
        out->array_qualifiers = array->syntax->u.array.qualifiers;
        if (array->syntax->u.array.form == ARRAY_DECLARATOR_STATIC)
            out->minimum_array_count = array->bound.count;
    }
    return SEMA_OK;
}

// Release scratch layers and parameter vectors that were not transferred.
static void free_declarator_layers(SemaDeclaratorLayer *layers, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        free(layers[i].parameters.types);
    free(layers);
}

// Analyze suffix operands in lexical order. A missing base and output request
// operand checks only. Otherwise apply the checked layers from root to leaf.
// Prototype recursion descends only through proper parameter declarator children.
static SemaStatus analyze_declarator(SemaContext *ctx, const Declarator *declarator,
                                    const SemaType *base, bool definition,
                                    SemaDeclarator *out)
{
    size_t count = 0;
    const Declarator *identifier = declarator;
    while (identifier && identifier->kind != DECLARATOR_IDENT) {
        ++count;
        identifier = identifier->inner;
    }
    SemaDeclaratorLayer *layers = count ? xcalloc(count, sizeof(*layers)) : NULL;
    const Declarator *syntax = declarator;
    for (size_t i = 0; i < count; ++i, syntax = syntax->inner)
        layers[i].syntax = syntax;

    SemaStatus status = analyze_declarator_operands(ctx, layers, count, definition);
    if (base) {
        *out = (SemaDeclarator){ .identifier = identifier };
        status = merge_sema_status(status,
            analyze_declarator_type(ctx, layers, count, base, definition, out));
    }
    free_declarator_layers(layers, count);
    return status;
}

// ---------------------------------------------------------------------------
// Tag definitions, members and enumerators
// ---------------------------------------------------------------------------

// Check duplicate promoted names. Recursion ends at non-anonymous members.
static bool check_member_name_conflict(const SemaMember *members, SemaMember member)
{
    if (member.kind != SEMA_MEMBER_ANONYMOUS)
        return string_view_length(member.name) && check_member_exists(members, member.name);
    for (const SemaMember *nested = member.type->u.tag->u.aggregate.members;
         nested; nested = nested->next)
        if (check_member_name_conflict(members, *nested))
            return true;
    return false;
}

// Check the member type before computing any target layout.
static DeclarationIssue check_member_type(const SemaAggregateDefinition *aggregate,
                                           const SemaType *type, bool is_last, bool bit_field)
{
    if (bit_field && ((type->qualifiers & TYPE_QUAL_ATOMIC) ||
        (type->kind != SEMA_TYPE_INT && type->kind != SEMA_TYPE_UINT && type->kind != SEMA_TYPE_BOOL)))
        return DECLARATION_BIT_FIELD_TYPE;
    if (type_is_complete_object(type))
        return aggregate->kind == SEMA_TYPE_STRUCT && type_has_flexible_array(type)
            ? DECLARATION_MEMBER_TYPE : DECLARATION_VALID;
    if (type->kind == SEMA_TYPE_ARRAY && !bit_field && is_last &&
        aggregate->kind == SEMA_TYPE_STRUCT &&
        ((aggregate->flags & SEMA_AGGREGATE_HAS_NAMED_MEMBER) || !aggregate->layout_known))
        return DECLARATION_VALID;
    return DECLARATION_MEMBER_TYPE;
}

// Check independent width constraints, applying the type's width limit only when known.
static SemaStatus analyze_bit_field_width(SemaContext *ctx, const Node *width,
                                          const SemaType *type, bool has_name, uint8_t *out)
{
    SemaStatus status = analyze_expression(ctx, width);
    if (status != SEMA_OK)
        return status;
    SemaIntegerConstant constant;
    if (eval_integer_constant(ctx->result, width, &constant) != SEMA_INTEGER_CONSTANT_OK ||
        !check_integer_constant_range(constant, has_name, UINT64_MAX))
        return report_declaration_issue(&width->span, DECLARATION_BIT_FIELD_WIDTH);
    if (type && !check_integer_constant_range(constant, 0, type_integer_traits(type).width))
        return report_declaration_issue(&width->span, DECLARATION_BIT_FIELD_WIDTH_LIMIT);
    if (out)
        *out = (uint8_t)constant.bits;
    return SEMA_OK;
}

// Commit a checked member to the partially constructed aggregate definition.
static void attach_aggregate_member(SemaResult *result, SemaAggregateDefinition *aggregate,
                                    SemaMember member, bool has_name, bool has_flexible_array)
{
    attach_tag_member(result, aggregate->last, member, &aggregate->last);
    if (!aggregate->first)
        aggregate->first = aggregate->last;
    size_t path_depth = 1;
    if (member.kind == SEMA_MEMBER_ANONYMOUS)
        path_depth += member.type->u.tag->u.aggregate.member_path_depth;
    if (path_depth > aggregate->member_path_depth)
        aggregate->member_path_depth = path_depth;
    if (has_flexible_array)
        aggregate->flags |= SEMA_AGGREGATE_CONTAINS_FLEXIBLE_ARRAY;
    if (has_name)
        aggregate->flags |= SEMA_AGGREGATE_HAS_NAMED_MEMBER;
}

// Resolve one member and delegate constraints, layout and publication.
static SemaStatus analyze_aggregate_member(SemaContext *ctx, SemaAggregateDefinition *aggregate,
                                            const Node *node, const Node *decl,
                                            const SemaType *base, unsigned alignment)
{
    const Node *width = decl ? decl->u.struct_declarator.width : NULL;
    const Node *source = decl ? decl : node;
    SemaDeclarator declarator = {0};
    SemaStatus status = SEMA_OK;
    if (decl) {
        status = analyze_declarator(ctx, decl->u.struct_declarator.declarator,
                                    base, false, base ? &declarator : NULL);
    } else {
        declarator.type = base;
    }
    if (status == SEMA_OK && declarator.variable_bound)
        status = report_declaration_issue(declarator.variable_bound, DECLARATION_VM_MEMBER);
    if (status != SEMA_OK || !base) {
        if (width && !ctx->scope->visibility_unknown)
            status = merge_sema_status(status, analyze_bit_field_width(ctx, width, NULL,
                                               decl->u.struct_declarator.declarator != NULL, NULL));
        return status;
    }
    SemaMember member = {
        .declaration = source,
        .type = declarator.type,
        .name = declarator.identifier ? declarator.identifier->u.ident.name : (StringView){0},
        .kind = width ? SEMA_MEMBER_BIT_FIELD
                      : decl ? SEMA_MEMBER_ORDINARY : SEMA_MEMBER_ANONYMOUS,
    };
    DeclarationIssue issue = check_member_type(aggregate, member.type,
                                                !node->next && (!decl || !decl->next),
                                                width != NULL);
    if (issue == DECLARATION_VALID && width && node->u.struct_decl.specifiers->alignments)
        issue = DECLARATION_ALIGNAS_CONTEXT;
    if (issue == DECLARATION_VALID && alignment && alignment < type_alignment(member.type))
        issue = DECLARATION_WEAK_ALIGNMENT;
    if (issue != DECLARATION_VALID) {
        status = report_declaration_issue(&source->span, issue);
        if (width) {
            const SemaType *type = issue == DECLARATION_BIT_FIELD_TYPE ? NULL : member.type;
            status = merge_sema_status(status, analyze_bit_field_width(ctx, width, type,
                                               declarator.identifier != NULL, NULL));
        }
        return status;
    }
    if (width) {
        status = analyze_bit_field_width(ctx, width, member.type,
            string_view_length(member.name) != 0, &member.u.bit_field.width);
        if (status != SEMA_OK)
            return status;
        member.u.bit_field.offset = 0;
    } else {
        member.u.alignment = alignment;
    }
    if (check_member_name_conflict(aggregate->first, member))
        return report_declaration_issue(&source->span, DECLARATION_DUPLICATE_MEMBER);
    if (aggregate->layout_known && !type_layout_member(&aggregate->layout, &member, aggregate->kind))
        return report_declaration_issue(&source->span, DECLARATION_AGGREGATE_SIZE);
    bool has_name = string_view_length(member.name) ||
        (member.kind == SEMA_MEMBER_ANONYMOUS &&
         (member.type->u.tag->u.aggregate.flags & SEMA_AGGREGATE_HAS_NAMED_MEMBER));
    bool flexible = !type_is_complete_object(member.type) || type_has_flexible_array(member.type);
    attach_aggregate_member(ctx->result, aggregate, member, has_name, flexible);
    return SEMA_OK;
}

// Analyze one field declaration, sharing its specifiers across declarators.
static SemaStatus analyze_aggregate_members(SemaContext *ctx, SemaAggregateDefinition *aggregate,
                                             const Node *member)
{
    if (member->type == ND_STATIC_ASSERT)
        return analyze_static_assertion(ctx, member);
    const SemaType *base;
    unsigned alignment;
    SemaStatus status = analyze_declaration_specifiers(ctx, member->u.struct_decl.specifiers,
                                                       false, &base, &alignment);
    if (status != SEMA_OK) {
        aggregate->layout_known = false;
        base = NULL;
    }
    const Node *decl = member->u.struct_decl.declarators;
    if (!decl)
        return status == SEMA_OK ? analyze_aggregate_member(ctx, aggregate, member, NULL, base, alignment)
                                 : status;
    for (; decl && !ctx->scope->visibility_unknown; decl = decl->next) {
        SemaStatus current = analyze_aggregate_member(ctx, aggregate, member, decl, base, alignment);
        status = merge_sema_status(status, current);
        if (current != SEMA_OK)
            aggregate->layout_known = false;
    }
    return status;
}

// Analyze members in the surrounding lexical scope and complete their tag.
static SemaStatus analyze_aggregate_definition(SemaContext *ctx, const TypeSpecifier *spec,
                                                SemaTag *tag)
{
    SemaAggregateDefinition aggregate = { .kind = tag->kind, .layout.alignment = 1, .layout_known = true };
    SemaStatus status = SEMA_OK;
    for (const Node *member = spec->u.aggregate.members;
         member && !ctx->scope->visibility_unknown; member = member->next) {
        SemaStatus current = analyze_aggregate_members(ctx, &aggregate, member);
        status = merge_sema_status(status, current);
        if (current != SEMA_OK && member->type != ND_STATIC_ASSERT)
            aggregate.layout_known = false;
    }
    if (status != SEMA_OK)
        return status;
    uint64_t size;
    if (!type_layout_aggregate(aggregate.layout, &size))
        return report_declaration_issue(&spec->span, DECLARATION_AGGREGATE_SIZE);
    attach_tag_aggregate(tag, (SemaAggregateInfo){
        .members = aggregate.first,
        .member_path_depth = aggregate.member_path_depth,
        .size = size, .alignment = aggregate.layout.alignment, .flags = aggregate.flags,
    });
    return SEMA_OK;
}

// Analyze an explicit enumerator value before its name can hide an outer one.
static SemaStatus analyze_enumerator_value(SemaContext *ctx, const Node *node, int64_t *out)
{
    const Node *expression = node->u.enumerator.value;
    SemaStatus status = analyze_expression(ctx, expression);
    if (status != SEMA_OK)
        return status;
    SemaIntegerConstant constant;
    if (eval_integer_constant(ctx->result, expression, &constant) != SEMA_INTEGER_CONSTANT_OK ||
        !check_integer_constant_range(constant, INT32_MIN, INT32_MAX))
        return report_declaration_issue(&expression->span, DECLARATION_INVALID_ENUMERATOR);
    uint64_t bits = eval_integer_conversion(constant, type_builtin(SEMA_TYPE_INT));
    *out = bits > INT32_MAX ? (int64_t)bits - (INT64_C(1) << 32) : (int64_t)bits;
    return SEMA_OK;
}

// Publish a checked int enumerator under its ordinary name.
static void bind_enumerator(SemaContext *ctx, const Node *node, int64_t value)
{
    const SemaType *type = type_builtin(SEMA_TYPE_INT);
    const SemaSymbol *symbol = attach_symbol(ctx->result, (SemaSymbol){
        .declaration = node, .name_span = &node->span,
        .kind = SEMA_SYMBOL_ENUMERATOR, .type = type, .declared_type = type,
        .u.enumerator_value = value,
    });
    bind_scope_name(ctx->scope, node->u.enumerator.name, (SemaBinding){ .symbol = symbol });
}

// Analyze a value in the prior scope, then commit this enumerator or its error binding.
static SemaStatus analyze_enumerator(SemaContext *ctx, const Node *node,
                                     SemaStatus predecessor, int64_t *value)
{
    SemaStatus status = predecessor;
    if (node->u.enumerator.value)
        status = analyze_enumerator_value(ctx, node, value);
    else if (status == SEMA_OK && *value > INT32_MAX)
        status = report_declaration_issue(&node->span, DECLARATION_INVALID_ENUMERATOR);
    if (ctx->scope->visibility_unknown)
        return status;
    if (lookup_current_binding(ctx->scope, node->u.enumerator.name)) {
        SemaStatus conflict = report_declaration_issue(&node->span, DECLARATION_DUPLICATE_ENUMERATOR);
        return merge_sema_status(status, conflict);
    }
    if (status == SEMA_OK)
        bind_enumerator(ctx, node, *value);
    else
        bind_scope_name(ctx->scope, node->u.enumerator.name, (SemaBinding){ .failure_status = status });
    return status;
}

// Analyze the enumerators in declaration order and choose the target's integer type.
static SemaStatus analyze_enum_definition(SemaContext *ctx, const TypeSpecifier *spec,
                                           SemaTag *tag)
{
    int64_t value = 0;
    bool has_negative = false;
    SemaStatus status = SEMA_OK;
    SemaStatus predecessor = SEMA_OK;
    for (const Node *node = spec->u.enumeration.enumerators; node; node = node->next) {
        predecessor = analyze_enumerator(ctx, node, predecessor, &value);
        status = merge_sema_status(status, predecessor);
        if (ctx->scope->visibility_unknown)
            return status;
        if (predecessor == SEMA_OK) {
            has_negative |= value < 0;
            ++value;
        }
    }
    if (status != SEMA_OK)
        return status;
    const SemaType *integer = type_builtin(has_negative ? SEMA_TYPE_INT : SEMA_TYPE_UINT);
    attach_tag_integer_type(tag, integer);
    return SEMA_OK;
}

// Check tag kind, completion and uniqueness without modifying the namespace.
static DeclarationIssue check_tag_specifier(const SemaTag *tag, SemaTypeKind kind, bool has_body)
{
    if (tag && tag->kind != kind)
        return DECLARATION_TAG_KIND;
    if (tag && has_body && tag->definition)
        return DECLARATION_DUPLICATE_TAG;
    if (kind == SEMA_TYPE_ENUM && !has_body && (!tag || !tag->is_complete))
        return DECLARATION_ENUM_INCOMPLETE;
    return DECLARATION_VALID;
}

// Query the current scope for a definition or forward declaration.
static SemaTag *resolve_tag(SemaContext *ctx, StringView name, bool current_scope)
{
    if (!string_view_length(name))
        return NULL;
    return current_scope ? lookup_current_tag(ctx->scope, name)
                         : lookup_visible_tag(ctx->scope, name);
}

// Select the semantic kind for the shared tag namespace.
static SemaTypeKind resolve_tag_kind(const TypeSpecifier *spec)
{
    switch (spec->kind) {
    case TYPE_SPECIFIER_STRUCT: return SEMA_TYPE_STRUCT;
    case TYPE_SPECIFIER_UNION: return SEMA_TYPE_UNION;
    case TYPE_SPECIFIER_ENUM: return SEMA_TYPE_ENUM;
    default: unreachable();
    }
}

// Bind tag identity before analyzing content, completing it on success.
static SemaStatus analyze_tag_specifier(SemaContext *ctx, const TypeSpecifier *spec,
                                        bool standalone, const SemaType **out)
{
    SemaTypeKind kind = resolve_tag_kind(spec);
    bool is_enum = kind == SEMA_TYPE_ENUM;
    StringView name = is_enum ? spec->u.enumeration.tag : spec->u.aggregate.tag;
    bool body = is_enum ? spec->u.enumeration.has_body : spec->u.aggregate.has_body;
    SemaTag *tag = resolve_tag(ctx, name, body || (standalone && !is_enum));
    DeclarationIssue issue = check_tag_specifier(tag, kind, body);
    if (issue != DECLARATION_VALID)
        return report_declaration_issue(&spec->span, issue);
    if (!tag) {
        tag = attach_tag(ctx->result, kind, &spec->span);
        if (string_view_length(name))
            bind_tag(ctx->scope, name, tag);
    }
    if (body) {
        attach_tag_definition(tag, &spec->span);
        SemaStatus status = is_enum ? analyze_enum_definition(ctx, spec, tag)
                                    : analyze_aggregate_definition(ctx, spec, tag);
        if (status != SEMA_OK)
            return status;
    }
    *out = new_tag_type(ctx->result, tag);
    return SEMA_OK;
}

// ---------------------------------------------------------------------------
// Declaration and function definition analysis
// ---------------------------------------------------------------------------

// Restore an earlier binding or leave an error slot after a rejected initializer.
static void recover_initializer_binding(SemaContext *ctx, DeclarationBinding binding,
                                         StringView name, SemaStatus status)
{
    SemaBinding previous = binding.previous;
    if (!previous.symbol && previous.failure_status == SEMA_OK)
        previous.failure_status = status;
    bind_scope_name(ctx->scope, name, previous);
}

// Expose the lexical type to self-references and commit only a checked definition.
static SemaStatus analyze_declaration_initializer(SemaContext *ctx, SemaDeclaration declaration,
                                                   DeclarationBinding binding,
                                                   const SemaType *type, const SemaSymbol **out)
{
    const Declarator *identifier = declaration.declarator.identifier;
    SemaStorageDuration duration = resolve_entity_duration(binding, declaration.storage);
    SemaEntity *entity = binding.entity;
    if (!entity)
        entity = attach_entity(ctx->result, (SemaEntity){
            .name = identifier->u.ident.name, .name_span = &identifier->span,
            .linkage = binding.linkage, .type = type, .u.object.duration = duration,
        });
    const SemaSymbol *initial = bind_declaration(ctx->result, ctx->scope, declaration,
                                                 binding.kind, type, entity);
    const Node *node = declaration.node;
    SemaInitializer initializer = {0};
    SemaStatus status = analyze_initializer(ctx, node, node->u.init_decl.init, type,
                                           duration != SEMA_DURATION_AUTOMATIC, &initializer);
    StringView name = identifier->u.ident.name;
    if (status != SEMA_OK && !ctx->scope->parent)
        attach_failed_object_definition(&ctx->linkage, binding.linkage, name, type,
                                         SEMA_OBJECT_DEFINITION_INITIALIZER_FAILED);
    if (status == SEMA_OK && initializer.type != type && binding.entity &&
        !type_is_compatible(binding.entity->type, initializer.type))
        status = report_declaration_binding(declaration, binding, DECLARATION_CONFLICTING_TYPES);
    if (status != SEMA_OK) {
        free((void *)initializer.steps);
        recover_initializer_binding(ctx, binding, name, status);
        return status;
    }

    if (initializer.type != type)
        declaration.declarator.type = initializer.type;
    SemaEntity value = new_composite_entity(ctx->result, declaration, binding);
    bind_entity(ctx, binding, entity, value);
    *out = initial;
    if (initializer.type != type) {
        *out = attach_symbol_revision(ctx->result, initial, initializer.type);
        bind_scope_name(ctx->scope, identifier->u.ident.name, (SemaBinding){ .symbol = *out });
    }
    attach_initializer(ctx->result, node, &initializer);
    return SEMA_OK;
}

// Classify definitions before publishing the declaration to its initializer.
static SemaDefinition resolve_object_definition(const SemaScope *scope,
                                                 SemaDeclaration declaration)
{
    if (declaration_kind(declaration) != SEMA_SYMBOL_OBJECT)
        return SEMA_DECLARED;
    if (declaration.node->u.init_decl.init)
        return SEMA_DEFINED;
    if (declaration.storage & STORAGE_EXTERN)
        return SEMA_DECLARED;
    return scope->parent ? SEMA_DEFINED : SEMA_TENTATIVE;
}

// Check the value-bearing element of an array for the inline object constraint.
static bool check_inline_object_modifiable(const SemaType *type)
{
    while (type->kind == SEMA_TYPE_ARRAY)
        type = type->u.array.element;
    return type_is_modifiable_object(type);
}

// Preserve the failed declaration's name and check independent initializer expressions.
static SemaStatus recover_init_declarator(SemaContext *ctx, const Node *node,
                                          const DeclarationSpecifiers *spec, DeclarationFailure failure)
{
    if (ctx->scope->visibility_unknown)
        return failure.status;
    recover_declarator_binding(ctx, ctx->scope, node,
                                node->u.init_decl.declarator, spec, failure);
    SemaStatus status = failure.status;
    if (node->u.init_decl.init)
        status = merge_sema_status(status, analyze_untyped_initializer(ctx, node->u.init_decl.init));
    return status;
}

// Analyze one declarator, bind it at its point of declaration, and examine
// the initializer in that updated scope.
static SemaStatus analyze_init_declarator(SemaContext *ctx, const Node *node,
                                          const SemaType *base, const DeclarationSpecifiers *spec,
                                          unsigned alignment, bool for_init)
{
    StorageClass storage = spec->storage;
    SemaDeclaration declaration = {
        .node = node, .storage = storage, .alignment = alignment,
        .has_alignment = spec->alignments != NULL, .function_specifiers = spec->function_specifiers,
    };
    SemaStatus status = analyze_declarator(ctx, node->u.init_decl.declarator,
                                          base, false, &declaration.declarator);
    if (status != SEMA_OK)
        return recover_init_declarator(ctx, node, spec, (DeclarationFailure){ .status = status });
    declaration.definition = resolve_object_definition(ctx->scope, declaration);
    DeclarationBinding binding = resolve_declaration_binding(ctx, ctx->scope, declaration);
    if (declaration.declarator.variable_bound) {
        DeclarationIssue issue = check_variable_declaration(ctx->scope, declaration, binding);
        status = report_declaration_issue(declaration.declarator.variable_bound, issue);
        return recover_init_declarator(ctx, node, spec, (DeclarationFailure){
            .status = status, .issue = issue,
        });
    }
    DeclarationIssue issue = for_init && binding.kind != SEMA_SYMBOL_OBJECT
        ? DECLARATION_NONOBJECT_FOR : check_declaration_binding(ctx->scope, declaration, binding);
    const Node *init = node->u.init_decl.init;
    if (issue != DECLARATION_VALID) {
        status = report_declaration_binding(declaration, binding, issue);
        return recover_init_declarator(ctx, node, spec, (DeclarationFailure){
            .status = status, .issue = issue, .type = declaration.declarator.type,
        });
    }
    const SemaType *type = analyze_declaration_type(ctx->result, declaration, binding);
    const SemaSymbol *symbol;
    if (init) {
        if (binding.kind != SEMA_SYMBOL_OBJECT || (ctx->scope->parent && (storage & STORAGE_EXTERN))) {
            status = report_declaration_issue(&init->span, DECLARATION_INVALID_INITIALIZER);
            return recover_init_declarator(ctx, node, spec, (DeclarationFailure){
                .status = status, .issue = DECLARATION_INVALID_INITIALIZER,
                .type = declaration.declarator.type,
            });
        }
        status = analyze_declaration_initializer(ctx, declaration, binding, type, &symbol);
        if (status != SEMA_OK)
            return status;
    } else {
        symbol = analyze_checked_declaration(ctx, ctx->scope, declaration, binding, type);
    }
    if (ctx->function && declaration.definition == SEMA_DEFINED &&
        symbol->kind == SEMA_SYMBOL_OBJECT && symbol->entity &&
        (symbol->entity->u.object.duration == SEMA_DURATION_STATIC ||
         symbol->entity->u.object.duration == SEMA_DURATION_THREAD) &&
        check_inline_object_modifiable(symbol->type))
        attach_inline_static(ctx->function, symbol->name_span);
    return SEMA_OK;
}

// Analyze specifiers and declarators in order under the given declaration rules.
static SemaStatus analyze_declaration_with_context(SemaContext *ctx, const Node *node,
                                                   bool for_init)
{
    const DeclarationSpecifiers *spec = node->u.decl.specifiers;
    const SemaType *base = NULL;
    unsigned alignment;
    SemaStatus status = analyze_declaration_specifiers(ctx, spec, !node->u.decl.declarators,
                                                       &base, &alignment);
    SemaStatus specifiers = status;
    for (const Node *decl = node->u.decl.declarators;
         decl && !ctx->scope->visibility_unknown; decl = decl->next) {
        SemaStatus current;
        if (specifiers == SEMA_OK) {
            current = analyze_init_declarator(ctx, decl, base, spec, alignment, for_init);
        } else {
            SemaDeclarator declarator = {0};
            current = analyze_declarator(ctx, decl->u.init_decl.declarator, base, false, &declarator);
            current = recover_init_declarator(ctx, decl, spec, (DeclarationFailure){
                .status = merge_sema_status(specifiers, current), .type = declarator.type,
            });
        }
        status = merge_sema_status(status, current);
    }
    return status;
}

// Report a failed or ill-formed assertion at its controlling expression.
static SemaStatus report_static_assertion(const Node *node, bool constant)
{
    const Node *expression = node->u.static_assertion.expression;
    const Node *message = node->u.static_assertion.message;
    if (constant)
        error_at(&expression->span, "static assertion failed: \"%.*s\"",
                 message->u.string.length, message->u.string.value);
    else
        error_at(&expression->span, "static assertion requires an integer constant expression");
    return SEMA_INVALID;
}

// ---------------------------------------------------------------------------
// Declaration analysis interfaces
// ---------------------------------------------------------------------------

// Resolve a type-name using the same base and abstract declarator rules.
SemaStatus analyze_type_name(SemaContext *ctx, const TypeName *name,
                             const SemaType **out)
{
    const SemaType *type;
    SemaStatus status = analyze_type_specifier(ctx, name->type, name->qualifiers, false, &type);
    if (status != SEMA_OK) {
        if (!ctx->scope->visibility_unknown)
            status = merge_sema_status(status,
                analyze_declarator(ctx, name->declarator, NULL, false, NULL));
        return status;
    }
    SemaDeclarator declarator;
    status = analyze_declarator(ctx, name->declarator, type, false, &declarator);
    if (status != SEMA_OK)
        return status;
    if (declarator.variable_bound)
        return report_declaration_issue(declarator.variable_bound, declarator.is_vla
            ? DECLARATION_UNSUPPORTED_VLA : DECLARATION_UNSUPPORTED_VM);
    *out = declarator.type;
    return SEMA_OK;
}

// Analyze an ordinary declaration without the for-initializer restriction.
SemaStatus analyze_declaration(SemaContext *ctx, const Node *node)
{
    return analyze_declaration_with_context(ctx, node, false);
}

// Apply the object constraint to each resolved for-initializer declarator.
SemaStatus analyze_for_declaration(SemaContext *ctx, const Node *node)
{
    return analyze_declaration_with_context(ctx, node, true);
}

// Check a C11 static assertion wherever a declaration can contain it.
SemaStatus analyze_static_assertion(SemaContext *ctx, const Node *node)
{
    const Node *expression = node->u.static_assertion.expression;
    SemaStatus status = analyze_expression(ctx, expression);
    if (status != SEMA_OK)
        return status;
    SemaIntegerConstant value;
    if (eval_integer_constant(ctx->result, expression, &value) != SEMA_INTEGER_CONSTANT_OK)
        return report_static_assertion(node, false);
    return value.bits ? SEMA_OK : report_static_assertion(node, true);
}

// Analyze definition specifiers in file scope, prior to any parameter bindings.
SemaStatus analyze_function_specifiers(SemaContext *ctx, const Node *node,
                                       const SemaType **out)
{
    const DeclarationSpecifiers *spec = node->u.func.specifiers;
    unsigned alignment;
    SemaStatus status = analyze_declaration_specifiers(ctx, spec, false, out, &alignment);
    if (status == SEMA_OK && spec->alignments)
        status = report_declaration_issue(&spec->alignments->span, DECLARATION_ALIGNAS_CONTEXT);
    return status;
}

// Analyze parameters in the active function scope and bind only a complete signature.
// A valid return type is available to the body independently of signature success.
SemaStatus analyze_function_signature(SemaContext *ctx, SemaScope *file_scope,
                                      const Node *node, const SemaType *base,
                                      SemaStatus specifiers,
                                      const SemaType **return_type, SemaEntity **function)
{
    SemaDeclaration declaration = {
        .node = node, .storage = node->u.func.specifiers->storage, .definition = SEMA_DEFINED,
        .function_specifiers = node->u.func.specifiers->function_specifiers,
    };
    SemaStatus status = analyze_declarator(ctx, node->u.func.declarator, base, true,
                                          &declaration.declarator);
    status = merge_sema_status(specifiers, status);
    if (declaration.declarator.variable_bound && !file_scope->visibility_unknown) {
        DeclarationBinding binding = resolve_declaration_binding(ctx, file_scope, declaration);
        SemaStatus variable = report_declaration_issue(declaration.declarator.variable_bound,
            check_variable_declaration(file_scope, declaration, binding));
        status = merge_sema_status(status, variable);
    }
    const SemaType *type = declaration.declarator.return_type;
    if (type && type->kind != SEMA_TYPE_VOID && !type_is_complete_object(type)) {
        SemaStatus incomplete = report_declaration_issue(&node->span, DECLARATION_INCOMPLETE_RETURN);
        status = merge_sema_status(status, incomplete);
    } else {
        *return_type = type;
    }
    if (status == SEMA_OK) {
        const SemaSymbol *symbol;
        status = analyze_declaration_binding(ctx, file_scope, declaration, &symbol);
        if (status == SEMA_OK)
            *function = (SemaEntity *)symbol->entity;
    }
    if (status != SEMA_OK)
        recover_declarator_binding(ctx, file_scope, node, node->u.func.declarator,
                                    node->u.func.specifiers, (DeclarationFailure){ .status = status });
    return status;
}
