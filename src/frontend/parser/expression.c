// C11 Annex A.2.1 expression grammar. This module owns precedence and operator
// dispatch; declaration syntax is reached only through parse_type_name().

#include "parser_internal.h"

#include "config.h"
#include "util/common.h"

typedef struct {
    TokenKind kind;
    Operator op;
} OperatorInfo;

typedef struct {
    const OperatorInfo *ops;
    size_t count;
} BinaryLevel;

typedef bool (*NodeSuffixParseFn)(ParserCursor *cursor, Node **out);
typedef bool (*TypeNameAlternativeParseFn)(ParserCursor *cursor,
                                           TypeName **out);
typedef Node *(*KeywordOperandParseFn)(ParserCursor *cursor,
                                       const Token *keyword);

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, NodeSuffixParseFn);
} NodeSuffixAlternative;

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, TypeNameAlternativeParseFn);
} TypeNameAlternative;

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, KeywordOperandParseFn);
} KeywordOperandAlternative;

// ---------------------------------------------------------------------------
// Static data
// ---------------------------------------------------------------------------

static const OperatorInfo g_unary_punct_ops[] = {
    { TOK_PLUS, UNARY_PLUS },
    { TOK_MINUS, UNARY_NEG },
    { TOK_TILDE, UNARY_BITNOT },
    { TOK_BANG, UNARY_LOGNOT },
    { TOK_INCREMENT, UNARY_PRE_INC },
    { TOK_DECREMENT, UNARY_PRE_DEC },
    { TOK_STAR, UNARY_DEREF },
    { TOK_AMPERSAND, UNARY_ADDR },
};

// Postfix operators: one entry per spelling, so a new one only extends this
// table (parse_postfix_expression() walks it after a primary expression).
static const OperatorInfo g_postfix_ops[] = {
    { TOK_INCREMENT, UNARY_POST_INC },
    { TOK_DECREMENT, UNARY_POST_DEC },
};

// Right-associative: kept as a table only to remove the 11-way if/else chain.
static const OperatorInfo g_assign_ops[] = {
    { TOK_ASSIGN, ASSIGN },
    { TOK_ADD_ASSIGN, ADD_ASSIGN },
    { TOK_SUB_ASSIGN, SUB_ASSIGN },
    { TOK_MUL_ASSIGN, MUL_ASSIGN },
    { TOK_DIV_ASSIGN, DIV_ASSIGN },
    { TOK_MOD_ASSIGN, MOD_ASSIGN },
    { TOK_SHIFT_LEFT_ASSIGN, SHL_ASSIGN },
    { TOK_SHIFT_RIGHT_ASSIGN, SHR_ASSIGN },
    { TOK_AND_ASSIGN, AND_ASSIGN },
    { TOK_XOR_ASSIGN, XOR_ASSIGN },
    { TOK_OR_ASSIGN, OR_ASSIGN },
};

static const OperatorInfo g_logical_or_ops[] = { { TOK_LOGICAL_OR, BINARY_LOGOR } };
static const OperatorInfo g_logical_and_ops[] = { { TOK_LOGICAL_AND, BINARY_LOGAND } };
static const OperatorInfo g_bitwise_or_ops[] = { { TOK_PIPE, BINARY_BITOR } };
static const OperatorInfo g_bitwise_xor_ops[] = { { TOK_CARET, BINARY_BITXOR } };
static const OperatorInfo g_bitwise_and_ops[] = { { TOK_AMPERSAND, BINARY_BITAND } };
static const OperatorInfo g_equality_ops[] = {
    { TOK_EQUAL_EQUAL, BINARY_EQ }, { TOK_NOT_EQUAL, BINARY_NE }
};
static const OperatorInfo g_relational_ops[] = {
    { TOK_LESS, BINARY_LT },
    { TOK_LESS_EQUAL, BINARY_LE },
    { TOK_GREATER, BINARY_GT },
    { TOK_GREATER_EQUAL, BINARY_GE },
};
static const OperatorInfo g_shift_ops[] = {
    { TOK_SHIFT_LEFT, BINARY_SHL }, { TOK_SHIFT_RIGHT, BINARY_SHR }
};
static const OperatorInfo g_additive_ops[] = {
    { TOK_PLUS, BINARY_ADD }, { TOK_MINUS, BINARY_SUB }
};
static const OperatorInfo g_multiplicative_ops[] = {
    { TOK_STAR, BINARY_MUL },
    { TOK_SLASH, BINARY_DIV },
    { TOK_PERCENT, BINARY_REM },
};

// Precedence ladder, low to high: C11 6.5.14 down to 6.5.5, all
// left-associative.
static const BinaryLevel g_binary_levels[] = {
    { g_logical_or_ops, ARRAY_LEN(g_logical_or_ops) },
    { g_logical_and_ops, ARRAY_LEN(g_logical_and_ops) },
    { g_bitwise_or_ops, ARRAY_LEN(g_bitwise_or_ops) },
    { g_bitwise_xor_ops, ARRAY_LEN(g_bitwise_xor_ops) },
    { g_bitwise_and_ops, ARRAY_LEN(g_bitwise_and_ops) },
    { g_equality_ops, ARRAY_LEN(g_equality_ops) },
    { g_relational_ops, ARRAY_LEN(g_relational_ops) },
    { g_shift_ops, ARRAY_LEN(g_shift_ops) },
    { g_additive_ops, ARRAY_LEN(g_additive_ops) },
    { g_multiplicative_ops, ARRAY_LEN(g_multiplicative_ops) },
};

static bool parse_generic_assoc_list(ParserCursor *cursor,
                                     GenericAssociation **out);
static bool parse_generic_association(ParserCursor *cursor,
                                      GenericAssociation **out);
