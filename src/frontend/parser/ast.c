// AST ownership for the parser subsystem. Constructors take ownership of
// children at their call boundary. node_free() iterates over Node edges;
// auxiliary TypeSpecifier and TypeName ownership still follows typed teardown.

#include "parser_internal.h"

#include "util/common.h"

#include <stdlib.h>

// ---------------------------------------------------------------------------
// Private construction and destruction primitives
// ---------------------------------------------------------------------------

// Return the source-backed spelling borrowed by a token.
static StringView token_text(const Token *tok)
{
    return tok->span.text;
}

// Allocate one owned declarator layer and take ownership of `inner`.
static Declarator *new_declarator(DeclaratorKind kind, Declarator *inner,
                                  SourceSpan span)
{
    Declarator *decl = xcalloc(1, sizeof(*decl));
    decl->kind = kind;
    decl->inner = inner;
    decl->span = span;
    return decl;
}

// Release a declarator chain and its variant-selected children iteratively.
static void declarator_free(Declarator *decl)
{
    while (decl) {
        Declarator *inner = decl->inner;
        switch (decl->kind) {
        case DECLARATOR_IDENT:
        case DECLARATOR_POINTER:
            break;
        case DECLARATOR_ARRAY:
            node_free(decl->u.array.size);
            break;
        case DECLARATOR_FUNCTION:
            if (decl->u.function.form == FUNCTION_PARAMS_PROTOTYPE)
                node_free(decl->u.function.clause.prototype.parameters);
            else if (decl->u.function.form == FUNCTION_PARAMS_IDENTIFIER_LIST)
                node_free(decl->u.function.clause.old_style.identifiers);
            else
                unreachable();
            break;
        default:
            unreachable();
        }
        free(decl);
        decl = inner;
    }
}

// Find the identifier leaf without changing the declarator shape.
static const Declarator *declarator_identifier(const Declarator *decl)
{
    while (decl && decl->inner)
        decl = decl->inner;
    return decl && decl->kind == DECLARATOR_IDENT ? decl : NULL;
}

// Allocate a zeroed Node with its final variant and source span established.
static Node *new_node(NodeType type, SourceSpan span)
{
    Node *node = xcalloc(1, sizeof(*node));
    node->type = type;
    node->span = span;
    return node;
}

// Transfer a Node sibling chain onto the iterative destruction work stack.
static void push_node_chain(Node **work, Node *chain)
{
    while (chain) {
        Node *next = chain->next;
        chain->next = *work;
        *work = chain;
        chain = next;
    }
}

// Release one generic-association chain while transferring its expression
// roots to the caller's iterative Node work stack.
static void release_generic_association_chain(
    GenericAssociation *association, Node **work)
{
    while (association) {
        GenericAssociation *next = association->next;
        type_name_free(association->type_name);
        push_node_chain(work, association->expression);
        free(association);
        association = next;
    }
}

