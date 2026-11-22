// C11 Annex A.2.3 statement grammar. Selection, iteration, jump and labeled
// statements delegate expressions and declarations to their owning modules.

#include "parser_internal.h"

#include "util/common.h"

typedef bool (*ForInitParseFn)(ParserCursor *cursor, const Token *keyword,
                               Node **out);

typedef struct {
    PARSER_ALTERNATIVE_FIELDS(ParserStartsFn, ForInitParseFn);
} ForInitAlternative;

// ---------------------------------------------------------------------------
// Static statement productions (C11 Annex A.2.3)
// ---------------------------------------------------------------------------

// Pure FIRST-set query owned by expression-statement. A semicolon selects its
// empty alternative; every other start is delegated to expression.
static bool starts_expression_statement(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_SEMICOLON) ||
           starts_assignment_expression(cursor);
}

// Pure FIRST-set query owned by labeled-statement.
static bool starts_labeled_statement(const ParserCursor *cursor);

// Pure FIRST-set query owned by compound-statement.
static bool starts_compound_statement(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_LBRACE);
}

// Pure FIRST-set query owned by selection-statement.
static bool starts_selection_statement(const ParserCursor *cursor);

// Pure FIRST-set query owned by iteration-statement.
static bool starts_iteration_statement(const ParserCursor *cursor);

// Pure FIRST-set query owned by jump-statement.
static bool starts_jump_statement(const ParserCursor *cursor);

// Pure FIRST-set query owned by statement and composed only from its six direct
// grammar children.
static bool starts_statement(const ParserCursor *cursor);

// Pure FIRST-set query owned by block-item-list and delegated to its required
// first block-item.
static bool starts_block_item_list(const ParserCursor *cursor)
{
    return starts_block_item(cursor);
}

// (6.8.3) expression-statement:
//     expressionopt ;
// A bare `;` is the empty statement and must not be read as a missing expression.
static Node *parse_expression_statement(ParserCursor *cursor)
{
    Token *semi = current_token(cursor);
    if (consume_if(cursor, TOK_SEMICOLON))
        return new_empty(semi->span);

    Node *expr = parse_expression(cursor);
    if (!expr)
        return NULL;
    Token *expr_semi = expect(cursor, TOK_SEMICOLON);
    if (!expr_semi) {
        node_free(expr);
        return NULL;
    }
    return new_expr_stmt(expr, expr_semi);
}

// ---------------------------------------------------------------------------
// block-item private region
// ---------------------------------------------------------------------------

// Branch of (6.8.2) block-item:
//     declaration
// Owned only by parse_block_item(); invalid nested definitions are recovered
// by declaration.c without manufacturing a statement node.
static bool parse_block_declaration(ParserCursor *cursor, Node **out)
{
    return parse_declaration(cursor, DECLARATION_BLOCK, NULL, out);
}

// Branch of (6.8.2) block-item:
//     statement
// Owned only by parse_block_item(); this adapter presents the statement child
// through the block-item alternative protocol.
static bool parse_block_statement(ParserCursor *cursor, Node **out)
{
    *out = parse_statement(cursor);
    return *out != NULL;
}

static const NodeOutAlternative g_block_item_alternatives[] = {
    { starts_declaration, parse_block_declaration },
    { starts_statement, parse_block_statement },
};

// (6.8.2) block-item:
//     declaration
//     statement
static bool parse_block_item(ParserCursor *cursor, Node **out)
{
    *out = NULL;
    for (size_t i = 0; i < ARRAY_LEN(g_block_item_alternatives); i++) {
        if (g_block_item_alternatives[i].starts(cursor))
            return g_block_item_alternatives[i].parse(cursor, out);
    }
    unreachable();
}

// (6.8.2) block-item-list:
//     block-item
//     block-item-list block-item
// Parse one or more items into `next`. The parent owns optionality and the
// closing brace; `last_start` only records the item in progress for an
// unclosed-block diagnostic.
static Node *parse_block_item_list(ParserCursor *cursor, const Token **last_start)
{
    Node *head = NULL;
    Node *tail = NULL;
    *last_start = NULL;

    do {
        ParserCursor item_start = *cursor;
        *last_start = current_token(cursor);
        Node *item = NULL;
        if (!parse_block_item(cursor, &item)) {
            handle_parser_error_block_item(cursor, &item_start);
        } else if (item)
            append_node_list(&head, &tail, item);
        skip_directives(cursor);
    } while (starts_block_item(cursor));
    return head;
}

