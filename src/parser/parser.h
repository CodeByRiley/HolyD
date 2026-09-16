#ifndef HOLYD_PARSER_H
#define HOLYD_PARSER_H

#include "../lexer/lexer.h"
#include "../ast/ast.h"

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    Token next;
    int had_error;      // Set by any diagnostic; ParseProgram fails on it.
} Parser;

void ParserInit(Parser* parser, const char* source);
ASTNode* ParseProgram(Parser* parser);

#endif