// Dispatch one Node variant to its typed teardown and transfer owned Node
// children to the shared iterative work stack.
static void release_node_contents(Node *node, Node **work)
{
    switch (node->type) {
    case ND_PROGRAM:
        push_node_chain(work, node->u.program.external_declarations);
        break;
    case ND_BLOCK:
        push_node_chain(work, node->u.block.items);
        break;
    case ND_INIT_LIST:
        push_node_chain(work, node->u.initializer_list.initializers);
        break;
    case ND_FUNCTION:
        declaration_specifiers_free(node->u.func.specifiers);
        declarator_free(node->u.func.declarator);
        push_node_chain(work, node->u.func.declarations);
        push_node_chain(work, node->u.func.body);
        break;
    case ND_EXPR_STMT:
    case ND_RETURN:
        push_node_chain(work, node->u.expr.expr);
        break;
    case ND_LABEL:
        push_node_chain(work, node->u.label.stmt);
        break;
    case ND_CASE:
        push_node_chain(work, node->u.case_stmt.expr);
        push_node_chain(work, node->u.case_stmt.stmt);
        break;
    case ND_DEFAULT:
        push_node_chain(work, node->u.default_stmt.stmt);
        break;
    case ND_NUM:
    case ND_EMPTY:
    case ND_BREAK:
    case ND_CONTINUE:
    case ND_GOTO:
    case ND_IDENTIFIER:
    case ND_MEMBER_DESIGNATOR:
        break;
    case ND_STRING:
        free(node->u.string.value);
        break;
    case ND_DECL:
        declaration_specifiers_free(node->u.decl.specifiers);
        push_node_chain(work, node->u.decl.declarators);
        break;
    case ND_INIT_DECL:
        declarator_free(node->u.init_decl.declarator);
        push_node_chain(work, node->u.init_decl.init);
        break;
    case ND_PARAMETER:
        declaration_specifiers_free(node->u.parameter.specifiers);
        declarator_free(node->u.parameter.declarator);
        break;
    case ND_STRUCT_DECL:
        declaration_specifiers_free(node->u.struct_decl.specifiers);
        push_node_chain(work, node->u.struct_decl.declarators);
        break;
    case ND_STRUCT_DECLARATOR:
        declarator_free(node->u.struct_declarator.declarator);
        push_node_chain(work, node->u.struct_declarator.width);
        break;
    case ND_ENUMERATOR:
        push_node_chain(work, node->u.enumerator.value);
        break;
    case ND_STATIC_ASSERT:
        push_node_chain(work, node->u.static_assertion.expression);
        push_node_chain(work, node->u.static_assertion.message);
        break;
    case ND_UNARY:
        push_node_chain(work, node->u.unary.operand);
        break;
    case ND_BINARY:
        push_node_chain(work, node->u.binary.lhs);
        push_node_chain(work, node->u.binary.rhs);
        break;
    case ND_ASSIGN:
        push_node_chain(work, node->u.assign.lhs);
        push_node_chain(work, node->u.assign.rhs);
        break;
    case ND_COND:
        push_node_chain(work, node->u.cond.cond);
        push_node_chain(work, node->u.cond.then_expr);
        push_node_chain(work, node->u.cond.else_expr);
        break;
    case ND_IF:
        push_node_chain(work, node->u.if_stmt.cond);
        push_node_chain(work, node->u.if_stmt.then_stmt);
        push_node_chain(work, node->u.if_stmt.else_stmt);
        break;
    case ND_SWITCH:
        push_node_chain(work, node->u.switch_stmt.cond);
        push_node_chain(work, node->u.switch_stmt.body);
        break;
    case ND_WHILE:
        push_node_chain(work, node->u.while_stmt.cond);
        push_node_chain(work, node->u.while_stmt.body);
        break;
    case ND_DO_WHILE:
        push_node_chain(work, node->u.do_while.body);
        push_node_chain(work, node->u.do_while.cond);
        break;
    case ND_FOR:
        push_node_chain(work, node->u.for_stmt.init);
        push_node_chain(work, node->u.for_stmt.cond);
        push_node_chain(work, node->u.for_stmt.post);
        push_node_chain(work, node->u.for_stmt.body);
        break;
    case ND_CALL:
        push_node_chain(work, node->u.call.callee);
        push_node_chain(work, node->u.call.args);
        break;
    case ND_SUBSCRIPT:
        push_node_chain(work, node->u.subscript.base);
        push_node_chain(work, node->u.subscript.index);
        break;
    case ND_MEMBER:
        push_node_chain(work, node->u.member_access.base);
        break;
    case ND_COMPOUND_LITERAL:
        type_name_free(node->u.compound_literal.type_name);
        push_node_chain(work, node->u.compound_literal.initializer);
        break;
    case ND_GENERIC_SELECTION:
        push_node_chain(work, node->u.generic_selection.control);
        release_generic_association_chain(
            node->u.generic_selection.associations, work);
        break;
    case ND_CAST:
        type_name_free(node->u.cast.type_name);
        push_node_chain(work, node->u.cast.expr);
        break;
    case ND_SIZEOF_TYPE:
    case ND_ALIGNOF_TYPE:
        type_name_free(node->u.type_query.type_name);
        break;
    case ND_DESIGNATED_INIT:
        push_node_chain(work, node->u.designated.designators);
        push_node_chain(work, node->u.designated.value);
        break;
    case ND_INDEX_DESIGNATOR:
        push_node_chain(work, node->u.index_designator.index);
        break;
    default:
        unreachable();
    }
}

// ---------------------------------------------------------------------------
// Type syntax and ownership
// ---------------------------------------------------------------------------

// Release one syntax-level type specifier and every structured body it owns.
void type_specifier_free(TypeSpecifier *type)
{
    if (!type)
        return;
    switch (type->kind) {
    case TYPE_SPECIFIER_BUILTIN:
    case TYPE_SPECIFIER_TYPEDEF_NAME:
        break;
    case TYPE_SPECIFIER_STRUCT:
    case TYPE_SPECIFIER_UNION:
        node_free(type->u.aggregate.members);
        break;
    case TYPE_SPECIFIER_ENUM:
        node_free(type->u.enumeration.enumerators);
        break;
    case TYPE_SPECIFIER_ATOMIC:
        type_name_free(type->u.atomic_type);
        break;
    default:
        unreachable();
    }
    free(type);
}

// Release an alignment-specifier chain and each selected type-name or
// constant-expression that remains owned by it.
void alignment_specifier_free(AlignmentSpecifier *specifier)
{
    while (specifier) {
        AlignmentSpecifier *next = specifier->next;
        switch (specifier->kind) {
        case ALIGNMENT_TYPE:
            type_name_free(specifier->u.type_name);
            break;
        case ALIGNMENT_EXPRESSION:
            node_free(specifier->u.expression);
            break;
        default:
            unreachable();
        }
        free(specifier);
        specifier = next;
    }
}

// Release declaration-specifiers that have not transferred to an AST owner.
void declaration_specifiers_free(DeclarationSpecifiers *specifiers)
{
    if (!specifiers)
        return;
    type_specifier_free(specifiers->type);
    alignment_specifier_free(specifiers->alignments);
    free(specifiers);
}

// Establish the common variant and source-range invariants for every
// syntax-level type specifier; typed constructors complete the selected payload.
static TypeSpecifier *new_type_specifier(TypeSpecifierKind kind,
                                         SourceSpan span)
{
    TypeSpecifier *specifier = xcalloc(1, sizeof(*specifier));
    specifier->kind = kind;
    specifier->span = span;
    return specifier;
}

// Construct the resolved builtin type-specifier syntax retained by the AST.
TypeSpecifier *new_builtin_type_specifier(TypeKind type, const Token *start,
                                          const Token *end)
{
    TypeSpecifier *specifier = new_type_specifier(
        TYPE_SPECIFIER_BUILTIN,
        source_span_join(&start->span, &end->span));
    specifier->u.builtin = type;
    return specifier;
}

