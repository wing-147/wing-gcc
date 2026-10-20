#ifndef AST_H
#define AST_H

#include <stdint.h>

typedef enum {
    ND_RETURN,
    ND_NUM,
    ND_BLOCK,
    ND_FUNCTION,
} NodeType;

typedef struct Node Node;

struct Node {
    NodeType type;

    // Binary expression
    Node *lhs;
    Node *rhs;

    // Statement list / block
    Node *body;

    // Function name
    char *name;

    // Numeric literal
    int64_t val;
};

#endif // AST_H