static bool starts_unary_expression(const ParserCursor *cursor);
static Node *parse_punctuator_unary_expression(ParserCursor *cursor);

// Look up an operator in one grammar-local exact-terminal table.
static bool find_operator(const Token *tok, const OperatorInfo *ops, size_t count,
                          Operator *out)
{
    for (size_t i = 0; i < count; i++) {
        if (token_is(tok, ops[i].kind)) {
            *out = ops[i].op;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// A.2.1 Expressions (C11 6.5)
// ---------------------------------------------------------------------------

// Pure FIRST-set query owned by generic-selection.
static bool starts_generic_selection(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_GENERIC);
}

// (6.5.1.1) generic-selection:
//     _Generic ( assignment-expression , generic-assoc-list )
static Node *parse_generic_selection(ParserCursor *cursor)
{
    Token *keyword = consume_if(cursor, TOK_KW_GENERIC);
    if (!keyword)
        unreachable();
    if (!expect(cursor, TOK_LPAREN))
        return NULL;
    Node *control = parse_assignment_expression(cursor);
    if (!control)
        return NULL;
    if (!expect(cursor, TOK_COMMA)) {
        node_free(control);
        return NULL;
    }
    GenericAssociation *associations = NULL;
    if (!parse_generic_assoc_list(cursor, &associations)) {
        node_free(control);
        return NULL;
    }
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        node_free(control);
        generic_association_free(associations);
        return NULL;
    }
    validate_generic_selection_constraints(associations);
    return new_generic_selection(keyword, control, associations, close);
}

// (6.5.1.1) generic-assoc-list:
//     generic-association
//     generic-assoc-list , generic-association
static bool parse_generic_assoc_list(ParserCursor *cursor,
                                     GenericAssociation **out)
{
    *out = NULL;
    GenericAssociation *head = NULL;
    GenericAssociation *tail = NULL;
    for (;;) {
        GenericAssociation *association = NULL;
        if (!parse_generic_association(cursor, &association)) {
            generic_association_free(head);
            return false;
        }
        append_generic_association(&head, &tail, association);
        if (!consume_if(cursor, TOK_COMMA))
            break;
    }
    *out = head;
    return true;
}

// ---------------------------------------------------------------------------
// generic-association private region
// ---------------------------------------------------------------------------

// Select the default generic-association head without consuming input.
static bool starts_generic_default(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_DEFAULT);
}

// Branch of (6.5.1.1) generic-association:
//     default : assignment-expression
// Owned only by parse_generic_association(); this adapter selects the no-type
// head before the parent parses the common suffix.
static bool parse_generic_default(ParserCursor *cursor, TypeName **out)
{
    consume(cursor);
    *out = NULL;
    return true;
}

// Branch of (6.5.1.1) generic-association:
//     type-name : assignment-expression
// Owned only by parse_generic_association(); this adapter transfers the
// selected type-name before the parent parses the common suffix.
static bool parse_generic_type_name(ParserCursor *cursor, TypeName **out)
{
    return parse_type_name(cursor, out);
}

static const TypeNameAlternative g_generic_association_alternatives[] = {
    { starts_generic_default, parse_generic_default },
    { starts_type_name, parse_generic_type_name },
};

// (6.5.1.1) generic-association:
//     type-name : assignment-expression
//     default : assignment-expression
static bool parse_generic_association(ParserCursor *cursor,
                                      GenericAssociation **out)
{
    *out = NULL;
    const Token *start = current_token(cursor);
    TypeName *type_name = NULL;
    const TypeNameAlternative *alternative = NULL;
    for (size_t i = 0;
         i < ARRAY_LEN(g_generic_association_alternatives); i++) {
        if (g_generic_association_alternatives[i].starts(cursor)) {
            alternative = &g_generic_association_alternatives[i];
            break;
        }
    }
    if (!alternative) {
        handle_parser_error_expected_syntax(current_token(cursor),
                                            "type name or 'default'");
        return false;
    }
    if (!alternative->parse(cursor, &type_name))
        return false;
    if (!expect(cursor, TOK_COLON)) {
        type_name_free(type_name);
        return false;
    }
    Node *expression = parse_assignment_expression(cursor);
    if (!expression) {
        type_name_free(type_name);
        return false;
    }
    *out = new_generic_association(type_name, expression, start);
    return true;
}

// ---------------------------------------------------------------------------
// primary-expression private region
// ---------------------------------------------------------------------------

// Branches of (6.5.1) primary-expression:
//     constant
//     string-literal
// Owned only by parse_primary_expression(); they share token-to-AST
// construction and are split from the alternative selector.
static bool starts_primary_literal(const ParserCursor *cursor)
{
    const Token *tok = current_token(cursor);
    return token_is(tok, TOK_NUMBER) || token_is(tok, TOK_CHARACTER) ||
           token_is(tok, TOK_STRING);
}

// Branches of (6.5.1) primary-expression:
//     constant
//     string-literal
// Owned only by parse_primary_expression(); both terminals share the same
// token-to-AST construction boundary.
static Node *parse_primary_literal(ParserCursor *cursor)
{
    Token *tok = consume(cursor);
    return token_is(tok, TOK_STRING) ? new_string(tok) : new_num(tok);
}

// Branch of (6.5.1) primary-expression:
//     identifier
// Owned only by parse_primary_expression(); its selector and committed parser
// form one typed table entry.
static bool starts_primary_identifier(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_IDENTIFIER);
}

// Branch of (6.5.1) primary-expression:
//     identifier
// Owned only by parse_primary_expression(); this branch constructs the
// selected identifier node.
static Node *parse_primary_identifier(ParserCursor *cursor)
{
    return new_identifier(consume(cursor));
}