// Construct a struct-or-union specifier, retaining its optional tag and owned
// member declaration chain. A NULL close denotes a tag-only reference.
TypeSpecifier *new_aggregate_type_specifier(TypeSpecifierKind kind,
                                            const Token *keyword,
                                            const Token *tag, Node *members,
                                            const Token *close)
{
    if (kind != TYPE_SPECIFIER_STRUCT && kind != TYPE_SPECIFIER_UNION)
        unreachable();
    const SourceSpan *end = close ? &close->span : tag ? &tag->span : &keyword->span;
    TypeSpecifier *specifier = new_type_specifier(
        kind, source_span_join(&keyword->span, end));
    specifier->u.aggregate.tag = tag ? token_text(tag) : (StringView){ 0 };
    specifier->u.aggregate.members = members;
    specifier->u.aggregate.has_body = close != NULL;
    return specifier;
}

// Identify the exact tagless definition form that C11 permits to declare an
// anonymous structure or union member without a struct-declarator-list.
bool type_specifier_is_anonymous_aggregate(const TypeSpecifier *type)
{
    return (type->kind == TYPE_SPECIFIER_STRUCT ||
            type->kind == TYPE_SPECIFIER_UNION) &&
           type->u.aggregate.has_body &&
           string_view_length(type->u.aggregate.tag) == 0;
}

// Construct an enum specifier with an optional tag and owned enumerator chain.
TypeSpecifier *new_enum_type_specifier(const Token *keyword, const Token *tag,
                                       Node *enumerators, const Token *close)
{
    const SourceSpan *end = close ? &close->span : tag ? &tag->span : &keyword->span;
    TypeSpecifier *specifier = new_type_specifier(
        TYPE_SPECIFIER_ENUM, source_span_join(&keyword->span, end));
    specifier->u.enumeration.tag = tag ? token_text(tag) : (StringView){ 0 };
    specifier->u.enumeration.enumerators = enumerators;
    specifier->u.enumeration.has_body = close != NULL;
    return specifier;
}

// Construct a typedef-name specifier borrowing its ordinary identifier spelling.
TypeSpecifier *new_typedef_type_specifier(const Token *name)
{
    TypeSpecifier *specifier = new_type_specifier(
        TYPE_SPECIFIER_TYPEDEF_NAME, name->span);
    specifier->u.typedef_name = token_text(name);
    return specifier;
}

// Construct an atomic-type-specifier and take ownership of its enclosed
// type-name.
TypeSpecifier *new_atomic_type_specifier(const Token *keyword,
                                         TypeName *type_name,
                                         const Token *close)
{
    TypeSpecifier *specifier = new_type_specifier(
        TYPE_SPECIFIER_ATOMIC,
        source_span_join(&keyword->span, &close->span));
    specifier->u.atomic_type = type_name;
    return specifier;
}

// Construct one type-name alignment request and take ownership of its type.
AlignmentSpecifier *new_type_alignment_specifier(const Token *keyword,
                                                 TypeName *type_name,
                                                 const Token *close)
{
    AlignmentSpecifier *specifier = xcalloc(1, sizeof(*specifier));
    specifier->span = source_span_join(&keyword->span, &close->span);
    specifier->kind = ALIGNMENT_TYPE;
    specifier->u.type_name = type_name;
    return specifier;
}

// Construct one constant-expression alignment request and take ownership of
// its expression.
AlignmentSpecifier *new_expression_alignment_specifier(const Token *keyword,
                                                       Node *expression,
                                                       const Token *close)
{
    AlignmentSpecifier *specifier = xcalloc(1, sizeof(*specifier));
    specifier->span = source_span_join(&keyword->span, &close->span);
    specifier->kind = ALIGNMENT_EXPRESSION;
    specifier->u.expression = expression;
    return specifier;
}

// Append one detached alignment-specifier to a parser-owned chain and update
// both list handles; ownership stays with the head after the transfer.
void append_alignment_specifier(AlignmentSpecifier **head,
                                AlignmentSpecifier **tail,
                                AlignmentSpecifier *specifier)
{
    if (!specifier || specifier->next)
        unreachable();
    if (*tail)
        (*tail)->next = specifier;
    else
        *head = specifier;
    *tail = specifier;
}

// Construct the single specifier owner shared by all init-declarators in one
// declaration.
DeclarationSpecifiers *new_declaration_specifiers(
    TypeSpecifier *type, unsigned qualifiers, StorageClass storage,
    unsigned function_specifiers, AlignmentSpecifier *alignments,
    const Token *start, const Token *end)
{
    DeclarationSpecifiers *specifiers = xcalloc(1, sizeof(*specifiers));
    specifiers->span = source_span_join(&start->span, &end->span);
    specifiers->type = type;
    specifiers->qualifiers = qualifiers;
    specifiers->storage = storage;
    specifiers->function_specifiers = function_specifiers;
    specifiers->alignments = alignments;
    return specifiers;
}

// Query used by builtin-only constraints while structured semantic types are
// not yet constructed.
TypeKind declaration_specifiers_builtin_type(
    const DeclarationSpecifiers *specifiers)
{
    if (!specifiers || !specifiers->type ||
        specifiers->type->kind != TYPE_SPECIFIER_BUILTIN)
        unreachable();
    return specifiers->type->u.builtin;
}

// ---------------------------------------------------------------------------
// Declarator construction and queries
// ---------------------------------------------------------------------------

// Construct the identifier leaf borrowing its spelling from the retained source.
Declarator *new_identifier_declarator(const Token *name_tok)
{
    Declarator *decl = new_declarator(DECLARATOR_IDENT, NULL, name_tok->span);
    decl->u.ident.name = token_text(name_tok);
    return decl;
}

