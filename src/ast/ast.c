#include "ast.h"
#include <stdio.h>
#include <stdlib.h>

static ASTNode* ASTNewNode(ASTNodeType type) {
    ASTNode* node = (ASTNode*)calloc(1, sizeof(ASTNode));
    if (node != NULL) node->type = type;
    return node;
}

HDSourceSpan HDSourceSpanCover(HDSourceSpan first, HDSourceSpan last) {
    if (first.start_line == 0) return last;
    if (last.start_line == 0) return first;
    first.end_offset = last.end_offset;
    first.end_line = last.end_line;
    first.end_column = last.end_column;
    return first;
}

ASTNode* ASTSetSpan(ASTNode* node, HDSourceSpan span) {
    if (node != NULL) node->span = span;
    return node;
}

ASTNode* ASTNewNumber(long long val) {
    ASTNode* node = ASTNewNode(AST_NUMBER);
    node->as.integer_literal.value = val;
    return node;
}

ASTNode* ASTNewBoolean(int value) {
    ASTNode* node = ASTNewNumber(value != 0);
    node->as.integer_literal.is_boolean = 1;
    return node;
}

ASTNode* ASTNewFloat(double val) {
    ASTNode* node = ASTNewNode(AST_FLOAT);
    node->as.float_literal.value = val;
    return node;
}

ASTNode* ASTNewString(const char* str, int len) {
    ASTNode* node = ASTNewNode(AST_STRING);
    node->as.string_literal.value = str;
    node->as.string_literal.length = len;
    return node;
}

ASTNode* ASTNewVarRef(const char* name, int len) {
    ASTNode* node = ASTNewNode(AST_VAR_REF);
    node->as.variable_ref.name = name;
    node->as.variable_ref.name_length = len;
    return node;
}

ASTNode* ASTNewVarDecl(TypeSyntax* type, const char* name, int len, ASTNode* init) {
    ASTNode* node = ASTNewNode(AST_VAR_DECL);
    node->as.variable_decl.declared_type = type;
    node->as.variable_decl.name = name;
    node->as.variable_decl.name_length = len;
    node->as.variable_decl.initializer = init;
    return node;
}

ASTNode* ASTNewAssign(const char* name, int len, ASTNode* value) {
    ASTNode* node = ASTNewNode(AST_ASSIGN);
    node->as.assignment.name = name;
    node->as.assignment.name_length = len;
    node->as.assignment.value = value;
    return node;
}

ASTNode* ASTNewBinaryOp(TokenType op, ASTNode* left, ASTNode* right) {
    ASTNode* node = ASTNewNode(AST_BINARY_OP);
    node->as.binary_op.operator_type = op;
    node->as.binary_op.left = left;
    node->as.binary_op.right = right;
    if (left != NULL && right != NULL)
        node->span = HDSourceSpanCover(left->span, right->span);
    return node;
}

ASTNode* ASTNewUnaryOp(TokenType op, ASTNode* operand) {
    ASTNode* node = ASTNewNode(AST_UNARY_OP);
    node->as.unary_op.operator_type = op;
    node->as.unary_op.operand = operand;
    if (operand != NULL) node->span = operand->span;
    return node;
}

ASTNode* ASTNewTernaryOp(ASTNode* condition, ASTNode* true_expr,
                         ASTNode* false_expr) {
    ASTNode* node = ASTNewNode(AST_TERNARY_OP);
    node->as.ternary_op.condition = condition;
    node->as.ternary_op.true_expr = true_expr;
    node->as.ternary_op.false_expr = false_expr;
    if (condition != NULL && false_expr != NULL)
        node->span = HDSourceSpanCover(condition->span, false_expr->span);
    return node;
}

/* A label names a point in its function, not a value, so it carries only
 * the name. Which point it names is a question for the resolver. */
ASTNode* ASTNewLabel(const char* name, int len) {
    ASTNode* node = ASTNewNode(AST_LABEL);
    node->as.label.name = name;
    node->as.label.len = len;
    return node;
}

ASTNode* ASTNewGoto(const char* target, int len) {
    ASTNode* node = ASTNewNode(AST_GOTO);
    node->as.goto_statement.target = target;
    node->as.goto_statement.target_len = len;
    return node;
}

ASTNode* ASTNewIndex(ASTNode* target, ASTNode* index) {
    ASTNode* node = ASTNewNode(AST_INDEX);
    node->as.index_expr.target = target;
    node->as.index_expr.index = index;
    if (target != NULL && index != NULL)
        node->span = HDSourceSpanCover(target->span, index->span);
    return node;
}

ASTNode* ASTNewIndexAssign(ASTNode* target, ASTNode* index, ASTNode* value) {
    ASTNode* node = ASTNewNode(AST_INDEX_ASSIGN);
    node->as.index_assignment.target = target;
    node->as.index_assignment.index = index;
    node->as.index_assignment.value = value;
    if (target != NULL && value != NULL)
        node->span = HDSourceSpanCover(target->span, value->span);
    return node;
}

ASTNode* ASTNewArrayLenExpr(ASTNode* target) {
    ASTNode* node = ASTNewNode(AST_ARRAY_LEN_EXPR);
    node->as.array_length_expr.target = target;
    return node;
}

ASTNode* ASTNewCall(const char* name, int len, ASTNode** args, int arg_count) {
    ASTNode* node = ASTNewNode(AST_CALL);
    node->as.call.callee_name = name;
    node->as.call.callee_name_length = len;
    node->as.call.arguments = args;
    node->as.call.argument_count = arg_count;
    return node;
}