// ---------------------------------------------------------------------------
// selection-statement private region
// ---------------------------------------------------------------------------

// Branches of (6.8.4) selection-statement:
//     if ( expression ) statement
//     if ( expression ) statement else statement
// Owned only by parse_selection_statement(); they are split out because this
// branch owns both the optional else arm and their shared cleanup.
static bool starts_selection_if(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_IF);
}

// Branches of (6.8.4) selection-statement:
//     if ( expression ) statement
//     if ( expression ) statement else statement
// Owned only by parse_selection_statement(); this branch owns the condition
// and both possible arms.
static Node *parse_selection_if(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    if (!expect(cursor, TOK_LPAREN))
        return NULL;
    Node *cond = parse_expression(cursor);
    if (!cond)
        return NULL;
    if (!expect(cursor, TOK_RPAREN)) {
        node_free(cond);
        return NULL;
    }
    Node *then_stmt = parse_statement(cursor);
    if (!then_stmt) {
        node_free(cond);
        return NULL;
    }
    Node *else_stmt = NULL;
    if (consume_if(cursor, TOK_KW_ELSE)) {
        else_stmt = parse_statement(cursor);
        if (!else_stmt) {
            node_free(cond);
            node_free(then_stmt);
            return NULL;
        }
    }
    return new_if(cond, then_stmt, else_stmt, keyword);
}

// Branch of (6.8.4) selection-statement:
//     switch ( expression ) statement
// Owned only by parse_selection_statement(); it keeps the switch-specific
// expression and body ownership separate from the if branch.
static bool starts_selection_switch(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_SWITCH);
}

// Branch of (6.8.4) selection-statement:
//     switch ( expression ) statement
// Owned only by parse_selection_statement(); this branch owns the switch
// condition and body.
static Node *parse_selection_switch(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    if (!expect(cursor, TOK_LPAREN))
        return NULL;
    Node *cond = parse_expression(cursor);
    if (!cond)
        return NULL;
    if (!expect(cursor, TOK_RPAREN)) {
        node_free(cond);
        return NULL;
    }
    Node *body = parse_statement(cursor);
    if (!body) {
        node_free(cond);
        return NULL;
    }
    return new_switch(cond, body, keyword);
}

static const NodeAlternative g_selection_alternatives[] = {
    { starts_selection_if, parse_selection_if },
    { starts_selection_switch, parse_selection_switch },
};

// Select selection-statement through its sole alternative inventory.
static bool starts_selection_statement(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_selection_alternatives); i++) {
        if (g_selection_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.8.4) selection-statement:
//     if ( expression ) statement
//     if ( expression ) statement else statement
//     switch ( expression ) statement
// Each keyword branch owns its complete grammar and cleanup; this dispatcher
// only selects and delegates.
static Node *parse_selection_statement(ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_selection_alternatives); i++) {
        if (g_selection_alternatives[i].starts(cursor))
            return g_selection_alternatives[i].parse(cursor);
    }
    unreachable();
}

// ---------------------------------------------------------------------------
// iteration-statement private region
// ---------------------------------------------------------------------------

// Branch of (6.8.5) iteration-statement:
//     do statement while ( expression ) ;
// Owned only by parse_iteration_statement(); the keyword is already committed
// before this branch parses the trailing while clause.
static bool starts_iteration_do(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_DO);
}

