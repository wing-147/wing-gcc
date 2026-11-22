#ifndef PARSER_INTERNAL_H
#define PARSER_INTERNAL_H

#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/token.h"

#include <stdbool.h>
#include <stddef.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef enum {
    TYPE_SPEC_OK,
    TYPE_SPEC_DUPLICATE,
    TYPE_SPEC_SIGN_CONFLICT,
    TYPE_SPEC_TOO_MANY_LONG,
    TYPE_SPEC_LONG_DOUBLE_CONFLICT,
    TYPE_SPEC_INVALID_COMBINATION,
} TypeSpecifierErrorKind;

typedef struct {
    TypeSpecifierErrorKind kind;
    const Token *anchor;
} TypeSpecifierError;

typedef struct {
    DeclarationSpecifiers *ast;
    const Token *storage_tok;
    const Token *thread_local_tok;
    const Token *function_specifier_tok;
    const Token *alignment_tok;
    const Token *start;
    const Token *end;
} ParsedDeclarationSpecifiers;

typedef enum {
    DECLARATION_FILE,
    DECLARATION_BLOCK,
    DECLARATION_FOR,
    DECLARATION_OLD_STYLE_PARAMETER,
} DeclarationContext;

typedef enum {
    DECLARATOR_ORDINARY,
    DECLARATOR_PARAMETER,
} DeclaratorContext;

typedef enum {
    DECLARATOR_CONSTRAINT_NONE,
    DECLARATOR_FUNCTION_RETURNING_FUNCTION,
    DECLARATOR_FUNCTION_RETURNING_ARRAY,
    DECLARATOR_ARRAY_OF_FUNCTIONS,
    DECLARATOR_ARRAY_PARAMETER_ONLY,
    DECLARATOR_ARRAY_OUTERMOST_ONLY,
    DECLARATOR_STAR_PROTOTYPE_ONLY,
} DeclaratorConstraintKind;

typedef struct {
    DeclaratorConstraintKind kind;
    const Declarator *layer;
} DeclaratorConstraintError;

typedef struct {
    const Token *name_tok;
    Declarator *prefix;
    Declarator *shape;
} ParsedDeclarator;

typedef struct NameScope NameScope;

// Parser-local state travels with the explicit cursor. Grammar functions may
// advance `token`; only declaration/scope boundaries may mutate `scope`.
typedef struct {
    Token *token;
    NameScope *scope;
} ParserCursor;

typedef bool (*ParserStartsFn)(const ParserCursor *cursor);

// Declare a type-safe alternative descriptor without erasing a module-private
// parse callback's output type.
#define PARSER_ALTERNATIVE_FIELDS(starts_fn_type, parse_fn_type) \
    starts_fn_type starts;                                      \
    parse_fn_type parse

typedef Node *(*NodeAlternativeParseFn)(ParserCursor *cursor);
typedef bool (*NodeOutAlternativeParseFn)(ParserCursor *cursor, Node **out);

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, NodeAlternativeParseFn);
} NodeAlternative;

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, NodeOutAlternativeParseFn);
} NodeOutAlternative;

Token *current_token(const ParserCursor *cursor);
Token *peek_token(const ParserCursor *cursor, size_t distance);
Token *consume(ParserCursor *cursor);
bool token_is(const Token *tok, TokenKind kind);
bool at_eof(const Token *tok);
Token *consume_if(ParserCursor *cursor, TokenKind kind);
Token *expect(ParserCursor *cursor, TokenKind kind);
bool skip_balanced_group(ParserCursor *cursor, TokenKind open,
                         TokenKind close);
Token *expect_identifier(ParserCursor *cursor);
void parser_push_scope(ParserCursor *cursor);
void parser_pop_scope(ParserCursor *cursor);
bool parser_name_is_typedef(const ParserCursor *cursor, const Token *name);
void parser_declare_name(ParserCursor *cursor, const StringView *name,
                         bool is_typedef);

void type_specifier_free(TypeSpecifier *type);
void alignment_specifier_free(AlignmentSpecifier *specifier);
void declaration_specifiers_free(DeclarationSpecifiers *specifiers);
TypeSpecifier *new_builtin_type_specifier(TypeKind type, const Token *start,
                                          const Token *end);
TypeSpecifier *new_aggregate_type_specifier(TypeSpecifierKind kind,
                                            const Token *keyword,
                                            const Token *tag, Node *members,
                                            const Token *close);
bool type_specifier_is_anonymous_aggregate(const TypeSpecifier *type);
TypeSpecifier *new_enum_type_specifier(const Token *keyword, const Token *tag,
                                       Node *enumerators, const Token *close);
TypeSpecifier *new_typedef_type_specifier(const Token *name);
TypeSpecifier *new_atomic_type_specifier(const Token *keyword,
                                         TypeName *type_name,
                                         const Token *close);
AlignmentSpecifier *new_type_alignment_specifier(const Token *keyword,
                                                 TypeName *type_name,
                                                 const Token *close);