// Branch of (6.5.1) primary-expression:
//     ( expression )
// Owned only by parse_primary_expression(); it is split because it owns
// delimiters and failure cleanup.
static bool starts_parenthesized_expression(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LPAREN);
}

// Branch of (6.5.1) primary-expression:
//     ( expression )
// Owned only by parse_primary_expression(); this branch owns the delimiters
// and nested expression cleanup.
static Node *parse_parenthesized_expression(ParserCursor *cursor)
{
    Token *open = consume(cursor);
    Node *expr = parse_expression(cursor);
    if (!expr)
        return NULL;
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        node_free(expr);
        return NULL;
    }
    parenthesize_expression(expr, open, close);
    return expr;
}

static const NodeAlternative g_primary_alternatives[] = {
    { starts_generic_selection, parse_generic_selection },
    { starts_primary_literal, parse_primary_literal },
    { starts_primary_identifier, parse_primary_identifier },
    { starts_parenthesized_expression, parse_parenthesized_expression },
};

// Pure FIRST-set query owned by primary-expression and composed from its direct
// generic-selection child plus its terminal-led alternatives.
static bool starts_primary_expression(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_primary_alternatives); i++) {
        if (g_primary_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.5.1) primary-expression:
//     identifier
//     constant
//     string-literal
//     ( expression )
//     generic-selection
// Literals, identifiers and parenthesized expressions; calls are handled by
// parse_postfix_expression() (C11 6.5.1). The alternative table is complete
// for C11, so implementing _Generic only changes its registered leaf parser.
static Node *parse_primary_expression(ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_primary_alternatives); i++) {
        if (g_primary_alternatives[i].starts(cursor))
            return g_primary_alternatives[i].parse(cursor);
    }

    handle_parser_error_expected_expression(current_token(cursor));
    return NULL;
}

// (6.5.2) argument-expression-list:
//     assignment-expression
//     argument-expression-list , assignment-expression
// argument-expression-list (C11 6.5.2.2): assignment-expressions chained
// through `next`. The caller owns the parentheses and consumes the ')'; this
// function stops before it.
static bool parse_argument_expression_list(ParserCursor *cursor, Node **args_out)
{
    *args_out = NULL;
    Node *head = NULL;
    Node *tail = NULL;

    for (;;) {
        Node *arg = parse_assignment_expression(cursor);
        if (!arg) {
            node_free(head);
            return false;
        }
        append_node_list(&head, &tail, arg);
        if (!consume_if(cursor, TOK_COMMA))
            break;
    }
    *args_out = head;
    return true;
}

// Return the token following a balanced `( type-name )` common prefix without
// consuming, allocating or diagnosing; NULL means the prefix is absent.
static const Token *token_after_parenthesized_type_name(
    const ParserCursor *cursor)
{
    ParserCursor contents = *cursor;
    if (!consume_if(&contents, TOK_LPAREN) ||
        !starts_type_name(&contents))
        return NULL;
    ParserCursor group = *cursor;
    return skip_balanced_group(&group, TOK_LPAREN, TOK_RPAREN)
               ? current_token(&group)
               : NULL;
}

// ---------------------------------------------------------------------------
// postfix-expression private region
// ---------------------------------------------------------------------------

// Pure selector for a parenthesized type-name prefix. Grammar functions use
// this starts_* boundary instead of inspecting the token-after helper.
static bool starts_parenthesized_type_name(const ParserCursor *cursor)
{
    return token_after_parenthesized_type_name(cursor) != NULL;
}

// Branches of (6.5.2) postfix-expression:
//     ( type-name ) { initializer-list }
//     ( type-name ) { initializer-list , }
// Pure selector owned only by parse_postfix_expression().
static bool starts_postfix_compound_literal(const ParserCursor *cursor)
{
    const Token *after = token_after_parenthesized_type_name(cursor);
    return after && token_is(after, TOK_LBRACE);
}

// Pure FIRST-set query owned by postfix-expression and delegated to its two
// base alternatives; suffixes cannot begin a postfix-expression by themselves.
static bool starts_postfix_expression(const ParserCursor *cursor);

// Branches of (6.5.2) postfix-expression:
//     ( type-name ) { initializer-list }
//     ( type-name ) { initializer-list , }
// Owned only by parse_postfix_expression(); the parent has already selected
// this branch, which owns its type-name, braces and initializer-list node.
static Node *parse_postfix_compound_literal(ParserCursor *cursor)
{
    Token *type_open = consume_if(cursor, TOK_LPAREN);
    TypeName *type_name = NULL;
    if (!parse_type_name(cursor, &type_name))
        return NULL;
    Token *type_close = expect(cursor, TOK_RPAREN);
    if (!type_close) {
        type_name_free(type_name);
        return NULL;
    }
    parenthesize_type_name(type_name, type_open, type_close);
    Token *init_open = expect(cursor, TOK_LBRACE);
    if (!init_open) {
        type_name_free(type_name);
        return NULL;
    }
    Node *items = NULL;
    if (!parse_initializer_list(cursor, &items)) {
        type_name_free(type_name);
        return NULL;
    }
    (void)consume_if(cursor, TOK_COMMA);
    Token *init_close = expect(cursor, TOK_RBRACE);
    if (!init_close) {
        type_name_free(type_name);
        node_free(items);
        return NULL;
    }
    Node *initializer = new_initializer_list(items, init_open, init_close);
    return new_compound_literal(type_name, initializer);
}

static const NodeAlternative g_postfix_base_alternatives[] = {
    { starts_postfix_compound_literal, parse_postfix_compound_literal },
    { starts_primary_expression, parse_primary_expression },
};