// Branch of (6.8.5) iteration-statement:
//     do statement while ( expression ) ;
// Owned only by parse_iteration_statement(); this branch owns the body and
// trailing while clause.
static Node *parse_iteration_do(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    Node *body = parse_statement(cursor);
    if (!body)
        return NULL;
    if (!expect(cursor, TOK_KW_WHILE)) {
        node_free(body);
        return NULL;
    }
    if (!expect(cursor, TOK_LPAREN)) {
        node_free(body);
        return NULL;
    }
    Node *cond = parse_expression(cursor);
    if (!cond) {
        node_free(body);
        return NULL;
    }
    if (!expect(cursor, TOK_RPAREN)) {
        node_free(body);
        node_free(cond);
        return NULL;
    }
    Token *semi = expect(cursor, TOK_SEMICOLON);
    if (!semi) {
        node_free(body);
        node_free(cond);
        return NULL;
    }
    return new_do_while(body, cond, keyword, semi);
}

// Branch of (6.8.5) iteration-statement:
//     while ( expression ) statement
// Owned only by parse_iteration_statement().
static bool starts_iteration_while(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_WHILE);
}

// Branch of (6.8.5) iteration-statement:
//     while ( expression ) statement
// Owned only by parse_iteration_statement(); this branch owns its condition
// and body.
static Node *parse_iteration_while(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    if (!expect(cursor, TOK_LPAREN))
        return NULL;
    Node *cond = parse_expression(cursor);
    if (!cond)
        return NULL;
    if (!expect(cursor, TOK_RPAREN)) {
        node_free(cond);
        return NULL;
    }
    Node *body = parse_statement(cursor);
    if (!body) {
        node_free(cond);
        return NULL;
    }
    return new_while(cond, body, keyword);
}

// Branches of (6.8.5) iteration-statement:
//     for ( expressionopt ; expressionopt ; expressionopt ) statement
//     for ( declaration expressionopt ; expressionopt ) statement
// Owned only by parse_iteration_statement(); they are split out because the
// two for-init alternatives share the remaining loop clauses.
static bool starts_iteration_for(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_FOR);
}

// Select the empty expressionopt for-init alternative.
static bool starts_empty_for_init(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_SEMICOLON);
}

// Branch of (6.8.5) iteration-statement:
//     for ( expressionopt ; expressionopt ; expressionopt ) statement
// Owned only by parse_iteration_for(); this adapter realizes the first empty
// expressionopt and consumes its delimiter.
static bool parse_empty_for_init(ParserCursor *cursor, const Token *keyword,
                                 Node **out)
{
    (void)keyword;
    consume(cursor);
    *out = NULL;
    return true;
}

// Branch of (6.8.5) iteration-statement:
//     for ( declaration expressionopt ; expressionopt ) statement
// Owned only by parse_iteration_for(); this adapter supplies loop context to
// the declaration child.
static bool parse_declaration_for_init(ParserCursor *cursor,
                                       const Token *keyword, Node **out)
{
    return parse_declaration(cursor, DECLARATION_FOR, keyword, out);
}

// Branch of (6.8.5) iteration-statement:
//     for ( expressionopt ; expressionopt ; expressionopt ) statement
// Owned only by parse_iteration_for(); this adapter parses a nonempty first
// expressionopt and its delimiter.
static bool parse_expression_for_init(ParserCursor *cursor,
                                      const Token *keyword, Node **out)
{
    (void)keyword;
    *out = parse_expression(cursor);
    if (!*out)
        return false;
    if (expect(cursor, TOK_SEMICOLON))
        return true;
    node_free(*out);
    *out = NULL;
    return false;
}

static const ForInitAlternative g_for_init_alternatives[] = {
    { starts_empty_for_init, parse_empty_for_init },
    { starts_declaration, parse_declaration_for_init },
    { starts_assignment_expression, parse_expression_for_init },
};