// Construct one pointer layer. Its direct-declarator is attached only after
// the complete prefix has been parsed, preserving C declarator binding.
Declarator *new_pointer_declarator(const Token *star, unsigned qualifiers)
{
    Declarator *decl = new_declarator(DECLARATOR_POINTER, NULL, star->span);
    decl->u.pointer.qualifiers = qualifiers;
    return decl;
}

// Wrap an optional direct-declarator in one array layer and take ownership of
// both the prior shape and optional bound expression.
Declarator *new_array_declarator(Declarator *inner, Node *size,
                                 unsigned qualifiers, ArrayDeclaratorForm form,
                                 const Token *open,
                                 const Token *close)
{
    SourceSpan span = inner ? source_span_join(&inner->span, &close->span)
                            : source_span_join(&open->span, &close->span);
    Declarator *decl = new_declarator(DECLARATOR_ARRAY, inner, span);
    decl->suffix_span = source_span_join(&open->span, &close->span);
    decl->u.array.size = size;
    decl->u.array.qualifiers = qualifiers;
    decl->u.array.form = form;
    return decl;
}

// Wrap an optional direct-declarator in one function layer and take ownership
// of the parameter-type-list or identifier-list selected by `form`.
Declarator *new_function_declarator(Declarator *inner,
                                    Node *parameter_clause,
                                    FunctionParameterForm form, bool variadic,
                                    const Token *open, const Token *close)
{
    SourceSpan span = inner ? source_span_join(&inner->span, &close->span)
                            : source_span_join(&open->span, &close->span);
    Declarator *decl = new_declarator(DECLARATOR_FUNCTION, inner, span);
    decl->suffix_span = source_span_join(&open->span, &close->span);
    decl->u.function.form = form;
    switch (form) {
    case FUNCTION_PARAMS_PROTOTYPE:
        decl->u.function.clause.prototype.parameters = parameter_clause;
        decl->u.function.clause.prototype.variadic = variadic;
        break;
    case FUNCTION_PARAMS_IDENTIFIER_LIST:
        if (variadic)
            unreachable();
        decl->u.function.clause.old_style.identifiers = parameter_clause;
        break;
    default:
        unreachable();
    }
    return decl;
}

// Extend a completed nested declarator through its owned parentheses.
void parenthesize_declarator(Declarator *declarator, const Token *open,
                             const Token *close)
{
    declarator->span = source_span_join(&open->span, &close->span);
}

// Return the derived layer closest to the identifier. That layer determines
// whether the declared entity itself is a function, pointer or array.
const Declarator *declarator_entity_layer(const Declarator *decl)
{
    const Declarator *parent = NULL;
    while (decl && decl->inner) {
        parent = decl;
        decl = decl->inner;
    }
    return decl && decl->kind == DECLARATOR_IDENT ? parent : NULL;
}

// Return the function layer for declarations whose declared entity is a
// function; pointer-to-function objects intentionally return NULL.
const Declarator *declarator_function(const Declarator *decl)
{
    const Declarator *entity = declarator_entity_layer(decl);
    return entity && entity->kind == DECLARATOR_FUNCTION ? entity : NULL;
}

// Read the declared identifier from the recursive declarator, or NULL for an
// abstract declarator.
const StringView *declarator_name(const Declarator *decl)
{
    const Declarator *ident = declarator_identifier(decl);
    return ident ? &ident->u.ident.name : NULL;
}

// Return the identifier leaf span used by declarator diagnostics, or NULL for
// an abstract declarator.
const SourceSpan *declarator_name_span(const Declarator *decl)
{
    const Declarator *ident = declarator_identifier(decl);
    return ident ? &ident->span : NULL;
}

// Classify the first recursive-declarator constraint violation without
// diagnostics or mutation; `context` controls parameter-only array forms.
bool find_declarator_constraint(const Declarator *decl,
                                DeclaratorContext context,
                                DeclaratorConstraintError *error)
{
    *error = (DeclaratorConstraintError){ .kind = DECLARATOR_CONSTRAINT_NONE };
    const Declarator *outermost_derived = NULL;
    for (const Declarator *layer = decl; layer; layer = layer->inner) {
        if (layer->kind != DECLARATOR_IDENT)
            outermost_derived = layer;
    }

    for (const Declarator *layer = decl; layer; layer = layer->inner) {
        if (layer->inner && layer->inner->kind == DECLARATOR_FUNCTION) {
            if (layer->kind == DECLARATOR_FUNCTION) {
                *error = (DeclaratorConstraintError){
                    DECLARATOR_FUNCTION_RETURNING_FUNCTION, layer
                };
                return true;
            }
            if (layer->kind == DECLARATOR_ARRAY) {
                *error = (DeclaratorConstraintError){
                    DECLARATOR_FUNCTION_RETURNING_ARRAY, layer
                };
                return true;
            }
        }
        if (layer->inner && layer->kind == DECLARATOR_FUNCTION &&
            layer->inner->kind == DECLARATOR_ARRAY) {
            *error = (DeclaratorConstraintError){
                DECLARATOR_ARRAY_OF_FUNCTIONS, layer
            };
            return true;
        }
        if (layer->kind != DECLARATOR_ARRAY)
            continue;
        if (layer->u.array.form == ARRAY_DECLARATOR_STAR &&
            context != DECLARATOR_PARAMETER) {
            *error = (DeclaratorConstraintError){
                DECLARATOR_STAR_PROTOTYPE_ONLY, layer
            };
            return true;
        }
        if (layer->u.array.form != ARRAY_DECLARATOR_STATIC &&
            layer->u.array.qualifiers == 0)
            continue;
        if (context != DECLARATOR_PARAMETER) {
            *error = (DeclaratorConstraintError){
                DECLARATOR_ARRAY_PARAMETER_ONLY, layer
            };
            return true;
        }
        if (layer != outermost_derived) {
            *error = (DeclaratorConstraintError){
                DECLARATOR_ARRAY_OUTERMOST_ONLY, layer
            };
            return true;
        }
    }
    return false;
}

