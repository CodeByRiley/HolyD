#include "ast.h"
#include <stdio.h>
#include <stdlib.h>

ASTNode* ASTNewNumber(long long val) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_NUMBER;
    node->as.integer_literal.value = val;
    return node;
}

ASTNode* ASTNewFloat(double val) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_FLOAT;
    node->as.float_literal.value = val;
    return node;
}

ASTNode* ASTNewString(const char* str, int len) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_STRING;
    node->as.string_literal.value = str;
    node->as.string_literal.length = len;
    return node;
}

ASTNode* ASTNewVarRef(const char* name, int len) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_VAR_REF;
    node->as.variable_ref.name = name;
    node->as.variable_ref.name_length = len;
    return node;
}

ASTNode* ASTNewVarDecl(TypeSyntax* type, const char* name, int len, ASTNode* init) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_VAR_DECL;
    node->as.variable_decl.declared_type = type;
    node->as.variable_decl.name = name;
    node->as.variable_decl.name_length = len;
    node->as.variable_decl.initializer = init;
    return node;
}

ASTNode* ASTNewAssign(const char* name, int len, ASTNode* value) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_ASSIGN;
    node->as.assignment.name = name;
    node->as.assignment.name_length = len;
    node->as.assignment.value = value;
    return node;
}

ASTNode* ASTNewBinaryOp(TokenType op, ASTNode* left, ASTNode* right) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_BINARY_OP;
    node->as.binary_op.operator_type = op;
    node->as.binary_op.left = left;
    node->as.binary_op.right = right;
    return node;
}

ASTNode* ASTNewUnaryOp(TokenType op, ASTNode* operand) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_UNARY_OP;
    node->as.unary_op.operator_type = op;
    node->as.unary_op.operand = operand;
    return node;
}

ASTNode* ASTNewIndex(ASTNode* target, ASTNode* index) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_INDEX;
    node->as.index_expr.target = target;
    node->as.index_expr.index = index;
    return node;
}

ASTNode* ASTNewIndexAssign(ASTNode* target, ASTNode* index, ASTNode* value) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_INDEX_ASSIGN;
    node->as.index_assignment.target = target;
    node->as.index_assignment.index = index;
    node->as.index_assignment.value = value;
    return node;
}

ASTNode* ASTNewArrayLenExpr(ASTNode* target) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_ARRAY_LEN_EXPR;
    node->as.array_length_expr.target = target;
    return node;
}

ASTNode* ASTNewCall(const char* name, int len, ASTNode** args, int arg_count) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_CALL;
    node->as.call.callee_name = name;
    node->as.call.callee_name_length = len;
    node->as.call.arguments = args;
    node->as.call.argument_count = arg_count;
    return node;
}

ASTNode* ASTNewBlock(ASTNode** stmts, int count) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_BLOCK;
    node->as.block.statements = stmts;
    node->as.block.statement_count = count;
    return node;
}

ASTNode* ASTNewIf(ASTNode* cond, ASTNode* then_block, ASTNode* else_block) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_IF;
    node->as.if_statement.condition = cond;
    node->as.if_statement.then_branch = then_block;
    node->as.if_statement.else_branch = else_block;
    return node;
}

ASTNode* ASTNewWhile(ASTNode* cond, ASTNode* body) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_WHILE;
    node->as.while_statement.condition = cond;
    node->as.while_statement.body = body;
    return node;
}

ASTNode* ASTNewFor(ASTNode* init, ASTNode* cond, ASTNode* inc, ASTNode* body) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_FOR;
    node->as.for_statement.initializer = init;
    node->as.for_statement.condition = cond;
    node->as.for_statement.increment = inc;
    node->as.for_statement.body = body;
    return node;
}

ASTNode* ASTNewForeach(TypeSyntax* variable_type, const char* var_name,
                       int var_len, TypeSyntax* index_type,
                       const char* index_name, int index_len,
                       ASTNode* array_expr, ASTNode* body) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_FOREACH;
    node->as.foreach_statement.variable_type = variable_type;
    node->as.foreach_statement.variable_name = var_name;
    node->as.foreach_statement.variable_name_length = var_len;
    node->as.foreach_statement.index_type = index_type;
    node->as.foreach_statement.index_name = index_name;
    node->as.foreach_statement.index_name_length = index_len;
    node->as.foreach_statement.array_expression = array_expr;
    node->as.foreach_statement.body = body;
    return node;
}

