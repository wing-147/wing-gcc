#ifndef AST_H
#define AST_H

#include "frontend/token.h"
#include "util/source.h"

#include <stdbool.h>
#include <stdint.h>

// Syntax-preserving C11 AST built bottom-up by the parser. Owning pointers and
// decoded buffers have one owner; SourceSpan and StringView borrow the source.
// Grammar lists use intrusive owning chains, avoiding separate containers.
// Construction and destruction remain private to parser/ast.c.

typedef enum {
    ND_PROGRAM,   // translation-unit (C11 6.9)
    ND_FUNCTION,  // function-definition (C11 6.9.1)
    ND_BLOCK,     // compound-statement (C11 6.8.2)
    ND_EXPR_STMT, // expression statement (C11 6.8.3)
    ND_EMPTY,     // empty statement `;` (C11 6.8.3)
    ND_DECL,      // declaration-specifiers + init-declarator list (C11 6.7)
    ND_INIT_DECL, // declarator + optional initializer (C11 6.7)
    ND_PARAMETER, // parameter-declaration (C11 6.7.6)
    ND_STRUCT_DECL,       // struct-declaration (C11 6.7.2.1)
    ND_STRUCT_DECLARATOR, // struct-declarator (C11 6.7.2.1)
    ND_ENUMERATOR,        // enumerator (C11 6.7.2.2)
    ND_STATIC_ASSERT,     // static_assert-declaration (C11 6.7.10)
    ND_RETURN,    // return [expr]; (C11 6.8.6.4)
    ND_LABEL,     // identifier: statement (C11 6.8.1)
    ND_CASE,      // case constant-expression: statement (C11 6.8.1)
    ND_DEFAULT,   // default: statement (C11 6.8.1)
    ND_IF,        // if (cond) then [else] (C11 6.8.4.1)
    ND_SWITCH,    // switch (cond) body (C11 6.8.4.2)
    ND_WHILE,     // while (cond) body (C11 6.8.5.1)
    ND_DO_WHILE,  // do body while (cond); (C11 6.8.5.2)
    ND_FOR,       // for (init; cond; post) body (C11 6.8.5.3)
    ND_GOTO,      // goto identifier; (C11 6.8.6.1)
    ND_BREAK,     // break; (C11 6.8.6.3)
    ND_CONTINUE,  // continue; (C11 6.8.6.2)
    ND_NUM,       // integer, floating or character constant (C11 6.4.4)
    ND_STRING,    // string literal (C11 6.4.5)
    ND_IDENTIFIER, // identifier syntax (C11 6.4.2.1)
    ND_UNARY,     // unary and postfix operators (C11 6.5.2.4, 6.5.3)
    ND_BINARY,    // binary and comma operators (C11 6.5.5–6.5.14, 6.5.17)
    ND_ASSIGN,    // lhs = rhs and compound assignments (C11 6.5.16)
    ND_COND,      // cond ? then : else (C11 6.5.15)
    ND_CALL,      // callee(args) (C11 6.5.2.2)
    ND_SUBSCRIPT, // base[index] (C11 6.5.2.1)
    ND_MEMBER,    // base.name or base->name (C11 6.5.2.3)
    ND_COMPOUND_LITERAL,  // compound literal (C11 6.5.2.5)
    ND_GENERIC_SELECTION, // generic-selection (C11 6.5.1.1)
    ND_CAST,              // cast-expression (C11 6.5.4)
    ND_SIZEOF_TYPE,       // sizeof(type-name) (C11 6.5.3)
    ND_ALIGNOF_TYPE,      // _Alignof(type-name) (C11 6.5.3)
    ND_INIT_LIST,         // braced initializer-list (C11 6.7.9)
    ND_DESIGNATED_INIT,   // designation + initializer (C11 6.7.9)
    ND_INDEX_DESIGNATOR,  // [constant-expression] (C11 6.7.9)
    ND_MEMBER_DESIGNATOR, // .identifier (C11 6.7.9)
} NodeType;