// Complete a pointer prefix after direct-declarator parsing. Keeping the two
// halves separate until this boundary preserves parenthesized binding.
Declarator *attach_pointer_prefix(Declarator *prefix, Declarator *direct)
{
    if (!prefix)
        return direct;
    if (!direct)
        return prefix;
    Declarator *tail = prefix;
    while (tail->inner)
        tail = tail->inner;
    tail->inner = direct;
    for (Declarator *node = prefix; node != direct; node = node->inner)
        node->span = source_span_join(&node->span, &direct->span);
    return prefix;
}

// Release every parser-owned part of an unfinished declarator and clear the
// fields so callers may safely run their existing cleanup paths.
void discard_parsed_declarator(ParsedDeclarator *decl)
{
    declarator_free(decl->prefix);
    declarator_free(decl->shape);
    *decl = (ParsedDeclarator){0};
}

// ---------------------------------------------------------------------------
// AST constructors
// ---------------------------------------------------------------------------

// AST constructors build nodes bottom-up, borrow raw source spellings, transfer
// decoded buffers and take ownership of child nodes. node_free() owns teardown.
// `next` is reserved for list chains (program/block, parameters, call arguments):
// expression nodes keep it NULL.

// Construct the valid empty-statement node for a consumed semicolon.
Node *new_empty(SourceSpan span)
{
    return new_node(ND_EMPTY, span);
}

// `tok` must be a decoded TOK_NUMBER or TOK_CHARACTER; the literal metadata is
// copied as-is, without a runtime kind check (caller contract).
Node *new_num(const Token *tok)
{
    Node *node = new_node(ND_NUM, tok->span);
    node->u.num.value = tok->value;
    node->u.num.literal = tok->literal;
    return node;
}

// Transfer a decoded string buffer from its token into the new node. Clearing
// the token leaves token_free() safe while the AST becomes the unique owner.
Node *new_string(Token *tok)
{
    Node *node = new_node(ND_STRING, tok->span);
    node->u.string.value = tok->str;
    node->u.string.length = tok->str_len;
    tok->str = NULL;
    tok->str_len = 0;
    return node;
}

// Borrow the identifier spelling from the source buffer retained by the AST.
Node *new_identifier(const Token *name_tok)
{
    Node *node = new_node(ND_IDENTIFIER, name_tok->span);
    node->u.identifier.name = token_text(name_tok);
    return node;
}

// Preserve redundant source parentheses without introducing a semantic AST
// node; callers have already established that `expr` is complete and owned.
void parenthesize_expression(Node *expr, const Token *open,
                             const Token *close)
{
    expr->span = source_span_join(&open->span, &close->span);
}

// Construct one init-declarator and take ownership of its recursive declarator
// and optional initializer.
Node *new_init_declarator(Node *init, Declarator *declarator)
{
    if (!declarator)
        unreachable();
    SourceSpan span = declarator->span;
    if (init)
        span = source_span_join(&span, &init->span);
    Node *node = new_node(ND_INIT_DECL, span);
    node->u.init_decl.declarator = declarator;
    node->u.init_decl.init = init;
    return node;
}

// Construct one C declaration and take ownership of its declaration-specifiers
// and complete init-declarator chain.
Node *new_declaration(DeclarationSpecifiers *specifiers, Node *declarators,
                      const Token *semi)
{
    Node *node = new_node(ND_DECL,
                          source_span_join(&specifiers->span, &semi->span));
    node->u.decl.specifiers = specifiers;
    node->u.decl.declarators = declarators;
    return node;
}

// Construct one member declaration and take ownership of its specifiers and
// optional struct-declarator chain.
Node *new_struct_declaration(DeclarationSpecifiers *specifiers,
                             Node *declarators, const Token *semi)
{
    Node *node = new_node(ND_STRUCT_DECL,
                          source_span_join(&specifiers->span, &semi->span));
    node->u.struct_decl.specifiers = specifiers;
    node->u.struct_decl.declarators = declarators;
    return node;
}

// Construct one member declarator, including unnamed bit-fields.
Node *new_struct_declarator(Declarator *declarator, Node *width)
{
    if (!declarator && !width)
        unreachable();
    const SourceSpan *start = declarator ? &declarator->span : &width->span;
    const SourceSpan *end = width ? &width->span : &declarator->span;
    Node *node = new_node(ND_STRUCT_DECLARATOR, source_span_join(start, end));
    node->u.struct_declarator.declarator = declarator;
    node->u.struct_declarator.width = width;
    return node;
}

// Construct one enumeration constant with its optional explicit value.
Node *new_enumerator(const Token *name, Node *value)
{
    SourceSpan span = value ? source_span_join(&name->span, &value->span)
                            : name->span;
    Node *node = new_node(ND_ENUMERATOR, span);
    node->u.enumerator.name = token_text(name);
    node->u.enumerator.value = value;
    return node;
}

