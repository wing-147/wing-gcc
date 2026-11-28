#ifndef SEMA_H
#define SEMA_H

#include "frontend/ast.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SEMA_OK,
    SEMA_INVALID,
    SEMA_UNSUPPORTED,
} SemaStatus;

typedef enum {
    SEMA_TYPE_INT,
    SEMA_TYPE_UINT,
    SEMA_TYPE_LONG,
    SEMA_TYPE_ULONG,
    SEMA_TYPE_LLONG,
    SEMA_TYPE_ULLONG,
    SEMA_TYPE_SCHAR,
    SEMA_TYPE_UCHAR,
    SEMA_TYPE_USHORT,
    SEMA_TYPE_FLOAT,
    SEMA_TYPE_DOUBLE,
    SEMA_TYPE_LDOUBLE,
    SEMA_TYPE_FLOAT_COMPLEX,
    SEMA_TYPE_DOUBLE_COMPLEX,
    SEMA_TYPE_LDOUBLE_COMPLEX,
    SEMA_TYPE_CHAR,
    SEMA_TYPE_SHORT,
    SEMA_TYPE_VOID,
    SEMA_TYPE_BOOL,
    SEMA_TYPE_POINTER,
    SEMA_TYPE_FUNCTION,
    SEMA_TYPE_ARRAY,
    SEMA_TYPE_STRUCT,
    SEMA_TYPE_UNION,
    SEMA_TYPE_ENUM,
} SemaTypeKind;

typedef enum {
    SEMA_VALUE,
    SEMA_LVALUE,
    SEMA_FUNCTION_DESIGNATOR,
} SemaValueCategory;

typedef enum {
    SEMA_CONVERSION_LVALUE_TO_VALUE,
    SEMA_CONVERSION_FUNCTION_TO_POINTER,
    SEMA_CONVERSION_INTEGER_PROMOTION,
    SEMA_CONVERSION_USUAL_ARITHMETIC,
    SEMA_CONVERSION_ASSIGNMENT,
    SEMA_CONVERSION_DEFAULT_ARGUMENT,
    SEMA_CONVERSION_NULL_POINTER,
    SEMA_CONVERSION_POINTER,
    SEMA_CONVERSION_CASE,
    SEMA_CONVERSION_ARRAY_TO_POINTER,
    SEMA_CONVERSION_ATOMIC_LOAD,
} SemaConversionKind;

typedef enum {
    SEMA_SYMBOL_OBJECT,
    SEMA_SYMBOL_FUNCTION,
    SEMA_SYMBOL_TYPEDEF,
    SEMA_SYMBOL_ENUMERATOR,
} SemaSymbolKind;

typedef enum {
    SEMA_LINKAGE_NONE,
    SEMA_LINKAGE_INTERNAL,
    SEMA_LINKAGE_EXTERNAL,
} SemaLinkage;

typedef enum {
    SEMA_DURATION_NONE,
    SEMA_DURATION_AUTOMATIC,
    SEMA_DURATION_STATIC,
    SEMA_DURATION_THREAD,
} SemaStorageDuration;

typedef enum {
    SEMA_DECLARED,
    SEMA_TENTATIVE,
    SEMA_DEFINED,
    SEMA_ZERO_DEFINED,
} SemaDefinition;

typedef enum {
    SEMA_MEMBER_ORDINARY,
    SEMA_MEMBER_BIT_FIELD,
    SEMA_MEMBER_ANONYMOUS,
} SemaMemberKind;

typedef enum {
    SEMA_FUNCTION_UNPROTOTYPED,
    SEMA_FUNCTION_EMPTY_DEFINITION,
    SEMA_FUNCTION_PROTOTYPE,
    SEMA_FUNCTION_VARIADIC,
} SemaFunctionForm;

typedef enum {
    SEMA_INITIALIZER_ZERO,
    SEMA_INITIALIZER_EXPRESSION,
    SEMA_INITIALIZER_STRING,
} SemaInitializerKind;

