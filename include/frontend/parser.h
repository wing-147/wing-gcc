#ifndef PARSER_H
#define PARSER_H

#include "frontend/ast.h"
#include "frontend/lexer.h"

Node *parse(Token *tokens);

#endif // PARSER_H