// Construct a static assertion and retain both its constant expression and
// decoded message literal.
Node *new_static_assertion(const Token *keyword, Node *expression,
                           Node *message, const Token *semi)
{
    Node *node = new_node(ND_STATIC_ASSERT,
                          source_span_join(&keyword->span, &semi->span));
    node->u.static_assertion.expression = expression;
    node->u.static_assertion.message = message;
    return node;
}

// Construct a parameter declaration and transfer its optional recursive
// declarator out of parser-owned state.
Node *new_parameter(ParsedDeclarationSpecifiers *specs, ParsedDeclarator *decl)
{
    Declarator *shape = decl ? decl->shape : NULL;
    SourceSpan span = specs->ast->span;
    if (shape)
        span = source_span_join(&span, &shape->span);
    Node *param = new_node(ND_PARAMETER, span);
    param->u.parameter.specifiers = specs->ast;
    param->u.parameter.declarator = shape;
    specs->ast = NULL;
    if (decl)
        decl->shape = NULL;
    return param;
}

// Query whether one completed parameter is the unadorned `void` form whose
// special list meaning is validated outside AST construction.
bool parameter_is_plain_void(const Node *parameter)
{
    return parameter && parameter->type == ND_PARAMETER &&
           parameter->u.parameter.specifiers->type->kind ==
               TYPE_SPECIFIER_BUILTIN &&
           declaration_specifiers_builtin_type(
               parameter->u.parameter.specifiers) == TYPE_VOID &&
           !parameter->u.parameter.declarator;
}

// Start one prefix-unary layer without an operand; attach_prefix_operand()
// completes the caller-owned chain before it becomes a successful expression.
Node *new_unary_shell(Operator op, const Token *op_tok)
{
    Node *node = new_node(ND_UNARY, op_tok->span);
    node->u.unary.op = op;
    return node;
}

// Append one incomplete prefix-unary layer at the caller-owned empty slot and
// return the new layer's empty operand slot for continued construction.
Node **append_unary_prefix(Node **slot, Operator op, const Token *op_tok)
{
    Node *node = new_unary_shell(op, op_tok);
    *slot = node;
    return &node->u.unary.operand;
}

// Construct a complete unary expression and take ownership of its operand.
Node *new_unary(Operator op, const Token *op_tok, Node *operand)
{
    Node *node = new_unary_shell(op, op_tok);
    node->u.unary.operand = operand;
    node->span = source_span_join(&node->span, &operand->span);
    return node;
}

// Complete a unary shell chain at its unique empty operand slot and extend
// every shell span through the transferred operand.
void attach_prefix_operand(Node *root, Node **operand_slot, Node *operand)
{
    *operand_slot = operand;
    for (Node *node = root; node != operand; node = node->u.unary.operand)
        node->span = source_span_join(&node->span, &operand->span);
}

// Construct a binary expression and take ownership of both operands.
Node *new_binary(Operator op, Node *lhs, Node *rhs)
{
    Node *node = new_node(ND_BINARY, source_span_join(&lhs->span, &rhs->span));
    node->u.binary.op = op;
    node->u.binary.lhs = lhs;
    node->u.binary.rhs = rhs;
    return node;
}

// Append one incomplete assignment layer at a caller-owned empty slot and
// return its empty right-operand slot for right-associative construction.
Node **append_assignment_prefix(Node **slot, Operator op, Node *lhs)
{
    Node *node = new_node(ND_ASSIGN, lhs->span);
    node->u.assign.op = op;
    node->u.assign.lhs = lhs;
    *slot = node;
    return &node->u.assign.rhs;
}

// Complete an assignment shell chain and extend every layer through the final
// right operand transferred into its unique empty slot.
void attach_assignment_rhs(Node *root, Node **rhs_slot, Node *rhs)
{
    *rhs_slot = rhs;
    for (Node *node = root; node != rhs; node = node->u.assign.rhs)
        node->span = source_span_join(&node->span, &rhs->span);
}

// Append one incomplete conditional layer at a caller-owned empty slot and
// return its empty else-operand slot for right-associative construction.
Node **append_conditional_prefix(Node **slot, Node *cond, Node *then_expr)
{
    Node *node = new_node(ND_COND, cond->span);
    node->u.cond.cond = cond;
    node->u.cond.then_expr = then_expr;
    *slot = node;
    return &node->u.cond.else_expr;
}

// Complete a conditional shell chain and extend every layer through the final
// else operand transferred into its unique empty slot.
void attach_conditional_else(Node *root, Node **else_slot, Node *else_expr)
{
    *else_slot = else_expr;
    for (Node *node = root; node != else_expr; node = node->u.cond.else_expr)
        node->span = source_span_join(&node->span, &else_expr->span);
}

// Construct a nonempty expression-statement through its terminating semicolon.
Node *new_expr_stmt(Node *expr, const Token *semi)
{
    Node *node = new_node(ND_EXPR_STMT, source_span_join(&expr->span, &semi->span));
    node->u.expr.expr = expr;
    return node;
}

// Construct a return statement and take ownership of its optional expression.
Node *new_return(Node *expr, const Token *keyword, const Token *semi)
{
    Node *node = new_node(ND_RETURN, source_span_join(&keyword->span, &semi->span));
    node->u.expr.expr = expr; // NULL for `return;`
    return node;
}

// Construct a named label borrowing its spelling and owning its statement.
Node *new_label(const Token *name, Node *stmt)
{
    Node *node = new_node(ND_LABEL, source_span_join(&name->span, &stmt->span));
    node->u.label.name = token_text(name);
    node->u.label.stmt = stmt;
    return node;
}