// Branches of (6.8.5) iteration-statement:
//     for ( expressionopt ; expressionopt ; expressionopt ) statement
//     for ( declaration expressionopt ; expressionopt ) statement
// Owned only by parse_iteration_statement(); this branch owns the common loop
// clauses and balances their declaration scope.
static Node *parse_iteration_for(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    if (!expect(cursor, TOK_LPAREN))
        return NULL;
    parser_push_scope(cursor);
    Node *init = NULL;
    const ForInitAlternative *init_alternative = NULL;
    for (size_t i = 0; i < ARRAY_LEN(g_for_init_alternatives); i++) {
        if (g_for_init_alternatives[i].starts(cursor)) {
            init_alternative = &g_for_init_alternatives[i];
            break;
        }
    }
    if (!init_alternative) {
        handle_parser_error_expected_expression(current_token(cursor));
        parser_pop_scope(cursor);
        return NULL;
    }
    if (!init_alternative->parse(cursor, keyword, &init)) {
        parser_pop_scope(cursor);
        return NULL;
    }

    Node *cond = NULL;
    if (!token_is(current_token(cursor), TOK_SEMICOLON)) {
        cond = parse_expression(cursor);
        if (!cond) {
            node_free(init);
            parser_pop_scope(cursor);
            return NULL;
        }
    }
    if (!expect(cursor, TOK_SEMICOLON)) {
        node_free(init);
        node_free(cond);
        parser_pop_scope(cursor);
        return NULL;
    }

    Node *post = NULL;
    if (!token_is(current_token(cursor), TOK_RPAREN)) {
        post = parse_expression(cursor);
        if (!post) {
            node_free(init);
            node_free(cond);
            parser_pop_scope(cursor);
            return NULL;
        }
    }
    if (!expect(cursor, TOK_RPAREN)) {
        node_free(init);
        node_free(cond);
        node_free(post);
        parser_pop_scope(cursor);
        return NULL;
    }

    Node *body = parse_statement(cursor);
    if (!body) {
        node_free(init);
        node_free(cond);
        node_free(post);
        parser_pop_scope(cursor);
        return NULL;
    }
    Node *loop = new_for(init, cond, post, body, keyword);
    parser_pop_scope(cursor);
    return loop;
}

static const NodeAlternative g_iteration_alternatives[] = {
    { starts_iteration_do, parse_iteration_do },
    { starts_iteration_while, parse_iteration_while },
    { starts_iteration_for, parse_iteration_for },
};

// Select iteration-statement through its sole alternative inventory.
static bool starts_iteration_statement(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_iteration_alternatives); i++) {
        if (g_iteration_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.8.5) iteration-statement:
//     while ( expression ) statement
//     do statement while ( expression ) ;
//     for ( expressionopt ; expressionopt ; expressionopt ) statement
//     for ( declaration expressionopt ; expressionopt ) statement
// Select one alternative, then delegate its complete grammar and ownership
// handling to the corresponding branch parser.
static Node *parse_iteration_statement(ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_iteration_alternatives); i++) {
        if (g_iteration_alternatives[i].starts(cursor))
            return g_iteration_alternatives[i].parse(cursor);
    }
    unreachable();
}

// ---------------------------------------------------------------------------
// jump-statement private region
// ---------------------------------------------------------------------------

// Branch of (6.8.6) jump-statement:
//     goto identifier ;
// Owned only by parse_jump_statement(); it is split so the identifier operand
// and semicolon remain one owned alternative.
static bool starts_jump_goto(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_GOTO);
}

// Branch of (6.8.6) jump-statement:
//     goto identifier ;
// Owned only by parse_jump_statement(); this branch owns the label operand and
// semicolon.
static Node *parse_jump_goto(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    const Token *name = expect_identifier(cursor);
    if (!name)
        return NULL;
    const Token *semi = expect(cursor, TOK_SEMICOLON);
    if (!semi)
        return NULL;
    return new_goto(name, keyword, semi);
}

// Branch of (6.8.6) jump-statement:
//     continue ;
// Owned only by parse_jump_statement(); it is a keyword-only alternative with
// its own semicolon extent.
static bool starts_jump_continue(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_CONTINUE);
}

// Branch of (6.8.6) jump-statement:
//     continue ;
// Owned only by parse_jump_statement(); this branch owns its semicolon extent.
static Node *parse_jump_continue(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    Token *semi = expect(cursor, TOK_SEMICOLON);
    if (!semi)
        return NULL;
    return new_jump(ND_CONTINUE, keyword, semi);
}

// Branch of (6.8.6) jump-statement:
//     break ;
// Owned only by parse_jump_statement(); it is a keyword-only alternative with
// its own semicolon extent.
static bool starts_jump_break(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_BREAK);
}