typedef enum {
    STORAGE_NONE,
    STORAGE_TYPEDEF = 1u << 0,
    STORAGE_EXTERN = 1u << 1,
    STORAGE_STATIC = 1u << 2,
    STORAGE_AUTO = 1u << 3,
    STORAGE_REGISTER = 1u << 4,
    STORAGE_THREAD_LOCAL = 1u << 5,
} StorageClass;

typedef enum {
    TYPE_QUAL_CONST = 1u << 0,
    TYPE_QUAL_RESTRICT = 1u << 1,
    TYPE_QUAL_VOLATILE = 1u << 2,
    TYPE_QUAL_ATOMIC = 1u << 3,
} TypeQualifier;

// Resolved builtin declaration type. This is intentionally separate from the
// literal classification in token.h; pointer/array/function layers remain in
// Declarator until semantic type construction lands.
typedef enum {
    TYPE_INT,
    TYPE_UINT,
    TYPE_LONG,
    TYPE_ULONG,
    TYPE_LLONG,
    TYPE_ULLONG,
    TYPE_SCHAR,
    TYPE_UCHAR,
    TYPE_USHORT,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_LDOUBLE,
    TYPE_FLOAT_COMPLEX,
    TYPE_DOUBLE_COMPLEX,
    TYPE_LDOUBLE_COMPLEX,
    TYPE_CHAR,
    TYPE_SHORT,
    TYPE_VOID,
    TYPE_BOOL,
} TypeKind;

typedef enum {
    TYPE_SPECIFIER_BUILTIN,
    TYPE_SPECIFIER_STRUCT,
    TYPE_SPECIFIER_UNION,
    TYPE_SPECIFIER_ENUM,
    TYPE_SPECIFIER_TYPEDEF_NAME,
    TYPE_SPECIFIER_ATOMIC,
} TypeSpecifierKind;

typedef enum {
    FUNCTION_SPEC_INLINE = 1u << 0,
    FUNCTION_SPEC_NORETURN = 1u << 1,
} FunctionSpecifier;

typedef enum {
    ALIGNMENT_TYPE,
    ALIGNMENT_EXPRESSION,
} AlignmentSpecifierKind;

// All operators share one enum: the parser tables and lookups then stay single
// regardless of precedence, and the NodeType already says whether an operator
// is unary, binary or assignment.
typedef enum {
    UNARY_PLUS,
    UNARY_NEG,
    UNARY_BITNOT,
    UNARY_LOGNOT,
    UNARY_PRE_INC,
    UNARY_PRE_DEC,
    UNARY_POST_INC,
    UNARY_POST_DEC,
    UNARY_ADDR,
    UNARY_DEREF,
    UNARY_SIZEOF,
    UNARY_ALIGNOF,
    BINARY_MUL,
    BINARY_DIV,
    BINARY_REM,
    BINARY_ADD,
    BINARY_SUB,
    BINARY_SHL,
    BINARY_SHR,
    BINARY_LT,
    BINARY_LE,
    BINARY_GT,
    BINARY_GE,
    BINARY_EQ,
    BINARY_NE,
    BINARY_BITAND,
    BINARY_BITXOR,
    BINARY_BITOR,
    BINARY_LOGAND,
    BINARY_LOGOR,
    BINARY_COMMA,
    ASSIGN,
    ADD_ASSIGN,
    SUB_ASSIGN,
    MUL_ASSIGN,
    DIV_ASSIGN,
    MOD_ASSIGN,
    SHL_ASSIGN,
    SHR_ASSIGN,
    AND_ASSIGN,
    XOR_ASSIGN,
    OR_ASSIGN,
} Operator;

typedef enum {
    MEMBER_DIRECT,
    MEMBER_INDIRECT,
} MemberAccessKind;