// Construct a case label and take ownership of its value and statement.
Node *new_case(Node *expr, Node *stmt, const Token *keyword)
{
    Node *node = new_node(ND_CASE, source_span_join(&keyword->span, &stmt->span));
    node->u.case_stmt.expr = expr;
    node->u.case_stmt.stmt = stmt;
    return node;
}

// Construct a default label and take ownership of its statement.
Node *new_default(Node *stmt, const Token *keyword)
{
    Node *node = new_node(ND_DEFAULT, source_span_join(&keyword->span, &stmt->span));
    node->u.default_stmt.stmt = stmt;
    return node;
}

// Construct an if statement and take ownership of its condition and branches;
// `else_stmt` may be NULL.
Node *new_if(Node *cond, Node *then_stmt, Node *else_stmt, const Token *if_tok)
{
    Node *last = else_stmt ? else_stmt : then_stmt;
    Node *node = new_node(ND_IF, source_span_join(&if_tok->span, &last->span));
    node->u.if_stmt.cond = cond;
    node->u.if_stmt.then_stmt = then_stmt;
    node->u.if_stmt.else_stmt = else_stmt;
    return node;
}

// Construct a switch statement and take ownership of its condition and body.
Node *new_switch(Node *cond, Node *body, const Token *keyword)
{
    Node *node = new_node(ND_SWITCH, source_span_join(&keyword->span, &body->span));
    node->u.switch_stmt.cond = cond;
    node->u.switch_stmt.body = body;
    return node;
}

// Construct a while statement and take ownership of its condition and body.
Node *new_while(Node *cond, Node *body, const Token *while_tok)
{
    Node *node = new_node(ND_WHILE, source_span_join(&while_tok->span, &body->span));
    node->u.while_stmt.cond = cond;
    node->u.while_stmt.body = body;
    return node;
}

// Construct a do statement through its semicolon and own its body and condition.
Node *new_do_while(Node *body, Node *cond, const Token *do_tok, const Token *semi)
{
    Node *node = new_node(ND_DO_WHILE, source_span_join(&do_tok->span, &semi->span));
    node->u.do_while.body = body;
    node->u.do_while.cond = cond;
    return node;
}

// Construct a for statement and take ownership of its optional control
// expressions, optional declaration and required body.
Node *new_for(Node *init, Node *cond, Node *post, Node *body, const Token *for_tok)
{
    Node *node = new_node(ND_FOR, source_span_join(&for_tok->span, &body->span));
    node->u.for_stmt.init = init;
    node->u.for_stmt.cond = cond;
    node->u.for_stmt.post = post;
    node->u.for_stmt.body = body;
    return node;
}

// Construct the caller-selected break or continue statement variant.
Node *new_jump(NodeType type, const Token *keyword, const Token *semi)
{
    if (type != ND_BREAK && type != ND_CONTINUE)
        unreachable();
    return new_node(type, source_span_join(&keyword->span, &semi->span));
}

// Construct a goto statement borrowing its target spelling.
Node *new_goto(const Token *name, const Token *keyword, const Token *semi)
{
    Node *node = new_node(ND_GOTO, source_span_join(&keyword->span, &semi->span));
    node->u.goto_stmt.name = token_text(name);
    return node;
}

// Construct a postfix call and take ownership of its callee and argument chain.
Node *new_call(Node *callee, Node *args, const Token *close)
{
    Node *node = new_node(ND_CALL, source_span_join(&callee->span, &close->span));
    node->u.call.callee = callee;
    node->u.call.args = args;
    return node;
}

// Construct one postfix subscript and take ownership of its base and index.
Node *new_subscript(Node *base, Node *index, const Token *close)
{
    Node *node = new_node(ND_SUBSCRIPT, source_span_join(&base->span, &close->span));
    node->u.subscript.base = base;
    node->u.subscript.index = index;
    return node;
}

// Construct the caller-selected member-access variant, borrowing the member
// spelling and taking ownership of the completed postfix base.
Node *new_member_access(MemberAccessKind kind, Node *base,
                        const Token *name)
{
    if (kind != MEMBER_DIRECT && kind != MEMBER_INDIRECT)
        unreachable();
    Node *node = new_node(ND_MEMBER, source_span_join(&base->span, &name->span));
    node->u.member_access.kind = kind;
    node->u.member_access.base = base;
    node->u.member_access.name = token_text(name);
    return node;
}

// Construct an owned C11 6.7.7 type-name from its resolved base type and
// optional abstract declarator.
TypeName *new_type_name(TypeSpecifier *type, unsigned qualifiers, Declarator *declarator,
                        const Token *start, const SourceSpan *end)
{
    TypeName *type_name = xcalloc(1, sizeof(*type_name));
    type_name->span = source_span_join(&start->span, end);
    type_name->type = type;
    type_name->qualifiers = qualifiers;
    type_name->declarator = declarator;
    return type_name;
}

// Extend a completed type-name through the parentheses owned by its caller.
void parenthesize_type_name(TypeName *type_name, const Token *open,
                            const Token *close)
{
    type_name->span = source_span_join(&open->span, &close->span);
}

// Release a type-name and its owned abstract declarator; safe with NULL.
void type_name_free(TypeName *type_name)
{
    if (!type_name)
        return;
    type_specifier_free(type_name->type);
    declarator_free(type_name->declarator);
    free(type_name);
}