// Select a postfix-expression base through its sole alternative inventory.
static bool starts_postfix_expression(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_postfix_base_alternatives); i++) {
        if (g_postfix_base_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// Branch of (6.5.2) postfix-expression:
//     postfix-expression [ expression ]
// Owned only by parse_postfix_expression(); it is split because subscript
// expressions own bracket-specific parsing and cleanup.
static bool starts_postfix_subscript_suffix(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LBRACKET);
}

// Branch of (6.5.2) postfix-expression:
//     postfix-expression [ expression ]
// Owned only by parse_postfix_expression(); this branch replaces the completed
// root after owning its index and closing bracket.
static bool parse_postfix_subscript_suffix(ParserCursor *cursor, Node **out)
{
    consume(cursor);
    Node *index = parse_expression(cursor);
    if (!index)
        return false;
    Token *close = expect(cursor, TOK_RBRACKET);
    if (!close) {
        node_free(index);
        return false;
    }
    *out = new_subscript(*out, index, close);
    return true;
}

// Branch of (6.5.2) postfix-expression:
//     postfix-expression ( argument-expression-listopt )
// Owned only by parse_postfix_expression(); it is split because calls own an
// argument chain and its closing parenthesis.
static bool starts_postfix_call_suffix(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LPAREN);
}

// Branch of (6.5.2) postfix-expression:
//     postfix-expression ( argument-expression-listopt )
// Owned only by parse_postfix_expression(); this branch replaces the completed
// root after owning its argument list and closing parenthesis.
static bool parse_postfix_call_suffix(ParserCursor *cursor, Node **out)
{
    consume(cursor);
    Node *args = NULL;
    if (!token_is(current_token(cursor), TOK_RPAREN) &&
        !parse_argument_expression_list(cursor, &args))
        return false;
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        node_free(args);
        return false;
    }
    *out = new_call(*out, args, close);
    return true;
}

// Branches of (6.5.2) postfix-expression:
//     postfix-expression . identifier
//     postfix-expression -> identifier
// Owned only by parse_postfix_expression(); they are split because member
// access requires an identifier after its operator.
static bool starts_postfix_member_suffix(const ParserCursor *cursor)
{
    const Token *tok = current_token(cursor);
    return token_is(tok, TOK_DOT) || token_is(tok, TOK_ARROW);
}

// Branches of (6.5.2) postfix-expression:
//     postfix-expression . identifier
//     postfix-expression -> identifier
// Owned only by parse_postfix_expression(); this branch replaces the completed
// root after consuming the selected member operator and name.
static bool parse_postfix_member_suffix(ParserCursor *cursor, Node **out)
{
    Token *operator_tok = consume(cursor);
    MemberAccessKind kind = token_is(operator_tok, TOK_DOT)
                                ? MEMBER_DIRECT
                                : MEMBER_INDIRECT;
    Token *name = expect_identifier(cursor);
    if (!name)
        return false;
    *out = new_member_access(kind, *out, name);
    return true;
}

// Branches of (6.5.2) postfix-expression:
//     postfix-expression ++
//     postfix-expression --
// Owned only by parse_postfix_expression(); they are split because postfix
// increment/decrement are table-driven leaf alternatives.
static bool starts_postfix_inc_dec_suffix(const ParserCursor *cursor)
{
    Operator op;
    return find_operator(current_token(cursor), g_postfix_ops,
                         ARRAY_LEN(g_postfix_ops), &op);
}

// Branches of (6.5.2) postfix-expression:
//     postfix-expression ++
//     postfix-expression --
// Owned only by parse_postfix_expression(); this branch replaces the completed
// root with the selected postfix operator.
static bool parse_postfix_inc_dec_suffix(ParserCursor *cursor, Node **out)
{
    Operator op;
    Token *op_tok = consume(cursor);
    if (!find_operator(op_tok, g_postfix_ops, ARRAY_LEN(g_postfix_ops), &op))
        unreachable();
    *out = new_unary(op, op_tok, *out);
    return true;
}

static const NodeSuffixAlternative g_postfix_suffix_alternatives[] = {
    { starts_postfix_subscript_suffix, parse_postfix_subscript_suffix },
    { starts_postfix_call_suffix, parse_postfix_call_suffix },
    { starts_postfix_member_suffix, parse_postfix_member_suffix },
    { starts_postfix_inc_dec_suffix, parse_postfix_inc_dec_suffix },
};

// (6.5.2) postfix-expression:
//     primary-expression
//     postfix-expression [ expression ]
//     postfix-expression ( argument-expression-listopt )
//     postfix-expression . identifier
//     postfix-expression -> identifier
//     postfix-expression ++
//     postfix-expression --
//     ( type-name ) { initializer-list }
//     ( type-name ) { initializer-list , }
// Calls and postfix ++/-- bind tighter than every prefix operator (C11 6.5.2).
// The suffix table already reserves every C11 branch; implementing a missing
// suffix changes only its leaf parser.
static Node *parse_postfix_expression(ParserCursor *cursor)
{
    Node *node = NULL;
    for (size_t i = 0; i < ARRAY_LEN(g_postfix_base_alternatives); i++) {
        if (g_postfix_base_alternatives[i].starts(cursor)) {
            node = g_postfix_base_alternatives[i].parse(cursor);
            break;
        }
    }

    while (node) {
        const NodeSuffixAlternative *alternative = NULL;
        for (size_t i = 0; i < ARRAY_LEN(g_postfix_suffix_alternatives); i++) {
            if (g_postfix_suffix_alternatives[i].starts(cursor)) {
                alternative = &g_postfix_suffix_alternatives[i];
                break;
            }
        }
        if (!alternative)
            break;
        if (!alternative->parse(cursor, &node)) {
                node_free(node);
                return NULL;
        }
    }
    return node;
}