typedef enum {
    DECLARATOR_IDENT,
    DECLARATOR_POINTER,
    DECLARATOR_ARRAY,
    DECLARATOR_FUNCTION,
} DeclaratorKind;

typedef enum {
    ARRAY_DECLARATOR_BOUND,
    ARRAY_DECLARATOR_STATIC,
    ARRAY_DECLARATOR_STAR,
} ArrayDeclaratorForm;

typedef enum {
    FUNCTION_PARAMS_PROTOTYPE,
    FUNCTION_PARAMS_IDENTIFIER_LIST,
} FunctionParameterForm;

typedef struct AlignmentSpecifier AlignmentSpecifier;
typedef struct DeclarationSpecifiers DeclarationSpecifiers;
typedef struct Declarator Declarator;
typedef struct GenericAssociation GenericAssociation;
typedef struct Node Node;
typedef struct TypeName TypeName;
typedef struct TypeSpecifier TypeSpecifier;

// Syntax-level type specifier. It owns structured payloads selected by `kind`;
// tag and typedef spellings borrow the source until semantic identity exists.
struct TypeSpecifier {
    TypeSpecifierKind kind;
    SourceSpan span;
    union {
        TypeKind builtin;
        struct {
            StringView tag;
            Node *members; // ND_STRUCT_DECL or ND_STATIC_ASSERT chain
            bool has_body;
        } aggregate;
        struct {
            StringView tag;
            Node *enumerators; // ND_ENUMERATOR chain
            bool has_body;
        } enumeration;
        StringView typedef_name;
        TypeName *atomic_type;
    } u;
};

// One alignment-specifier owns its selected type-name or constant-expression;
// the list head owns the complete `next` suffix.
struct AlignmentSpecifier {
    SourceSpan span;
    AlignmentSpecifierKind kind;
    union {
        TypeName *type_name;
        Node *expression;
    } u;
    AlignmentSpecifier *next;
};

// Shared declaration-specifiers own their type and alignment-specifier chain.
struct DeclarationSpecifiers {
    SourceSpan span;
    TypeSpecifier *type;
    unsigned qualifiers;
    // STORAGE_* flag set; only _Thread_local may combine with static/extern.
    StorageClass storage;
    unsigned function_specifiers;
    AlignmentSpecifier *alignments;
};

// C11 6.7.6 declarators form a recursive syntax tree. `inner` preserves the
// distinction between `*f(void)` and `(*f)(void)`; semantic type construction
// can therefore walk from the identifier through each derived layer.
struct Declarator {
    DeclaratorKind kind;
    SourceSpan span;
    SourceSpan suffix_span; // Delimiters for array and function layers.
    Declarator *inner;
    union {
        struct {
            StringView name;
        } ident;
        struct {
            FunctionParameterForm form;
            // Owns either a parameter-type-list or an identifier-list. An
            // function-definition declaration-list belongs to ND_FUNCTION.
            union {
                struct {
                    Node *parameters; // ND_PARAMETER chain
                    bool variadic;
                } prototype;
                struct {
                    Node *identifiers; // ND_IDENTIFIER chain
                } old_style;
            } clause;
        } function;
        struct {
            unsigned qualifiers;
        } pointer;
        struct {
            Node *size; // assignment-expression; NULL for [] and [*]
            unsigned qualifiers;
            ArrayDeclaratorForm form;
        } array;
    } u;
};

// C11 6.7.7 type-name owns its type specifier and optional abstract declarator;
// the enclosing cast, query, alignment, generic association, compound literal
// or atomic type specifier owns the completed TypeName.
struct TypeName {
    SourceSpan span;
    TypeSpecifier *type;
    unsigned qualifiers;
    Declarator *declarator;
};

// One generic-association owns its assignment-expression and optional type-name;
// a NULL type_name denotes `default`. The head owns the complete `next` suffix.
struct GenericAssociation {
    SourceSpan span;
    TypeName *type_name;
    Node *expression;
    GenericAssociation *next;
};