// Construct a compound literal and take ownership of both its type-name and
// initializer-list node.
Node *new_compound_literal(TypeName *type_name, Node *initializer)
{
    Node *node = new_node(
        ND_COMPOUND_LITERAL,
        source_span_join(&type_name->span, &initializer->span));
    node->u.compound_literal.type_name = type_name;
    node->u.compound_literal.initializer = initializer;
    return node;
}

// Construct one generic association. A NULL type-name represents `default`.
GenericAssociation *new_generic_association(TypeName *type_name,
                                            Node *expression,
                                            const Token *start)
{
    GenericAssociation *association = xcalloc(1, sizeof(*association));
    association->span = source_span_join(&start->span, &expression->span);
    association->type_name = type_name;
    association->expression = expression;
    return association;
}

// Append one complete association to a caller-owned list and transfer its
// ownership while preserving the list's head/tail state.
void append_generic_association(GenericAssociation **head,
                                GenericAssociation **tail,
                                GenericAssociation *association)
{
    if (!association || association->next)
        unreachable();
    if (*tail)
        (*tail)->next = association;
    else
        *head = association;
    *tail = association;
}

// Release a generic-association chain that has not transferred to a generic
// selection node.
void generic_association_free(GenericAssociation *association)
{
    Node *work = NULL;
    release_generic_association_chain(association, &work);
    node_free(work);
}

// Construct a generic selection and take ownership of its controlling
// assignment-expression and association chain.
Node *new_generic_selection(const Token *keyword, Node *control,
                            GenericAssociation *associations,
                            const Token *close)
{
    Node *node = new_node(
        ND_GENERIC_SELECTION,
        source_span_join(&keyword->span, &close->span));
    node->u.generic_selection.control = control;
    node->u.generic_selection.associations = associations;
    return node;
}

// Construct a cast and take ownership of both the explicit type-name and the
// recursively parsed operand.
Node *new_cast(TypeName *type_name, Node *expr)
{
    Node *node = new_node(ND_CAST, source_span_join(&type_name->span, &expr->span));
    node->u.cast.type_name = type_name;
    node->u.cast.expr = expr;
    return node;
}

// Construct sizeof(type-name) or _Alignof(type-name), taking type-name
// ownership and extending the expression span through its closing parenthesis.
Node *new_type_query(NodeType type, const Token *keyword, TypeName *type_name)
{
    if (type != ND_SIZEOF_TYPE && type != ND_ALIGNOF_TYPE)
        unreachable();
    Node *node = new_node(type, source_span_join(&keyword->span, &type_name->span));
    node->u.type_query.type_name = type_name;
    return node;
}

// Construct a braced initializer and take ownership of its ordered item chain.
Node *new_initializer_list(Node *items, const Token *open, const Token *close)
{
    Node *node = new_node(ND_INIT_LIST, source_span_join(&open->span, &close->span));
    node->u.initializer_list.initializers = items;
    return node;
}

// Bind one or more designators to the initializer selected by the designation.
Node *new_designated_initializer(Node *designators, Node *value)
{
    Node *node = new_node(ND_DESIGNATED_INIT,
                          source_span_join(&designators->span, &value->span));
    node->u.designated.designators = designators;
    node->u.designated.value = value;
    return node;
}

// Construct an array designator and take ownership of its constant-expression.
Node *new_index_designator(const Token *open, Node *index, const Token *close)
{
    Node *node = new_node(ND_INDEX_DESIGNATOR, source_span_join(&open->span, &close->span));
    node->u.index_designator.index = index;
    return node;
}

// Construct a member designator borrowing its identifier spelling.
Node *new_member_designator(const Token *dot, const Token *name)
{
    Node *node = new_node(ND_MEMBER_DESIGNATOR, source_span_join(&dot->span, &name->span));
    node->u.member.name = token_text(name);
    return node;
}

// Construct a compound-statement and take ownership of its block-item chain.
Node *new_block(Node *items, const Token *open, const Token *close)
{
    Node *node = new_node(ND_BLOCK, source_span_join(&open->span, &close->span));
    node->u.block.items = items;
    return node;
}

// Construct a translation-unit and take ownership of its external-declaration
// chain; an empty file keeps a zero span.
Node *new_program(Node *items)
{
    SourceSpan span = { 0 };
    if (items) {
        Node *last = items;
        while (last->next)
            last = last->next;
        span = source_span_join(&items->span, &last->span);
    }
    Node *node = new_node(ND_PROGRAM, span);
    node->u.program.external_declarations = items;
    return node;
}

// Append a node chain while preserving the caller's head/tail list state.
void append_node_list(Node **head, Node **tail, Node *chain)
{
    if (!chain)
        return;
    if (*tail)
        (*tail)->next = chain;
    else
        *head = chain;
    while (chain->next)
        chain = chain->next;
    *tail = chain;
}

// Construct a function-definition and take ownership of its declarator,
// optional declaration-list and required compound body; constraints decide
// whether that list is valid for the retained parameter form.
Node *new_function(DeclarationSpecifiers *specifiers,
                   Declarator *declarator, Node *declarations, Node *body)
{
    Node *node = new_node(
        ND_FUNCTION, source_span_join(&specifiers->span, &body->span));
    node->u.func.specifiers = specifiers;
    node->u.func.declarator = declarator;
    node->u.func.declarations = declarations;
    node->u.func.body = body;
    return node;
}

// Release a complete or partial Node chain iteratively; NULL is accepted.
void node_free(Node *node)
{
    Node *work = NULL;
    push_node_chain(&work, node);
    while (work) {
        Node *current = work;
        work = current->next;
        current->next = NULL;
        release_node_contents(current, &work);
        free(current);
    }
}