ASTNode* ASTNewFuncDecl(TypeSyntax* return_type, const char* name, int len,
                        ParameterSyntax* parameters, int parameter_count,
                        ASTNode* body) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_FUNC_DECL;
    node->as.function_decl.return_type = return_type;
    node->as.function_decl.name = name;
    node->as.function_decl.name_length = len;
    node->as.function_decl.parameters = parameters;
    node->as.function_decl.parameter_count = parameter_count;
    node->as.function_decl.body = body;
    return node;
}

ASTNode* ASTNewReturn(ASTNode* expr) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_RETURN;
    node->as.return_statement.expression = expr;
    return node;
}

static void ASTPrintIndent(int indent) {
    for (int i = 0; i < indent; i++) {
        fputs("  ", stdout);
    }
}

static void ASTPrintChild(const char* label, const ASTNode* child, int indent) {
    ASTPrintIndent(indent);
    printf("%s\n", label);
    ASTPrint(child, indent + 1);
}

void ASTPrint(const ASTNode* node, int indent) {
    ASTPrintIndent(indent);
    if (node == NULL) {
        fputs("<null-node>\n", stdout);
        return;
    }

    switch (node->type) {
        case AST_NUMBER:
            printf("INTEGER_LITERAL %lld\n", node->as.integer_literal.value);
            break;

        case AST_FLOAT:
            printf("FLOAT_LITERAL %.17g\n", node->as.float_literal.value);
            break;

        case AST_STRING:
            printf("STRING_LITERAL \"%.*s\"\n", node->as.string_literal.length,
                   node->as.string_literal.value);
            break;

        case AST_VAR_DECL:
            printf("VARIABLE_DECL %.*s\n", node->as.variable_decl.name_length,
                   node->as.variable_decl.name);
            ASTPrintIndent(indent + 1);
            fputs("TYPE\n", stdout);
            TypeSyntaxPrint(node->as.variable_decl.declared_type, indent + 2);
            if (node->as.variable_decl.initializer != NULL) {
                ASTPrintChild("INITIALIZER", node->as.variable_decl.initializer,
                              indent + 1);
            }
            break;

        case AST_VAR_REF:
            printf("VARIABLE_REF %.*s\n", node->as.variable_ref.name_length,
                   node->as.variable_ref.name);
            break;

        case AST_ASSIGN:
            printf("ASSIGN %.*s\n", node->as.assignment.name_length,
                   node->as.assignment.name);
            ASTPrintChild("VALUE", node->as.assignment.value, indent + 1);
            break;

        case AST_INDEX_ASSIGN:
            fputs("INDEX_ASSIGN\n", stdout);
            ASTPrintChild("TARGET", node->as.index_assignment.target,
                          indent + 1);
            ASTPrintChild("INDEX", node->as.index_assignment.index,
                          indent + 1);
            ASTPrintChild("VALUE", node->as.index_assignment.value,
                          indent + 1);
            break;

        case AST_BINARY_OP:
            printf("BINARY_OP %s\n",
                   TokenTypeToString(node->as.binary_op.operator_type));
            ASTPrintChild("LEFT", node->as.binary_op.left, indent + 1);
            ASTPrintChild("RIGHT", node->as.binary_op.right, indent + 1);
            break;

        case AST_UNARY_OP:
            printf("UNARY_OP %s\n",
                   TokenTypeToString(node->as.unary_op.operator_type));
            ASTPrintChild("OPERAND", node->as.unary_op.operand, indent + 1);
            break;

        case AST_CALL:
            printf("CALL %.*s\n", node->as.call.callee_name_length,
                   node->as.call.callee_name);
            for (int i = 0; i < node->as.call.argument_count; i++) {
                ASTPrintIndent(indent + 1);
                printf("ARGUMENT %d\n", i);
                ASTPrint(node->as.call.arguments[i], indent + 2);
            }
            break;

        case AST_INDEX:
            fputs("INDEX\n", stdout);
            ASTPrintChild("TARGET", node->as.index_expr.target, indent + 1);
            ASTPrintChild("SUBSCRIPT", node->as.index_expr.index, indent + 1);
            break;

        case AST_ARRAY_LEN_EXPR:
            fputs("ARRAY_LENGTH\n", stdout);
            ASTPrintChild("TARGET", node->as.array_length_expr.target,
                          indent + 1);
            break;

        case AST_BLOCK:
            printf("BLOCK %d\n", node->as.block.statement_count);
            for (int i = 0; i < node->as.block.statement_count; i++) {
                ASTPrint(node->as.block.statements[i], indent + 1);
            }
            break;

        case AST_IF:
            fputs("IF_STATEMENT\n", stdout);
            ASTPrintChild("CONDITION", node->as.if_statement.condition,
                          indent + 1);
            ASTPrintChild("THEN_BRANCH", node->as.if_statement.then_branch,
                          indent + 1);
            if (node->as.if_statement.else_branch != NULL) {
                ASTPrintChild("ELSE_BRANCH", node->as.if_statement.else_branch,
                              indent + 1);
            }
            break;

        case AST_WHILE:
            fputs("WHILE_STATEMENT\n", stdout);
            ASTPrintChild("CONDITION", node->as.while_statement.condition,
                          indent + 1);
            ASTPrintChild("BODY", node->as.while_statement.body, indent + 1);
            break;

        case AST_FOR:
            fputs("FOR_STATEMENT\n", stdout);
            if (node->as.for_statement.initializer != NULL) {
                ASTPrintChild("INITIALIZER",
                              node->as.for_statement.initializer, indent + 1);
            }
            if (node->as.for_statement.condition != NULL) {
                ASTPrintChild("CONDITION", node->as.for_statement.condition,
                              indent + 1);
            }
            if (node->as.for_statement.increment != NULL) {
                ASTPrintChild("INCREMENT", node->as.for_statement.increment,
                              indent + 1);
            }
            ASTPrintChild("BODY", node->as.for_statement.body, indent + 1);
            break;

        case AST_FOREACH:
            printf("FOREACH_STATEMENT value=%.*s",
                   node->as.foreach_statement.variable_name_length,
                   node->as.foreach_statement.variable_name);
            if (node->as.foreach_statement.index_name != NULL) {
                printf(" index=%.*s",
                       node->as.foreach_statement.index_name_length,
                       node->as.foreach_statement.index_name);
            }
            fputc('\n', stdout);
            if (node->as.foreach_statement.index_type != NULL) {
                ASTPrintIndent(indent + 1);
                fputs("INDEX_TYPE\n", stdout);
                TypeSyntaxPrint(node->as.foreach_statement.index_type,
                                indent + 2);
            }
            if (node->as.foreach_statement.variable_type != NULL) {
                ASTPrintIndent(indent + 1);
                fputs("VALUE_TYPE\n", stdout);
                TypeSyntaxPrint(node->as.foreach_statement.variable_type,
                                indent + 2);
            }
            ASTPrintChild("RANGE",
                          node->as.foreach_statement.array_expression,
                          indent + 1);
            ASTPrintChild("BODY", node->as.foreach_statement.body, indent + 1);
            break;

        case AST_FUNC_DECL:
            printf("FUNCTION_DECL %.*s\n", node->as.function_decl.name_length,
                   node->as.function_decl.name);
            ASTPrintIndent(indent + 1);
            fputs("RETURN_TYPE\n", stdout);
            TypeSyntaxPrint(node->as.function_decl.return_type, indent + 2);
            ASTPrintIndent(indent + 1);
            printf("PARAMETERS %d\n", node->as.function_decl.parameter_count);
            for (int i = 0; i < node->as.function_decl.parameter_count; i++) {
                ParameterSyntaxPrint(&node->as.function_decl.parameters[i],
                                     indent + 2);
            }
            ASTPrintChild("BODY", node->as.function_decl.body, indent + 1);
            break;

        case AST_RETURN:
            fputs("RETURN_STATEMENT\n", stdout);
            if (node->as.return_statement.expression != NULL) {
                ASTPrintChild("EXPRESSION",
                              node->as.return_statement.expression,
                              indent + 1);
            }
            break;
    }
}
