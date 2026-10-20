// The recursive-descent parser replaces this stub in the next phase; keeping
// the stub means the token contract stays frozen while the AST is designed.

#include "frontend/parser.h"
#include "util/common.h"

#include <stdbool.h>

static __attribute__((unused)) bool consume(Token **current, TokenType type)
{
    Token *tok = *current;
    if (tok->type != type) {
        return false;
    }
    *current = tok->next;
    return true;
}

static __attribute__((unused)) void expect(Token **current, TokenType type)
{
    if (!consume(current, type)) {
        unreachable();
    }
}

static __attribute__((unused)) bool consume_punct(Token **current, const char *punct)
{
    (void)current;
    (void)punct;
    return false;
}

static __attribute__((unused)) void expect_punct(Token **current, const char *punct)
{
    (void)current;
    (void)punct;
}

static __attribute__((unused)) Node *new_node(NodeType type)
{
    (void)type;
    return NULL;
}

static __attribute__((unused)) Node *new_num(int64_t value)
{
    (void)value;
    return NULL;
}

static __attribute__((unused)) Node *parse_number(Token **current)
{
    (void)current;
    return NULL;
}

static __attribute__((unused)) Node *parse_expression(Token **current)
{
    (void)current;
    return NULL;
}

static __attribute__((unused)) Node *parse_statement(Token **current)
{
    (void)current;
    return NULL;
}

static __attribute__((unused)) Node *parse_function(Token **current)
{
    (void)current;
    return NULL;
}

Node *parse(Token *tokens)
{
    (void)tokens;
    return NULL;
}