typedef enum {
    SEMA_EXPR_PLAIN, // This expression has only common facts.
    SEMA_EXPR_IDENTIFIER,
    SEMA_EXPR_SIZEOF,
    SEMA_EXPR_ALIGNOF,
    SEMA_EXPR_COMPOUND_LITERAL,
    SEMA_EXPR_GENERIC_SELECTION,
    SEMA_EXPR_ASSIGNMENT,
    SEMA_EXPR_MEMBER,
    SEMA_EXPR_CONDITIONAL,
    SEMA_EXPR_UPDATE,
} SemaExprKind;

enum {
    SEMA_AGGREGATE_HAS_NAMED_MEMBER = 1u << 0,
    SEMA_AGGREGATE_CONTAINS_FLEXIBLE_ARRAY = 1u << 1,
};

enum {
    SEMA_UPDATE_RETURNS_PREVIOUS_VALUE = 1u << 0, // Postfix ++ and --.
    SEMA_UPDATE_ATOMIC = 1u << 1, // One atomic read-modify-write.
};

typedef struct SemaResult SemaResult;
typedef struct SemaType SemaType;
typedef struct SemaEntity SemaEntity;
typedef struct SemaTag SemaTag;
typedef struct SemaMember SemaMember;

struct SemaMember {
    const Node *declaration;
    StringView name;
    const SemaType *type;
    const SemaMember *next;
    uint64_t offset;
    SemaMemberKind kind;
    union {
        unsigned alignment; // Ordinary or anonymous member. Zero when omitted.
        struct {
            uint8_t offset; // Bit offset within the byte at the member offset.
            uint8_t width;
        } bit_field;
    } u;
};

typedef struct {
    const SemaMember *members;
    size_t member_path_depth; // Maximum route length through anonymous aggregates.
    uint64_t size;
    unsigned alignment;
    unsigned flags; // SEMA_AGGREGATE_* flags
} SemaAggregateInfo;

struct SemaTag {
    const SourceSpan *declaration;
    const SourceSpan *definition;
    SemaTypeKind kind;
    bool is_complete; // The definition span is set before analysis completes.
    union {
        const SemaType *integer_type; // Compatible integer type of a completed enum.
        SemaAggregateInfo aggregate; // Struct or union.
    } u;
};

struct SemaType {
    SemaTypeKind kind;
    unsigned qualifiers; // TYPE_QUAL_* flags
    union {
        const SemaType *pointee;
        const SemaTag *tag;
        struct {
            const SemaType *element;
            uint64_t count; // Zero denotes an unknown bound.
        } array;
        struct {
            const SemaType *return_type;
            // Only prototype forms carry an adjusted parameter vector.
            const SemaType *const *parameters;
            size_t parameter_count;
            SemaFunctionForm form;
        } function;
    } u; // Selected by kind.
};

typedef struct {
    const Node *declaration; // ND_INIT_DECL, ND_PARAMETER or ND_FUNCTION
    const SourceSpan *name_span;
    const SemaType *type;
    const SemaType *declared_type;
    const SemaEntity *entity; // Objects and functions.
    SemaSymbolKind kind;
    StorageClass storage;
    union {
        struct {
            uint64_t minimum_array_count; // A named parameter with a static bound.
            unsigned alignment; // Explicit alignment, or zero when omitted.
        } object;
        unsigned function_specifiers;
        int64_t enumerator_value;
    } u; // Selected by kind. Typedefs have no payload.
} SemaSymbol;

struct SemaEntity {
    StringView name;
    const SourceSpan *name_span;
    const SemaType *type; // Final composite type. Uses read their declaration.
    const Node *definition_node;
    SemaLinkage linkage;
    SemaDefinition definition;
    union {
        struct {
            SemaStorageDuration duration;
            unsigned alignment;
            unsigned definition_alignment;
        } object;
        struct {
            unsigned specifiers; // FUNCTION_SPEC_* flags
            bool is_inline_definition;
        } function;
    } u; // Selected by whether type is a function type.
};

typedef struct {
    const SemaType *type;
    const SemaMember *bit_field;
    SemaValueCategory category;
    bool register_object; // Designates an object or subobject declared with register.
} SemaExprProperties;

