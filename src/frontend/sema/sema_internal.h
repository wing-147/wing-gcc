#ifndef SEMA_INTERNAL_H
#define SEMA_INTERNAL_H

// The longest local chain is a target read, a promotion, and a common type.
#define SEMA_MAX_CONVERSION_STEPS 3

#include "frontend/sema.h"
#include "util/hashmap.h"

typedef enum {
    SEMA_OBJECT_DEFINITION_TYPE_UNAVAILABLE,
    SEMA_OBJECT_DEFINITION_CONSTRAINT_FAILED,
    SEMA_OBJECT_DEFINITION_INITIALIZER_FAILED,
} SemaObjectDefinitionFailure;

typedef enum {
    SEMA_INTEGER_CONSTANT_OK,
    SEMA_INTEGER_CONSTANT_INVALID_OPERANDS,
    SEMA_INTEGER_CONSTANT_EVALUATION_FAILED,
} SemaIntegerConstantStatus;

enum {
    SEMA_FAILED_FUNCTION_DEFINITION = 1u << 0,
    SEMA_FAILED_EXTERNAL_FUNCTION_DECLARATION = 1u << 1,
};

typedef struct SemaScope {
    struct SemaScope *parent;
    HashMap names;
    HashMap tags;
    bool visibility_unknown;
} SemaScope;

typedef struct {
    // A checked symbol is result-owned. A failure status keeps its name visible.
    const SemaSymbol *symbol;
    SemaStatus failure_status;
} SemaBinding;

typedef struct {
    HashMap internal_bindings;
    HashMap external_bindings;
} SemaLinkageIndex;

typedef struct SemaFailedObjectDefinition {
    const SemaType *type;
    SemaObjectDefinitionFailure reason;
    struct SemaFailedObjectDefinition *next;
} SemaFailedObjectDefinition;

typedef struct {
    const SemaMember **members;
    size_t count;
} SemaMemberPath;

typedef struct {
    const SourceSpan *inline_internal_reference;
    const SourceSpan *inline_modifiable_static;
    bool has_external_function_declaration;
} SemaEntityAnalysis;

typedef struct {
    SemaResult *result;
    SemaScope *scope;
    SemaLinkageIndex linkage;
    SemaEntity *function;
    bool unsupported;
} SemaContext;

typedef struct {
    unsigned rank;
    unsigned width;
    bool is_unsigned;
    SemaTypeKind unsigned_kind;
} IntegerTraits;

typedef struct {
    const SemaType *type;
    uint64_t bits;
} SemaIntegerConstant;

typedef struct {
    uint64_t bits;
    unsigned alignment;
} SemaAggregateLayout;

typedef struct {
    const SemaType *source;
    const SemaType *promoted; // NULL when no integer-promotion rule applies
    const SemaType *converted;
} SemaArithmeticOperand;

typedef struct {
    SemaArithmeticOperand left;
    SemaArithmeticOperand right;
    const SemaType *result;
} SemaArithmeticPlan;

typedef struct {
    SemaExprProperties value;
    SemaConversion steps[SEMA_MAX_CONVERSION_STEPS];
    size_t count;
} SemaConversionPlan;

typedef struct {
    SemaConversionPlan left;
    SemaConversionPlan right;
} SemaOperandConversions;

typedef struct {
    bool allow_void_or_null;
    bool left_is_null;
    bool right_is_null;
} SemaPointerConversionContext;

typedef struct {
    SemaConversionPlan target_read;
    SemaConversionPlan rhs;
    SemaConversionPlan writeback;
} SemaUpdateConversions;

// Semantic types
const SemaType *type_builtin(SemaTypeKind kind);
const SemaType *type_builtin_specifier(TypeKind kind);
const SemaType *type_literal(LiteralType literal);
const SemaType *type_size_t(void);
const SemaType *new_pointer_type(SemaResult *result, const SemaType *pointee,
                                 unsigned qualifiers);
const SemaType *new_array_type(SemaResult *result, const SemaType *element, uint64_t count);
const SemaType *new_tag_type(SemaResult *result, const SemaTag *tag);
const SemaType *new_function_type(SemaResult *result, const SemaType *return_type,
                                  const SemaType **parameters, size_t count,
                                  SemaFunctionForm form);
const SemaType *new_qualified_type(SemaResult *result, const SemaType *type,
                                   unsigned qualifiers);