// ---------------------------------------------------------------------------
// unary-expression private region
// ---------------------------------------------------------------------------

// Branch of (6.5.3) unary-expression:
//     sizeof ( type-name )
// Owned only by parse_unary_sizeof(); it is split because the parenthesized
// type-name owns a TypeName rather than an expression operand.
static Node *parse_unary_sizeof_type_name(ParserCursor *cursor,
                                          const Token *keyword)
{
    Token *open = consume_if(cursor, TOK_LPAREN);
    TypeName *type_name = NULL;
    if (!parse_type_name(cursor, &type_name))
        return NULL;
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        type_name_free(type_name);
        return NULL;
    }
    parenthesize_type_name(type_name, open, close);
    return new_type_query(ND_SIZEOF_TYPE, keyword, type_name);
}

// Branch of (6.5.3) unary-expression:
//     sizeof unary-expression
// Owned only by parse_unary_sizeof(); it constructs the expression-operand
// form selected after the keyword.
static Node *parse_unary_sizeof_expression(ParserCursor *cursor,
                                           const Token *keyword)
{
    Node *operand = parse_unary_expression(cursor);
    return operand ? new_unary(UNARY_SIZEOF, keyword, operand) : NULL;
}

static const KeywordOperandAlternative g_sizeof_operand_alternatives[] = {
    { starts_parenthesized_type_name, parse_unary_sizeof_type_name },
    { starts_unary_expression, parse_unary_sizeof_expression },
};

// Branches of (6.5.3) unary-expression:
//     sizeof unary-expression
//     sizeof ( type-name )
// Owned only by parse_unary_expression(); they are split because type-name and
// unary-expression operands have distinct owners.
static Node *parse_unary_sizeof(ParserCursor *cursor, const Token *keyword)
{
    for (size_t i = 0; i < ARRAY_LEN(g_sizeof_operand_alternatives); i++) {
        if (g_sizeof_operand_alternatives[i].starts(cursor)) {
            return g_sizeof_operand_alternatives[i].parse(cursor, keyword);
        }
    }
    handle_parser_error_expected_expression(current_token(cursor));
    return NULL;
}

// Branch of (6.5.3) unary-expression:
//     _Alignof ( type-name )
// Owned only by parse_unary_alignof(); it is split because _Alignof requires a
// parenthesized TypeName and rejects expression operands.
static Node *parse_unary_alignof_type_name(ParserCursor *cursor,
                                           const Token *keyword)
{
    Token *open = consume_if(cursor, TOK_LPAREN);
    TypeName *type_name = NULL;
    if (!parse_type_name(cursor, &type_name))
        return NULL;
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        type_name_free(type_name);
        return NULL;
    }
    parenthesize_type_name(type_name, open, close);
    return new_type_query(ND_ALIGNOF_TYPE, keyword, type_name);
}

// Branch of (6.5.3) unary-expression:
//     _Alignof ( type-name )
// Owned only by parse_unary_expression().
static Node *parse_unary_alignof(ParserCursor *cursor, const Token *keyword)
{
    if (!starts_parenthesized_type_name(cursor)) {
        handle_parser_error_alignof_expression(keyword);
        return NULL;
    }
    return parse_unary_alignof_type_name(cursor, keyword);
}

#if GNU_EXTENSION
// GNU extension branches of unary-expression:
//     __alignof__ unary-expression
//     __alignof__ ( type-name )
// Owned only by parse_unary_gnu_alignof(); these are not N1570 productions;
// this leaf constructs the expression-operand form.
static Node *parse_unary_gnu_alignof_expression(ParserCursor *cursor,
                                               const Token *keyword)
{
    Node *operand = parse_unary_expression(cursor);
    return operand ? new_unary(UNARY_ALIGNOF, keyword, operand) : NULL;
}

static const KeywordOperandAlternative g_gnu_alignof_operand_alternatives[] = {
    { starts_parenthesized_type_name, parse_unary_alignof_type_name },
    { starts_unary_expression, parse_unary_gnu_alignof_expression },
};

// GNU extension branches of unary-expression:
//     __alignof__ unary-expression
//     __alignof__ ( type-name )
// Owned only by parse_unary_expression(); this dispatcher is not an N1570
// production and selects only the GNU operand form.
static Node *parse_unary_gnu_alignof(ParserCursor *cursor,
                                     const Token *keyword)
{
    for (size_t i = 0;
         i < ARRAY_LEN(g_gnu_alignof_operand_alternatives); i++) {
        if (g_gnu_alignof_operand_alternatives[i].starts(cursor)) {
            return g_gnu_alignof_operand_alternatives[i].parse(cursor,
                                                               keyword);
        }
    }
    handle_parser_error_expected_expression(current_token(cursor));
    return NULL;
}
#endif

// Select the committed sizeof branch without consuming parser state.
static bool starts_unary_sizeof(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_SIZEOF);
}

// Branches of (6.5.3) unary-expression:
//     sizeof unary-expression
//     sizeof ( type-name )
// Owned only by parse_keyword_unary_expression(); this adapter consumes the
// selected keyword before delegating its operand.
static Node *parse_unary_sizeof_alternative(ParserCursor *cursor)
{
    return parse_unary_sizeof(cursor, consume(cursor));
}

// Select the committed _Alignof branch without consuming parser state.
static bool starts_unary_alignof(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_ALIGNOF);
}

// Branch of (6.5.3) unary-expression:
//     _Alignof ( type-name )
// Owned only by parse_keyword_unary_expression(); this adapter consumes the
// selected keyword before delegating its operand.
static Node *parse_unary_alignof_alternative(ParserCursor *cursor)
{
    return parse_unary_alignof(cursor, consume(cursor));
}