typedef struct {
    SemaConversionKind kind;
    SemaExprProperties from;
    SemaExprProperties to;
    const Node *use; // the AST construct requiring the conversion
} SemaConversion;

typedef struct {
    const SemaConversion *steps;
    size_t count;
} SemaConversionSequence;

typedef struct {
    const SemaType *type;
    uint64_t offset;
    SemaInitializerKind kind;
    union {
        struct {
            const Node *operand;
            const SemaMember *bit_field;
        } expression;
        struct {
            const Node *literal;
            uint64_t bytes;
        } string;
    } u; // Selected by kind. Zero steps have no payload.
} SemaInitializerStep;

typedef struct {
    const SemaType *type;
    const SemaInitializerStep *steps; // Override order. Expression evaluation order is unspecified.
    size_t count;
} SemaInitializer;

// An update reads and writes one evaluation of the target designator.
typedef struct {
    const Node *target;
    Operator arithmetic_operator;
    unsigned flags; // SEMA_UPDATE_* flags
    const SemaType *calculation_type;
    const SemaType *implicit_rhs_type; // type of 1 for ++ and --
    SemaConversionSequence target_read;
    SemaConversionSequence implicit_rhs;
    SemaConversionSequence writeback;
} SemaUpdateInfo;

typedef struct {
    const Node *expression; // Test for nonzero, or NULL for an omitted for condition.
} SemaConditionInfo;

typedef struct {
    SemaExprProperties original;
    SemaConversionSequence conversions; // In application order.
    SemaExprKind kind; // Selects the active payload in u.
    union {
        const SemaSymbol *symbol; // SEMA_EXPR_IDENTIFIER
        size_t sizeof_value; // SEMA_EXPR_SIZEOF, target bytes.
        size_t alignof_value; // SEMA_EXPR_ALIGNOF, target bytes.
        bool static_duration; // SEMA_EXPR_COMPOUND_LITERAL
        const Node *selected; // SEMA_EXPR_GENERIC_SELECTION
        bool atomic_store; // SEMA_EXPR_ASSIGNMENT
        struct {
            const SemaMember *field;
            uint64_t offset; // Includes anonymous aggregate path offsets.
        } member; // SEMA_EXPR_MEMBER
        SemaConditionInfo condition; // SEMA_EXPR_CONDITIONAL
        const SemaUpdateInfo *update; // SEMA_EXPR_UPDATE
    } u;
} SemaExprInfo;

typedef struct {
    const Node *switch_statement;
    const SemaType *value_type; // NULL for default.
    uint64_t value_bits; // Target representation in value_type, when present.
} SemaSwitchLabelInfo;

// Read the endpoint of the implicit conversion chain, borrowing its type and member.
static inline SemaExprProperties sema_converted_properties(const SemaExprInfo *info)
{
    return info->conversions.count
        ? info->conversions.steps[info->conversions.count - 1].to : info->original;
}

// Analyze an ND_PROGRAM from successful lexing and parsing. Only SEMA_OK
// transfers a result to *out. Both failure statuses leave *out NULL.
SemaStatus sema_analyze(const Node *root, SemaResult **out);

// Release an owned result (NULL is accepted) before releasing its AST or source.
void sema_free(SemaResult *result);

// Read a fact from a completed result. All returned pointers are borrowed until
// sema_free(). A missing fact yields NULL.
const SemaSymbol *sema_lookup_symbol(const SemaResult *result, const Node *node);
const SemaExprInfo *sema_lookup_expression(const SemaResult *result,
                                          const Node *node);
const Node *sema_lookup_jump_target(const SemaResult *result, const Node *node);
const SemaSwitchLabelInfo *sema_lookup_switch_label(const SemaResult *result, const Node *node);
const SemaConditionInfo *sema_lookup_condition(const SemaResult *result, const Node *node);
const SemaInitializer *sema_lookup_initializer(const SemaResult *result, const Node *node);

#endif // SEMA_H