bool type_is_integer(const SemaType *type);
bool type_is_arithmetic(const SemaType *type);
bool type_is_real(const SemaType *type);
bool type_is_scalar(const SemaType *type);
bool type_is_object(const SemaType *type);
bool type_has_prototype(const SemaType *type);
bool type_is_complete_object(const SemaType *type);
bool type_has_flexible_array(const SemaType *type);
IntegerTraits type_integer_traits(const SemaType *type);
const SemaType *type_integer_promotion(const SemaType *type);
const SemaType *type_default_argument_promotion(const SemaType *type);
bool type_is_compatible_unqualified(const SemaType *lhs, const SemaType *rhs);
bool type_is_compatible(const SemaType *lhs, const SemaType *rhs);
bool type_is_identical(const SemaType *lhs, const SemaType *rhs);
const SemaType *new_composite_type(SemaResult *result, const SemaType *lhs,
                                   const SemaType *rhs);
bool type_is_modifiable_object(const SemaType *type);

// Target layout
size_t type_size(const SemaType *type);
unsigned type_alignment(const SemaType *type);
bool check_type_alignment(uint64_t alignment);
bool check_array_size(const SemaType *element, uint64_t count);
bool type_layout_member(SemaAggregateLayout *layout, SemaMember *member, SemaTypeKind kind);
bool type_layout_aggregate(SemaAggregateLayout layout, uint64_t *size);

// Lexical scopes, linkage and member paths
void init_linkage_index(SemaLinkageIndex *index);
void free_linkage_index(SemaLinkageIndex *index);
SemaEntity *lookup_linked_entity(const SemaLinkageIndex *index, SemaLinkage linkage,
                                StringView name);
void bind_linked_entity(SemaLinkageIndex *index, SemaEntity *entity);
unsigned lookup_linkage_recovery(const SemaLinkageIndex *index, SemaLinkage linkage,
                                  StringView name);
void attach_linkage_recovery(SemaLinkageIndex *index, SemaLinkage linkage, StringView name,
                              unsigned facts);
const SemaFailedObjectDefinition *lookup_failed_object_definitions(const SemaLinkageIndex *index,
                                                                     SemaLinkage linkage, StringView name);
void attach_failed_object_definition(SemaLinkageIndex *index, SemaLinkage linkage, StringView name,
                                       const SemaType *type, SemaObjectDefinitionFailure reason);
void push_scope(SemaContext *ctx, SemaScope *scope);
void pop_scope(SemaContext *ctx);
void update_current_scope(SemaContext *ctx, SemaScope *scope);
void mark_scope_visibility_unknown(SemaScope *scope);
const SemaBinding *lookup_current_binding(const SemaScope *scope, StringView name);
const SemaBinding *lookup_visible_binding(const SemaScope *scope, StringView name);
void bind_scope_name(SemaScope *scope, StringView name, SemaBinding binding);
SemaTag *lookup_current_tag(const SemaScope *scope, StringView name);
SemaTag *lookup_visible_tag(const SemaScope *scope, StringView name);
void bind_tag(SemaScope *scope, StringView name, SemaTag *tag);
bool check_member_exists(const SemaMember *members, StringView name);
void free_member_path(SemaMemberPath *path);
SemaMemberPath new_member_path(const SemaAggregateInfo *aggregate, StringView name);

// Result storage
SemaResult *new_sema_result(void);
const SemaType *attach_type(SemaResult *result, SemaType type);
SemaEntity *attach_entity(SemaResult *result, SemaEntity entity);
void update_entity(SemaEntity *entity, SemaEntity value);
void attach_external_function_declaration(SemaEntity *function);
void attach_inline_reference(SemaEntity *function, const SourceSpan *span);
void attach_inline_static(SemaEntity *function, const SourceSpan *span);
void visit_sema_entities(SemaResult *result,
                          void (*visit)(SemaEntity *entity, const SemaEntityAnalysis *analysis, void *context),
                          void *context);
const SemaSymbol *attach_symbol(SemaResult *result, SemaSymbol symbol);
const SemaSymbol *attach_symbol_revision(SemaResult *result, const SemaSymbol *initial,
                                         const SemaType *completed_type);
void attach_initializer(SemaResult *result, const Node *owner, SemaInitializer *initializer);
SemaTag *attach_tag(SemaResult *result, SemaTypeKind kind, const SourceSpan *declaration);
void attach_tag_definition(SemaTag *tag, const SourceSpan *definition);
void attach_tag_aggregate(SemaTag *tag, SemaAggregateInfo aggregate);
void attach_tag_integer_type(SemaTag *tag, const SemaType *integer_type);
void attach_tag_member(SemaResult *result, SemaMember *last, SemaMember member,
                       SemaMember **out);
void attach_expression(SemaResult *result, const Node *node, SemaExprProperties original);
void attach_identifier_expression(SemaResult *result, const Node *node,
                                    SemaExprProperties value, const SemaSymbol *symbol);
void attach_sizeof_expression(SemaResult *result, const Node *node,
                              SemaExprProperties value, size_t size);