// Branch of (6.8.6) jump-statement:
//     break ;
// Owned only by parse_jump_statement(); this branch owns its semicolon extent.
static Node *parse_jump_break(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    Token *semi = expect(cursor, TOK_SEMICOLON);
    if (!semi)
        return NULL;
    return new_jump(ND_BREAK, keyword, semi);
}

// Branch of (6.8.6) jump-statement:
//     return expressionopt ;
// Owned only by parse_jump_statement(); it owns the optional expression and
// the return-at-EOF recovery boundary.
static bool starts_jump_return(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_RETURN);
}

// Branch of (6.8.6) jump-statement:
//     return expressionopt ;
// Owned only by parse_jump_statement(); this branch owns the optional
// expression and semicolon.
static Node *parse_jump_return(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    if (at_eof(current_token(cursor))) {
        handle_parser_error_return_missing_operand(current_token(cursor),
                                                   keyword);
        return NULL;
    }

    Node *expr = NULL;
    if (!token_is(current_token(cursor), TOK_SEMICOLON)) {
        expr = parse_expression(cursor);
        if (!expr)
            return NULL;
    }
    Token *semi = expect(cursor, TOK_SEMICOLON);
    if (!semi) {
        node_free(expr);
        return NULL;
    }
    return new_return(expr, keyword, semi);
}

static const NodeAlternative g_jump_alternatives[] = {
    { starts_jump_goto, parse_jump_goto },
    { starts_jump_continue, parse_jump_continue },
    { starts_jump_break, parse_jump_break },
    { starts_jump_return, parse_jump_return },
};

// Select jump-statement through its sole alternative inventory.
static bool starts_jump_statement(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_jump_alternatives); i++) {
        if (g_jump_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.8.6) jump-statement:
//     goto identifier ;
//     continue ;
//     break ;
//     return expressionopt ;
static Node *parse_jump_statement(ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_jump_alternatives); i++) {
        if (g_jump_alternatives[i].starts(cursor))
            return g_jump_alternatives[i].parse(cursor);
    }
    unreachable();
}

// ---------------------------------------------------------------------------
// labeled-statement private region
// ---------------------------------------------------------------------------

// Branch of (6.8.1) labeled-statement:
//     identifier : statement
// Owned only by parse_labeled_statement(); prove the following colon before
// consuming an identifier so expression-statements remain viable.
static bool starts_labeled_identifier(const ParserCursor *cursor)
{
    const Token *name = current_token(cursor);
    return token_is(name, TOK_IDENTIFIER) &&
           token_is(peek_token(cursor, 1), TOK_COLON);
}

// Branch of (6.8.1) labeled-statement:
//     identifier : statement
// Owned only by parse_labeled_statement(); this branch owns its nested
// statement after the proven label prefix.
static Node *parse_labeled_identifier(ParserCursor *cursor)
{
    const Token *name = consume(cursor);
    consume(cursor);
    Node *stmt = parse_statement(cursor);
    if (!stmt)
        return NULL;
    return new_label(name, stmt);
}

// Branch of (6.8.1) labeled-statement:
//     case constant-expression : statement
// Owned only by parse_labeled_statement(); it is split because case owns a
// constant-expression and its cleanup.
static bool starts_labeled_case(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_CASE);
}

// Branch of (6.8.1) labeled-statement:
//     case constant-expression : statement
// Owned only by parse_labeled_statement(); this branch owns its expression and
// nested statement.
static Node *parse_labeled_case(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    Node *expr = parse_constant_expression(cursor);
    if (!expr)
        return NULL;
    if (!expect(cursor, TOK_COLON)) {
        node_free(expr);
        return NULL;
    }
    Node *stmt = parse_statement(cursor);
    if (!stmt) {
        node_free(expr);
        return NULL;
    }
    return new_case(expr, stmt, keyword);
}

// Branch of (6.8.1) labeled-statement:
//     default : statement
// Owned only by parse_labeled_statement(); it is the no-expression alternative
// paired with case and identifier labels.
static bool starts_labeled_default(const ParserCursor *cursor)
{
    return token_is(current_token(cursor), TOK_KW_DEFAULT);
}