ASTNode* ASTNewArrayLiteral(ASTNode** elements, int element_count) {
    ASTNode* node = ASTNewNode(AST_ARRAY_LITERAL);
    node->as.array_literal.elements = elements;
    node->as.array_literal.element_count = element_count;
    if (element_count > 0 && elements[0] != NULL &&
        elements[element_count - 1] != NULL) {
        node->span = HDSourceSpanCover(elements[0]->span,
                                       elements[element_count - 1]->span);
    }
    return node;
}

ASTNode* ASTNewBlock(ASTNode** stmts, int count) {
    ASTNode* node = ASTNewNode(AST_BLOCK);
    node->as.block.statements = stmts;
    node->as.block.statement_count = count;
    if (count > 0 && stmts[0] != NULL && stmts[count - 1] != NULL)
        node->span = HDSourceSpanCover(stmts[0]->span, stmts[count - 1]->span);
    return node;
}

ASTNode* ASTNewIf(ASTNode* cond, ASTNode* then_block, ASTNode* else_block) {
    ASTNode* node = ASTNewNode(AST_IF);
    node->as.if_statement.condition = cond;
    node->as.if_statement.then_branch = then_block;
    node->as.if_statement.else_branch = else_block;
    if (cond != NULL) {
        ASTNode* last = else_block != NULL ? else_block : then_block;
        node->span = last != NULL ? HDSourceSpanCover(cond->span, last->span)
                                  : cond->span;
    }
    return node;
}

ASTNode* ASTNewWhile(ASTNode* cond, ASTNode* body) {
    ASTNode* node = ASTNewNode(AST_WHILE);
    node->as.while_statement.condition = cond;
    node->as.while_statement.body = body;
    if (cond != NULL && body != NULL)
        node->span = HDSourceSpanCover(cond->span, body->span);
    return node;
}

ASTNode* ASTNewFor(ASTNode* init, ASTNode* cond, ASTNode* inc, ASTNode* body) {
    ASTNode* node = ASTNewNode(AST_FOR);
    node->as.for_statement.initializer = init;
    node->as.for_statement.condition = cond;
    node->as.for_statement.increment = inc;
    node->as.for_statement.body = body;
    ASTNode* first = init != NULL ? init : cond != NULL ? cond : inc;
    if (first != NULL && body != NULL)
        node->span = HDSourceSpanCover(first->span, body->span);
    return node;
}

ASTNode* ASTNewForeach(TypeSyntax* variable_type, const char* var_name,
                       int var_len, TypeSyntax* index_type,
                       const char* index_name, int index_len,
                       ASTNode* array_expr, ASTNode* body) {
    ASTNode* node = ASTNewNode(AST_FOREACH);
    node->as.foreach_statement.variable_type = variable_type;
    node->as.foreach_statement.variable_name = var_name;
    node->as.foreach_statement.variable_name_length = var_len;
    node->as.foreach_statement.index_type = index_type;
    node->as.foreach_statement.index_name = index_name;
    node->as.foreach_statement.index_name_length = index_len;
    node->as.foreach_statement.array_expression = array_expr;
    node->as.foreach_statement.body = body;
    if (array_expr != NULL && body != NULL)
        node->span = HDSourceSpanCover(array_expr->span, body->span);
    return node;
}

ASTNode* ASTNewFuncDecl(TypeSyntax* return_type, const char* name, int len,
                        ParameterSyntax* parameters, int parameter_count,
                        ASTNode* body) {
    ASTNode* node = ASTNewNode(AST_FUNC_DECL);
    node->as.function_decl.return_type = return_type;
    node->as.function_decl.name = name;
    node->as.function_decl.name_length = len;
    node->as.function_decl.parameters = parameters;
    node->as.function_decl.parameter_count = parameter_count;
    node->as.function_decl.body = body;
    if (body != NULL) node->span = body->span;
    return node;
}

ASTNode* ASTNewReturn(ASTNode* expr) {
    ASTNode* node = ASTNewNode(AST_RETURN);
    node->as.return_statement.expression = expr;
    if (expr != NULL) node->span = expr->span;
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

    if (node->span.start_line != 0) {
        printf("[%zu:%zu-%zu:%zu] ", node->span.start_line,
               node->span.start_column, node->span.end_line,
               node->span.end_column);
    }

    switch (node->type) {
        case AST_NUMBER:
            if (node->as.integer_literal.is_boolean)
                printf("BOOL_LITERAL %s\n",
                       node->as.integer_literal.value ? "true" : "false");
            else
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

        case AST_TERNARY_OP:
            fputs("TERNARY_OP\n", stdout);
            ASTPrintChild("CONDITION", node->as.ternary_op.condition,
                          indent + 1);
            ASTPrintChild("TRUE", node->as.ternary_op.true_expr, indent + 1);
            ASTPrintChild("FALSE", node->as.ternary_op.false_expr,
                          indent + 1);
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

        case AST_ARRAY_LITERAL:
            printf("ARRAY_LITERAL %d\n", node->as.array_literal.element_count);
            for (int i = 0; i < node->as.array_literal.element_count; i++) {
                ASTPrintIndent(indent + 1);
                printf("ELEMENT %d\n", i);
                ASTPrint(node->as.array_literal.elements[i], indent + 2);
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

        case AST_LABEL:
            printf("LABEL %.*s\n", node->as.label.len, node->as.label.name);
            break;

        case AST_GOTO:
            printf("GOTO %.*s\n", node->as.goto_statement.target_len,
                   node->as.goto_statement.target);
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