void attach_alignof_expression(SemaResult *result, const Node *node,
                               SemaExprProperties value, size_t alignment);
void attach_compound_literal(SemaResult *result, const Node *node,
                              SemaExprProperties value, bool static_duration);
void attach_generic_selection(SemaResult *result, const Node *node,
                               SemaExprProperties value, const Node *selected);
void attach_assignment_expression(SemaResult *result, const Node *node,
                                   SemaExprProperties value, bool atomic_store);
void attach_member_expression(SemaResult *result, const Node *node,
                              SemaExprProperties value, const SemaMember *member, uint64_t offset);
void attach_conditional_expression(SemaResult *result, const Node *node,
                                   SemaExprProperties value, SemaConditionInfo condition);
void attach_update_expression(SemaResult *result, const Node *node,
                               SemaExprProperties value, SemaUpdateInfo update);
void attach_conversion_sequence(SemaResult *result, const Node *node,
                                SemaConversionSequence sequence);
void attach_jump_target(SemaResult *result, const Node *node, const Node *target);
void attach_switch_label(SemaResult *result, const Node *node, SemaSwitchLabelInfo label);
void attach_condition(SemaResult *result, const Node *node, SemaConditionInfo condition);

// Implicit conversion planning
SemaConversionPlan convert_value(SemaResult *result, const Node *use,
                                 SemaExprProperties source);
void convert_integer_promotion(SemaConversionPlan *plan, const Node *use);
void convert_case_value(SemaConversionPlan *plan, const Node *use, const SemaType *target);
bool plan_arithmetic(Operator op, SemaExprProperties left, SemaExprProperties right,
                     SemaArithmeticPlan *out);
void convert_arithmetic_operands(const SemaArithmeticPlan *arithmetic, const Node *use,
                                 SemaOperandConversions *operands);
const SemaType *convert_usual_arithmetic(const Node *use, SemaOperandConversions *operands);
const SemaType *convert_pointer_operands(SemaResult *result, const Node *use,
                                         SemaPointerConversionContext context,
                                         SemaOperandConversions *operands);
void convert_update(const SemaArithmeticPlan *arithmetic, const Node *use,
                    SemaUpdateConversions *update);
bool convert_assignment_value(SemaResult *result, const Node *use,
                              SemaExprProperties source, const SemaType *target,
                              bool null_pointer_constant, SemaConversionPlan *out);
void convert_default_argument(SemaConversionPlan *plan, const Node *use);

// Constant evaluation
SemaIntegerConstantStatus eval_integer_constant(const SemaResult *result, const Node *node,
                                                SemaIntegerConstant *out);
uint64_t eval_integer_conversion(SemaIntegerConstant source, const SemaType *target);
bool check_integer_constant_range(SemaIntegerConstant constant, int64_t min, uint64_t max);
bool eval_is_null_pointer_constant(const SemaResult *result, const Node *node);
bool eval_static_initializer(const SemaResult *result, const Node *node);

// Declarations and type names
SemaStatus analyze_type_name(SemaContext *ctx, const TypeName *name,
                             const SemaType **out);
SemaStatus analyze_declaration(SemaContext *ctx, const Node *node);
SemaStatus analyze_for_declaration(SemaContext *ctx, const Node *node);
SemaStatus analyze_static_assertion(SemaContext *ctx, const Node *node);
SemaStatus analyze_function_specifiers(SemaContext *ctx, const Node *node,
                                       const SemaType **out);
SemaStatus analyze_function_signature(SemaContext *ctx, SemaScope *file_scope,
                                      const Node *node, const SemaType *base,
                                      SemaStatus specifiers,
                                      const SemaType **return_type, SemaEntity **function);

// Initializers
SemaStatus analyze_initializer(SemaContext *ctx, const Node *owner, const Node *initializer,
                                const SemaType *type, bool constant_required, SemaInitializer *out);
SemaStatus analyze_untyped_initializer(SemaContext *ctx, const Node *initializer);

// Expressions
SemaStatus analyze_expression(SemaContext *ctx, const Node *node);
SemaStatus analyze_value_expression(SemaContext *ctx, const Node *node,
                                    const Node *use);
SemaStatus analyze_typed_assignment_value(SemaContext *ctx, const Node *node,
                                          const Node *use, const SemaType *target);
SemaStatus analyze_assignment_value(SemaContext *ctx, const Node *node,
                                    const Node *use, const SemaType *target);

// Function bodies
SemaStatus analyze_function_body(SemaContext *ctx, const Node *body,
                                 const SemaType *return_type, unsigned function_specifiers);

// Stage status
SemaStatus merge_sema_status(SemaStatus first, SemaStatus second);
SemaStatus mark_unsupported(SemaContext *ctx, bool may_declare_names);

#endif // SEMA_INTERNAL_H