// Branch of (6.8.1) labeled-statement:
//     default : statement
// Owned only by parse_labeled_statement(); this branch owns its nested
// statement.
static Node *parse_labeled_default(ParserCursor *cursor)
{
    const Token *keyword = consume(cursor);
    if (!expect(cursor, TOK_COLON))
        return NULL;
    Node *stmt = parse_statement(cursor);
    if (!stmt)
        return NULL;
    return new_default(stmt, keyword);
}

static const NodeAlternative g_labeled_alternatives[] = {
    { starts_labeled_identifier, parse_labeled_identifier },
    { starts_labeled_case, parse_labeled_case },
    { starts_labeled_default, parse_labeled_default },
};

// Select labeled-statement through its sole alternative inventory.
static bool starts_labeled_statement(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_labeled_alternatives); i++) {
        if (g_labeled_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.8.1) labeled-statement:
//     identifier : statement
//     case constant-expression : statement
//     default : statement
static Node *parse_labeled_statement(ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_labeled_alternatives); i++) {
        if (g_labeled_alternatives[i].starts(cursor))
            return g_labeled_alternatives[i].parse(cursor);
    }
    unreachable();
}

// ---------------------------------------------------------------------------
// statement dispatch private region
// ---------------------------------------------------------------------------

static const NodeAlternative g_statement_alternatives[] = {
    { starts_labeled_statement, parse_labeled_statement },
    { starts_compound_statement, parse_compound_statement },
    { starts_expression_statement, parse_expression_statement },
    { starts_selection_statement, parse_selection_statement },
    { starts_iteration_statement, parse_iteration_statement },
    { starts_jump_statement, parse_jump_statement },
};

// Select statement through the same inventory used by its parser.
static bool starts_statement(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_statement_alternatives); i++) {
        if (g_statement_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}

// (6.8) statement:
//     labeled-statement
//     compound-statement
//     expression-statement
//     selection-statement
//     iteration-statement
//     jump-statement
// statement (C11 6.8): select only its six grammar children. A stray else is
// handled after every legal alternative declines; it is recovery, not a
// seventh statement production.
Node *parse_statement(ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_statement_alternatives); i++) {
        if (g_statement_alternatives[i].starts(cursor))
            return g_statement_alternatives[i].parse(cursor);
    }
    if (token_is(current_token(cursor), TOK_KW_ELSE)) {
        handle_parser_error_else_without_if(cursor);
        return NULL;
    }
    handle_parser_error_expected_expression(current_token(cursor));
    return NULL;
}

// (6.8.2) compound-statement:
//     { block-item-listopt }
Node *parse_compound_statement(ParserCursor *cursor)
{
    Token *open = expect(cursor, TOK_LBRACE);
    if (!open)
        return NULL;
    parser_push_scope(cursor);

    const Token *last_start = NULL;
    skip_directives(cursor);
    Node *head = NULL;
    Node *tail = NULL;
    while (!at_eof(current_token(cursor)) &&
           !token_is(current_token(cursor), TOK_RBRACE)) {
        if (starts_block_item_list(cursor)) {
            Node *items = parse_block_item_list(cursor, &last_start);
            append_node_list(&head, &tail, items);
            continue;
        }
        last_start = current_token(cursor);
        ParserCursor item_start = *cursor;
        handle_parser_error_expected_block_item(cursor, &item_start);
        skip_directives(cursor);
    }
    if (at_eof(current_token(cursor))) {
        handle_parser_error_unclosed_block(open, last_start);
        node_free(head);
        parser_pop_scope(cursor);
        return NULL;
    }
    Token *close = expect(cursor, TOK_RBRACE);
    if (!close) {
        node_free(head);
        parser_pop_scope(cursor);
        return NULL;
    }
    parser_pop_scope(cursor);
    return new_block(head, open, close);
}

// Pure FIRST-set query owned by block-item. Recovery calls it only when
// synchronizing back to this grammar boundary.
bool starts_block_item(const ParserCursor *cursor)
{
    for (size_t i = 0; i < ARRAY_LEN(g_block_item_alternatives); i++) {
        if (g_block_item_alternatives[i].starts(cursor))
            return true;
    }
    return false;
}