AlignmentSpecifier *new_expression_alignment_specifier(const Token *keyword,
                                                       Node *expression,
                                                       const Token *close);
void append_alignment_specifier(AlignmentSpecifier **head,
                                AlignmentSpecifier **tail,
                                AlignmentSpecifier *specifier);
DeclarationSpecifiers *new_declaration_specifiers(
    TypeSpecifier *type, unsigned qualifiers, StorageClass storage,
    unsigned function_specifiers, AlignmentSpecifier *alignments,
    const Token *start, const Token *end);
TypeKind declaration_specifiers_builtin_type(
    const DeclarationSpecifiers *specifiers);

Declarator *new_identifier_declarator(const Token *name_tok);
Declarator *new_pointer_declarator(const Token *star, unsigned qualifiers);
Declarator *new_array_declarator(Declarator *inner, Node *size,
                                 unsigned qualifiers, ArrayDeclaratorForm form,
                                 const Token *open,
                                 const Token *close);
Declarator *new_function_declarator(Declarator *inner,
                                    Node *parameter_clause,
                                    FunctionParameterForm form, bool variadic,
                                    const Token *open, const Token *close);
void parenthesize_declarator(Declarator *declarator, const Token *open,
                             const Token *close);
const Declarator *declarator_entity_layer(const Declarator *decl);
const Declarator *declarator_function(const Declarator *decl);
const StringView *declarator_name(const Declarator *decl);
const SourceSpan *declarator_name_span(const Declarator *decl);
bool find_declarator_constraint(const Declarator *decl,
                                DeclaratorContext context,
                                DeclaratorConstraintError *error);
Declarator *attach_pointer_prefix(Declarator *prefix, Declarator *direct);
void discard_parsed_declarator(ParsedDeclarator *decl);

Node *new_empty(SourceSpan span);
Node *new_num(const Token *tok);
Node *new_string(Token *tok);
Node *new_identifier(const Token *name_tok);
void parenthesize_expression(Node *expr, const Token *open,
                             const Token *close);
Node *new_init_declarator(Node *init, Declarator *declarator);
Node *new_declaration(DeclarationSpecifiers *specifiers, Node *declarators,
                      const Token *semi);
Node *new_struct_declaration(DeclarationSpecifiers *specifiers,
                             Node *declarators, const Token *semi);
Node *new_struct_declarator(Declarator *declarator, Node *width);
Node *new_enumerator(const Token *name, Node *value);
Node *new_static_assertion(const Token *keyword, Node *expression,
                           Node *message, const Token *semi);
Node *new_parameter(ParsedDeclarationSpecifiers *specs, ParsedDeclarator *decl);
bool parameter_is_plain_void(const Node *parameter);
Node *new_unary_shell(Operator op, const Token *op_tok);
Node **append_unary_prefix(Node **slot, Operator op, const Token *op_tok);
Node *new_unary(Operator op, const Token *op_tok, Node *operand);
void attach_prefix_operand(Node *root, Node **operand_slot, Node *operand);
Node *new_binary(Operator op, Node *lhs, Node *rhs);
Node **append_assignment_prefix(Node **slot, Operator op, Node *lhs);
void attach_assignment_rhs(Node *root, Node **rhs_slot, Node *rhs);
Node **append_conditional_prefix(Node **slot, Node *cond, Node *then_expr);
void attach_conditional_else(Node *root, Node **else_slot, Node *else_expr);
Node *new_expr_stmt(Node *expr, const Token *semi);
Node *new_return(Node *expr, const Token *keyword, const Token *semi);
Node *new_label(const Token *name, Node *stmt);
Node *new_case(Node *expr, Node *stmt, const Token *keyword);
Node *new_default(Node *stmt, const Token *keyword);
Node *new_if(Node *cond, Node *then_stmt, Node *else_stmt, const Token *if_tok);
Node *new_switch(Node *cond, Node *body, const Token *keyword);
Node *new_while(Node *cond, Node *body, const Token *while_tok);
Node *new_do_while(Node *body, Node *cond, const Token *do_tok, const Token *semi);
Node *new_for(Node *init, Node *cond, Node *post, Node *body, const Token *for_tok);
Node *new_jump(NodeType type, const Token *keyword, const Token *semi);
Node *new_goto(const Token *name, const Token *keyword, const Token *semi);
Node *new_call(Node *callee, Node *args, const Token *close);
Node *new_subscript(Node *base, Node *index, const Token *close);
Node *new_member_access(MemberAccessKind kind, Node *base,
                        const Token *name);
TypeName *new_type_name(TypeSpecifier *type, unsigned qualifiers,
                        Declarator *declarator, const Token *start,
                        const SourceSpan *end);
void parenthesize_type_name(TypeName *type_name, const Token *open,
                            const Token *close);