#if GNU_EXTENSION
// Select the committed GNU __alignof__ branch without consuming parser state.
static bool starts_unary_gnu_alignof(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_ALIGNOF_GNU);
}

// GNU extension branches of unary-expression:
//     __alignof__ unary-expression
//     __alignof__ ( type-name )
// Owned only by parse_keyword_unary_expression(); this adapter is not an N1570
// production and consumes the selected keyword before delegation.
static Node *parse_unary_gnu_alignof_alternative(ParserCursor *cursor)
{
    return parse_unary_gnu_alignof(cursor, consume(cursor));
}
#endif

// Split branches of (6.5.3) unary-expression:
//     sizeof unary-expression
//     sizeof ( type-name )
//     _Alignof ( type-name )
// GNU extension branches:
//     __alignof__ unary-expression
//     __alignof__ ( type-name )
// Owned only by parse_unary_expression(); the table is shared by selection and
// committed parsing so keyword alternatives cannot drift apart.
static const NodeAlternative g_keyword_unary_alternatives[] = {
    { starts_unary_sizeof, parse_unary_sizeof_alternative },
    { starts_unary_alignof, parse_unary_alignof_alternative },
#if GNU_EXTENSION
    { starts_unary_gnu_alignof, parse_unary_gnu_alignof_alternative },
#endif
};

// Branches of (6.5.3) unary-expression:
//     sizeof unary-expression
//     sizeof ( type-name )
//     _Alignof ( type-name )
// Owned only by parse_unary_expression(); GNU builds add their explicitly
// marked extension branches to the same private inventory.
static Node *parse_keyword_unary_expression(ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_keyword_unary_alternatives); i++) {
        if (g_keyword_unary_alternatives[i].starts(cursor))
            return g_keyword_unary_alternatives[i].parse(cursor);
    }
    unreachable();
}