struct Node {
    NodeType type;
    SourceSpan span; // location of the construct, for later diagnostics
    // Owning intrusive sibling link. A list head owns its complete `next`
    // suffix; the link represents grammar-list peers, never a child edge.
    Node *next;

    union {
        struct {
            // The integer member stores a raw 64-bit pattern; `literal`
            // supplies signedness, so uint64_t conversion recovers UINT64_MAX.
            LiteralValue value;
            LiteralType literal;
        } num;
        struct {
            char *value; // decoded bytes, heap copy owned by the node; may hold NUL
            int length;  // decoded byte count
        } string;
        struct {
            DeclarationSpecifiers *specifiers;
            Node *declarators;
        } decl;
        struct {
            Declarator *declarator;
            Node *init; // NULL when there is no `= initializer`
        } init_decl;
        struct {
            DeclarationSpecifiers *specifiers;
            Declarator *declarator;
        } parameter;
        struct {
            DeclarationSpecifiers *specifiers;
            Node *declarators;
        } struct_decl;
        struct {
            Declarator *declarator;
            Node *width;
        } struct_declarator;
        struct {
            StringView name;
            Node *value;
        } enumerator;
        struct {
            Node *expression;
            Node *message;
        } static_assertion;
        struct {
            StringView name; // borrowed from the source buffer
        } identifier;
        struct {
            Operator op;
            Node *operand;
        } unary;
        struct {
            Operator op;
            Node *lhs;
            Node *rhs;
        } binary;
        struct {
            Operator op;
            Node *lhs;
            Node *rhs;
        } assign;
        struct {
            Node *cond;
            Node *then_expr;
            Node *else_expr;
        } cond;
        struct {
            Node *expr; // NULL for `return;`
        } expr;         // ND_EXPR_STMT and ND_RETURN share this shape
        struct {
            StringView name;
            Node *stmt;
        } label;
        struct {
            Node *expr;
            Node *stmt;
        } case_stmt;
        struct {
            Node *stmt;
        } default_stmt;
        struct {
            Node *cond;
            Node *then_stmt;
            Node *else_stmt; // NULL when there is no `else`
        } if_stmt;
        struct {
            Node *cond;
            Node *body;
        } switch_stmt;
        struct {
            Node *cond;
            Node *body;
        } while_stmt; // ND_WHILE
        struct {
            Node *body;
            Node *cond;
        } do_while;
        struct {
            Node *init; // NULL, ND_DECL or an expression
            Node *cond; // NULL when omitted
            Node *post; // NULL when omitted
            Node *body;
        } for_stmt;
        struct {
            StringView name;
        } goto_stmt;
        struct {
            Node *callee;
            Node *args; // chained via next; NULL when the call has no arguments
        } call;
        struct {
            Node *base;
            Node *index;
        } subscript;
        struct {
            MemberAccessKind kind;
            Node *base;
            StringView name;
        } member_access;
        struct {
            TypeName *type_name;
            Node *initializer;
        } compound_literal;
        struct {
            Node *control;
            GenericAssociation *associations;
        } generic_selection;
        struct {
            TypeName *type_name;
            Node *expr;
        } cast;
        struct {
            TypeName *type_name;
        } type_query; // ND_SIZEOF_TYPE and ND_ALIGNOF_TYPE
        struct {
            Node *designators;
            Node *value;
        } designated;
        struct {
            Node *index;
        } index_designator;
        struct {
            StringView name;
        } member;
        struct {
            Node *external_declarations;
        } program;
        struct {
            Node *items;
        } block;
        struct {
            Node *initializers;
        } initializer_list;
        struct {
            DeclarationSpecifiers *specifiers;
            Declarator *declarator;
            Node *declarations; // old-style declaration-list, otherwise NULL
            Node *body;         // required: prototypes are ND_DECL
        } func;
    } u;
};

#endif // AST_H