void type_name_free(TypeName *type_name);
Node *new_compound_literal(TypeName *type_name, Node *initializer);
GenericAssociation *new_generic_association(TypeName *type_name,
                                            Node *expression,
                                            const Token *start);
void append_generic_association(GenericAssociation **head,
                                GenericAssociation **tail,
                                GenericAssociation *association);
void generic_association_free(GenericAssociation *association);
Node *new_generic_selection(const Token *keyword, Node *control,
                            GenericAssociation *associations,
                            const Token *close);
Node *new_cast(TypeName *type_name, Node *expr);
Node *new_type_query(NodeType type, const Token *keyword, TypeName *type_name);
Node *new_initializer_list(Node *items, const Token *open, const Token *close);
Node *new_designated_initializer(Node *designators, Node *value);
Node *new_index_designator(const Token *open, Node *index, const Token *close);
Node *new_member_designator(const Token *dot, const Token *name);
Node *new_block(Node *items, const Token *open, const Token *close);
Node *new_program(Node *items);
void append_node_list(Node **head, Node **tail, Node *chain);
Node *new_function(DeclarationSpecifiers *specifiers,
                   Declarator *declarator, Node *declarations, Node *body);
void node_free(Node *node);

Node *parse_unary_expression(ParserCursor *cursor);
Node *parse_cast_expression(ParserCursor *cursor);
Node *parse_conditional_expression(ParserCursor *cursor);
Node *parse_assignment_expression(ParserCursor *cursor);
Node *parse_expression(ParserCursor *cursor);
Node *parse_constant_expression(ParserCursor *cursor);
bool starts_assignment_expression(const ParserCursor *cursor);
Node *parse_initializer(ParserCursor *cursor);
bool parse_initializer_list(ParserCursor *cursor, Node **out);
Node *parse_statement(ParserCursor *cursor);
Node *parse_compound_statement(ParserCursor *cursor);
bool starts_block_item(const ParserCursor *cursor);
bool parse_declaration(ParserCursor *cursor, DeclarationContext context,
                       const Token *context_anchor, Node **out);
bool parse_external_declaration(ParserCursor *cursor, Node **out);
bool starts_external_declaration(const ParserCursor *cursor);
bool starts_external_declaration_sync(const ParserCursor *cursor);
bool parse_type_name(ParserCursor *cursor, TypeName **out);
bool starts_declaration_specifiers(const ParserCursor *cursor);
bool starts_type_name(const ParserCursor *cursor);
bool starts_declaration(const ParserCursor *cursor);

void skip_directives(ParserCursor *cursor);
void handle_parser_error_expected_token(const Token *tok, TokenKind kind);
void handle_parser_error_expected_syntax(const Token *tok, const char *what);
void handle_parser_error_unexpected_external_declaration(
    ParserCursor *cursor, const ParserCursor *item_start);
void handle_parser_error_external_declaration(
    ParserCursor *cursor, const ParserCursor *item_start);
void handle_parser_error_expected_expression(const Token *tok);
void handle_parser_error_alignof_expression(const Token *keyword);
void handle_parser_error_return_missing_operand(const Token *eof_tok,
                                                const Token *keyword);
void handle_parser_error_type_specifier(const TypeSpecifierError *error);
void handle_parser_error_multiple_storage_classes(const Token *tok);
bool handle_parser_error_declaration_tail(
    ParserCursor *cursor, const Node *declarations, DeclarationContext context);
void handle_parser_error_useless_declaration(const Token *type_tok);
void handle_parser_error_unclosed_block(const Token *open,
                                        const Token *stmt_start);
void handle_parser_error_else_without_if(ParserCursor *cursor);
void handle_parser_error_parameter_list(const Token *tok);
void handle_parser_error_parameter_expression(ParserCursor *cursor);
void handle_parser_error_block_item(ParserCursor *cursor,
                                    const ParserCursor *item_start);
void handle_parser_error_expected_block_item(
    ParserCursor *cursor, const ParserCursor *item_start);
void validate_declarator_constraints(const ParsedDeclarator *decl,
                                     DeclaratorContext context);
void validate_init_declarator_constraints(const Node *decl);
void validate_declaration_constraints(
    const ParsedDeclarationSpecifiers *specs, DeclarationContext context,
    const Token *context_anchor, const Node *decl);
void validate_parameter_declaration_constraints(
    const ParsedDeclarationSpecifiers *specs, const ParsedDeclarator *decl);
void validate_parameter_type_list_constraints(const Node *params);
void validate_struct_declaration_constraints(
    const DeclarationSpecifiers *specifiers, const Node *declarators,
    const Token *semi);
void validate_function_definition_storage_class(
    const ParsedDeclarationSpecifiers *specs, const ParsedDeclarator *decl);
void validate_function_definition_constraints(const Declarator *function);
void validate_function_definition_declarations(const Declarator *function,
                                                const Node *declarations);
void validate_generic_selection_constraints(
    const GenericAssociation *associations);

#endif // PARSER_INTERNAL_H