// Pure FIRST-set query for the keyword-led alternatives of unary-expression.
static bool starts_keyword_unary_expression(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_keyword_unary_alternatives); i++) {
        if (g_keyword_unary_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// Pure FIRST-set query owned by unary-expression and composed from its keyword,
// punctuator-prefix and postfix-expression alternatives.
// Select a punctuator-led unary-expression without consuming parser state.
static bool starts_punctuator_unary_expression(const ParserCursor *cursor)
{
    Operator op;
    return find_operator(current_token(cursor), g_unary_punct_ops,
                         ARRAY_LEN(g_unary_punct_ops), &op);
}

static const NodeAlternative g_unary_alternatives[] = {
    { starts_keyword_unary_expression, parse_keyword_unary_expression },
    { starts_punctuator_unary_expression,
      parse_punctuator_unary_expression },
    { starts_postfix_expression, parse_postfix_expression },
};

static bool starts_unary_expression(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_unary_alternatives); i++) {
        if (g_unary_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// Pure selector for the `( type-name ) cast-expression` branch. A following
// `{` reserves the common prefix for postfix-expression's compound literal.
static bool starts_cast_type_name_branch(const ParserCursor *cursor)
{
    const Token *after = token_after_parenthesized_type_name(cursor);
    return after && !token_is(after, TOK_LBRACE);
}

// Skip one postfix-expression only on starts_assignment_unary_branch()'s
// local cursor. Nested expressions stay opaque balanced groups because the
// selector needs only the token immediately following the unary-expression.
static bool skip_postfix_expression(ParserCursor *lookahead)
{
    Token *tok = current_token(lookahead);
    if (token_is(tok, TOK_IDENTIFIER) || token_is(tok, TOK_NUMBER) ||
        token_is(tok, TOK_CHARACTER) || token_is(tok, TOK_STRING)) {
        consume(lookahead);
    } else if (consume_if(lookahead, TOK_KW_GENERIC)) {
        if (!skip_balanced_group(lookahead, TOK_LPAREN, TOK_RPAREN))
            return false;
    } else if (starts_postfix_compound_literal(lookahead)) {
        if (!skip_balanced_group(lookahead, TOK_LPAREN, TOK_RPAREN) ||
            !skip_balanced_group(lookahead, TOK_LBRACE, TOK_RBRACE))
            return false;
    } else if (!skip_balanced_group(lookahead, TOK_LPAREN, TOK_RPAREN)) {
        return false;
    }

    for (;;) {
        if (token_is(current_token(lookahead), TOK_LBRACKET)) {
            if (!skip_balanced_group(lookahead, TOK_LBRACKET, TOK_RBRACKET))
                return false;
            continue;
        }
        if (token_is(current_token(lookahead), TOK_LPAREN)) {
            if (!skip_balanced_group(lookahead, TOK_LPAREN, TOK_RPAREN))
                return false;
            continue;
        }
        if (consume_if(lookahead, TOK_DOT) ||
            consume_if(lookahead, TOK_ARROW)) {
            if (!token_is(current_token(lookahead), TOK_IDENTIFIER))
                return false;
            consume(lookahead);
            continue;
        }
        if (consume_if(lookahead, TOK_INCREMENT) ||
            consume_if(lookahead, TOK_DECREMENT))
            continue;
        return true;
    }
}

// Skip one unary-expression only on starts_assignment_unary_branch()'s local
// cursor. Loops replace input-depth recursion; cast prefixes are admitted only
// after a unary-operator, as required by C11 6.5.3 and 6.5.4.
static bool skip_unary_expression(ParserCursor *lookahead)
{
    bool allows_cast_prefix = false;
    for (;;) {
        if (allows_cast_prefix) {
            while (starts_cast_type_name_branch(lookahead)) {
                if (!skip_balanced_group(lookahead, TOK_LPAREN, TOK_RPAREN))
                    return false;
            }
            allows_cast_prefix = false;
        }

        Operator op;
        Token *tok = current_token(lookahead);
        if (find_operator(tok, g_unary_punct_ops,
                          ARRAY_LEN(g_unary_punct_ops), &op)) {
            consume(lookahead);
            if (op != UNARY_PRE_INC && op != UNARY_PRE_DEC)
                allows_cast_prefix = true;
            continue;
        }

        if (consume_if(lookahead, TOK_KW_SIZEOF)) {
            if (starts_parenthesized_type_name(lookahead))
                return skip_balanced_group(lookahead, TOK_LPAREN, TOK_RPAREN);
            continue;
        }
        if (consume_if(lookahead, TOK_KW_ALIGNOF)) {
            return starts_parenthesized_type_name(lookahead) &&
                   skip_balanced_group(lookahead, TOK_LPAREN, TOK_RPAREN);
        }
#if GNU_EXTENSION
        if (consume_if(lookahead, TOK_KW_ALIGNOF_GNU)) {
            if (starts_parenthesized_type_name(lookahead))
                return skip_balanced_group(lookahead, TOK_LPAREN, TOK_RPAREN);
            continue;
        }
#endif
        return skip_postfix_expression(lookahead);
    }
}

// Pure selector for the `unary-expression assignment-operator` branch of
// assignment-expression. Only a local cursor advances; no AST or diagnostic
// state is created and the caller's cursor remains untouched.
static bool starts_assignment_unary_branch(const ParserCursor *cursor)
{
    ParserCursor lookahead = *cursor;
    if (!skip_unary_expression(&lookahead))
        return false;
    Operator op;
    return find_operator(current_token(&lookahead), g_assign_ops,
                         ARRAY_LEN(g_assign_ops), &op);
}

// Branch of (6.5.4) cast-expression:
//     ( type-name ) cast-expression
// Owned only by parse_cast_expression(); it is split because the parenthesized
// type-name owns different cleanup from unary-expression.
static Node *parse_cast_type_name_branch(ParserCursor *cursor)
{
    Token *open = consume_if(cursor, TOK_LPAREN);
    TypeName *type_name = NULL;
    if (!parse_type_name(cursor, &type_name))
        return NULL;
    Token *close = expect(cursor, TOK_RPAREN);
    if (!close) {
        type_name_free(type_name);
        return NULL;
    }
    parenthesize_type_name(type_name, open, close);
    Node *expr = parse_cast_expression(cursor);
    if (!expr) {
        type_name_free(type_name);
        return NULL;
    }
    return new_cast(type_name, expr);
}

// (6.5.5) multiplicative-expression:
//     cast-expression
//     multiplicative-expression * cast-expression
//     multiplicative-expression / cast-expression
//     multiplicative-expression % cast-expression
// (6.5.6) additive-expression:
//     multiplicative-expression
//     additive-expression + multiplicative-expression
//     additive-expression - multiplicative-expression
// (6.5.7) shift-expression:
//     additive-expression
//     shift-expression << additive-expression
//     shift-expression >> additive-expression
// (6.5.8) relational-expression:
//     shift-expression
//     relational-expression < shift-expression
//     relational-expression > shift-expression
//     relational-expression <= shift-expression
//     relational-expression >= shift-expression
// (6.5.9) equality-expression:
//     relational-expression
//     equality-expression == relational-expression
//     equality-expression != relational-expression
// (6.5.10) AND-expression:
//     equality-expression
//     AND-expression & equality-expression
// (6.5.11) exclusive-OR-expression:
//     AND-expression
//     exclusive-OR-expression ^ AND-expression
// (6.5.12) inclusive-OR-expression:
//     exclusive-OR-expression
//     inclusive-OR-expression | exclusive-OR-expression
// (6.5.13) logical-AND-expression:
//     inclusive-OR-expression
//     logical-AND-expression && inclusive-OR-expression
// (6.5.14) logical-OR-expression:
//     logical-AND-expression
//     logical-OR-expression || logical-AND-expression
// One generic left-associative climb per level. Operands are parsed one level
// higher, so the lowest-precedence level (0) ends up outermost and the table
// reads in increasing precedence; past the last level the ladder falls through
// to parse_cast_expression() (C11 6.5.5–6.5.14). Direct recursion is
// intentional and terminates at the fixed table length, independent of input
// nesting depth.
static Node *parse_binary_level(ParserCursor *cursor, size_t level)
{
    if (level == ARRAY_LEN(g_binary_levels))
        return parse_cast_expression(cursor);

    const BinaryLevel *current = &g_binary_levels[level];
    Node *node = parse_binary_level(cursor, level + 1);

    while (node) {
        Operator op;
        if (!find_operator(current_token(cursor), current->ops, current->count, &op))
            break;
        consume(cursor);
        Node *rhs = parse_binary_level(cursor, level + 1);
        if (!rhs) {
            node_free(node);
            return NULL;
        }
        node = new_binary(op, node, rhs);
    }
    return node;
}

// ---------------------------------------------------------------------------
// Public expression productions
// ---------------------------------------------------------------------------

// Branches of (6.5.3) unary-expression:
//     ++ unary-expression
//     -- unary-expression
//     unary-operator cast-expression
// Owned only by parse_unary_expression(); this branch linearizes consecutive
// punctuator prefixes and attaches their one final operand.
static Node *parse_punctuator_unary_expression(ParserCursor *cursor)
{
    Node *root = NULL;
    Node **operand_slot = &root;
    Operator innermost = UNARY_PLUS;
    for (;;) {
        Operator op;
        Token *op_tok = current_token(cursor);
        if (!find_operator(op_tok, g_unary_punct_ops,
                           ARRAY_LEN(g_unary_punct_ops), &op))
            break;
        consume(cursor);
        operand_slot = append_unary_prefix(operand_slot, op, op_tok);
        innermost = op;
    }

    // The loop has exhausted punctuator prefixes. A ++/-- operand can now only
    // start with a keyword alternative or reduce to postfix-expression; a
    // unary-operator instead takes a cast-expression.
    Node *operand = NULL;
    if (innermost == UNARY_PRE_INC || innermost == UNARY_PRE_DEC) {
        if (starts_keyword_unary_expression(cursor))
            operand = parse_keyword_unary_expression(cursor);
        else
            operand = parse_postfix_expression(cursor);
    } else {
        operand = parse_cast_expression(cursor);
    }
    if (!operand) {
        node_free(root);
        return NULL;
    }
    attach_prefix_operand(root, operand_slot, operand);
    return root;
}

// (6.5.3) unary-expression:
//     postfix-expression
//     ++ unary-expression
//     -- unary-expression
//     unary-operator cast-expression
//     sizeof unary-expression
//     sizeof ( type-name )
//     _Alignof ( type-name )
// (6.5.3) unary-operator: one of
//     & * + - ~ !
Node *parse_unary_expression(ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_unary_alternatives); i++) {
        if (g_unary_alternatives[i].starts(cursor))
            return g_unary_alternatives[i].parse(cursor);
    }
    handle_parser_error_expected_expression(current_token(cursor));
    return NULL;
}

// (6.5.4) cast-expression:
//     unary-expression
//     ( type-name ) cast-expression
// The type-name branch is selected by `(` followed by a specifier or qualifier;
// all other inputs reduce to unary-expression without cursor rewind.
Node *parse_cast_expression(ParserCursor *cursor)
{
    static const NodeAlternative alternatives[] = {
        { starts_cast_type_name_branch, parse_cast_type_name_branch },
        { starts_unary_expression, parse_unary_expression },
    };
    for (size_t i = 0; i < ARRAY_LEN(alternatives); i++) {
        if (alternatives[i].starts(cursor))
            return alternatives[i].parse(cursor);
    }
    handle_parser_error_expected_expression(current_token(cursor));
    return NULL;
}

// (6.5.15) conditional-expression:
//     logical-OR-expression
//     logical-OR-expression ? expression : conditional-expression
// ?: sits between assignment and logical-or; the middle operand is a full
// expression (C11 6.5.15). Incomplete final AST layers retain the grammar's
// right associativity without input-depth recursion.
Node *parse_conditional_expression(ParserCursor *cursor)
{
    Node *root = NULL;
    Node **else_slot = &root;
    for (;;) {
        Node *cond = parse_binary_level(cursor, 0);
        if (!cond) {
            node_free(root);
            return NULL;
        }
        if (!consume_if(cursor, TOK_QUESTION)) {
            if (!root)
                return cond;
            attach_conditional_else(root, else_slot, cond);
            return root;
        }

        Node *then_expr = parse_expression(cursor);
        if (!then_expr) {
            node_free(root);
            node_free(cond);
            return NULL;
        }
        if (!expect(cursor, TOK_COLON)) {
            node_free(root);
            node_free(cond);
            node_free(then_expr);
            return NULL;
        }
        else_slot = append_conditional_prefix(else_slot, cond, then_expr);
    }
}

// (6.5.16) assignment-expression:
//     conditional-expression
//     unary-expression assignment-operator assignment-expression
// (6.5.16) assignment-operator: one of
//     = *= /= %= +=                -=    <<=     >>=       &=    ^=   |=
// assignment-expression is right-associative and sits below ?: (C11 6.5.16).
// A silent starts_* selector commits only complete unary-expression/operator
// prefixes; the final conditional-expression terminates the right-associated
// chain without cursor rewind or input-depth recursion.
Node *parse_assignment_expression(ParserCursor *cursor)
{
    Node *root = NULL;
    Node **rhs_slot = &root;
    while (starts_assignment_unary_branch(cursor)) {
        Node *lhs = parse_unary_expression(cursor);
        if (!lhs) {
            node_free(root);
            return NULL;
        }
        Token *op_tok = current_token(cursor);
        Operator op;
        if (!find_operator(op_tok, g_assign_ops, ARRAY_LEN(g_assign_ops), &op))
            unreachable();
        consume(cursor);
        rhs_slot = append_assignment_prefix(rhs_slot, op, lhs);
    }

    Node *rhs = parse_conditional_expression(cursor);
    if (!rhs) {
        node_free(root);
        return NULL;
    }
    if (!root)
        return rhs;
    attach_assignment_rhs(root, rhs_slot, rhs);
    return root;
}

// (6.5.17) expression:
//     assignment-expression
//     expression , assignment-expression
// Comma is the lowest-precedence, left-associative operator (C11 6.5.17).
Node *parse_expression(ParserCursor *cursor)
{
    Node *node = parse_assignment_expression(cursor);
    while (node && consume_if(cursor, TOK_COMMA)) {
        Node *rhs = parse_assignment_expression(cursor);
        if (!rhs) {
            node_free(node);
            return NULL;
        }
        node = new_binary(BINARY_COMMA, node, rhs);
    }
    return node;
}

// (6.6) constant-expression:
//     conditional-expression
// Its constness is a semantic constraint checked by later compiler phases.
Node *parse_constant_expression(ParserCursor *cursor)
{
    return parse_conditional_expression(cursor);
}

// Pure FIRST-set query for the implemented assignment-expression grammar. It
// lets owning list productions distinguish repetition from a parent suffix
// without parsing, allocating or diagnosing.
bool starts_assignment_expression(const ParserCursor *cursor)
{
    return starts_unary_expression(cursor);
}
