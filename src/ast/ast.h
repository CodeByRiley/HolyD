#ifndef HOLYD_AST_H
#define HOLYD_AST_H

#include "../lexer/lexer.h"
#include "type_syntax.h"

// All the different kinds of AST nodes
typedef enum {
    AST_NUMBER,         // 123
    AST_FLOAT,          // 1.5
    AST_STRING,         // "Hello"
    AST_VAR_DECL,       // I64 x = expr;
    AST_VAR_REF,        // x
    AST_ASSIGN,         // x = expr;
    AST_INDEX_ASSIGN,   // arr[index] = expr;
    AST_BINARY_OP,      // expr + expr
    AST_UNARY_OP,       // !expr, -expr
    AST_TERNARY_OP,     // cond ? expr : expr
    AST_CALL,           // Print(expr)
    AST_ARRAY_LITERAL,  // [expr, expr]
    AST_INDEX,          // array[index]
    AST_ARRAY_LEN_EXPR, // array.length
    AST_BLOCK,          // { statement1; statement2; }
    AST_IF,             // if (cond) { block } else { block }
    AST_WHILE,          // while (cond) { block }
    AST_FOR,            // for (init; cond; inc) { block }
    AST_FOREACH,        // foreach (x; arr) { block }
    AST_GOTO,						// GOTO label
    AST_LABEL,					// .label: (. defines start of label : defines the end)
    AST_BREAK,          // break;
    AST_CONTINUE,       // continue;
    AST_FUNC_DECL,
    AST_RETURN,				  // return expr
} ASTNodeType;

typedef struct {
    long long value;
    int is_boolean;
} ASTIntegerLiteral;

typedef struct {
    double value;
} ASTFloatLiteral;

typedef struct {
    const char* value;
    int length;
} ASTStringLiteral;

typedef struct {
    TypeSyntax* declared_type;
    const char* name;
    int name_length;
    ASTNode* initializer;
} ASTVariableDecl;

typedef struct {
    const char* name;
    int name_length;
} ASTVariableRef;

typedef struct {
    const char* name;
    int name_length;
    ASTNode* value;
} ASTAssignment;

typedef struct {
    ASTNode* target;
    ASTNode* index;
    ASTNode* value;
} ASTIndexAssignment;

typedef struct {
    TokenType operator_type;
    ASTNode* left;
    ASTNode* right;
} ASTBinaryOp;

typedef struct {
    TokenType operator_type;
    ASTNode* operand;
} ASTUnaryOp;

typedef struct {
  ASTNode* condition;
  ASTNode* true_expr;
  ASTNode* false_expr;
} ASTTernaryOp;

typedef struct {
	const char* name;
	int len;
} ASTLabel;

typedef struct {
	const char* target;
	int target_len;
} ASTGoto;

typedef struct {
    ASTNode* target;
    ASTNode* index;
} ASTIndexExpr;

typedef struct {
    ASTNode* target;
} ASTArrayLengthExpr;

typedef struct {
    const char* callee_name;
    int callee_name_length;
    ASTNode** arguments;
    int argument_count;
} ASTCall;

typedef struct {
    ASTNode** elements;
    int element_count;
} ASTArrayLiteral;

typedef struct {
    ASTNode** statements;
    int statement_count;
} ASTBlock;

typedef struct {
    ASTNode* condition;
    ASTNode* then_branch;
    ASTNode* else_branch;
} ASTIfStatement;

typedef struct {
    ASTNode* condition;
    ASTNode* body;
} ASTWhileStatement;

typedef struct {
    ASTNode* initializer;
    ASTNode* condition;
    ASTNode* increment;
    ASTNode* body;
} ASTForStatement;

typedef struct {
    TypeSyntax* variable_type;
    const char* variable_name;
    int variable_name_length;
    TypeSyntax* index_type;
    const char* index_name;
    int index_name_length;
    ASTNode* array_expression;
    ASTNode* body;
} ASTForeachStatement;

typedef struct {
    TypeSyntax* return_type;
    const char* name;
    int name_length;
    ParameterSyntax* parameters;
    int parameter_count;
    ASTNode* body;
} ASTFunctionDecl;

typedef struct {
    ASTNode* expression;
} ASTReturnStatement;

struct ASTNode {
    ASTNodeType type;
    HDSourceSpan span;
    union {
        ASTIntegerLiteral integer_literal;
        ASTFloatLiteral float_literal;
        ASTStringLiteral string_literal;
        ASTVariableDecl variable_decl;
        ASTVariableRef variable_ref;
        ASTAssignment assignment;
        ASTIndexAssignment index_assignment;
        ASTBinaryOp binary_op;
        ASTUnaryOp unary_op;
        ASTTernaryOp ternary_op;
        ASTIndexExpr index_expr;
        ASTArrayLengthExpr array_length_expr;
        ASTCall call;
        ASTArrayLiteral array_literal;
        ASTBlock block;
        ASTIfStatement if_statement;
        ASTWhileStatement while_statement;
        ASTForStatement for_statement;
        ASTForeachStatement foreach_statement;
        ASTFunctionDecl function_decl;
        ASTReturnStatement return_statement;
        ASTLabel label;
        ASTGoto goto_statement;
    } as;
};

HDSourceSpan HDSourceSpanCover(HDSourceSpan first, HDSourceSpan last);
ASTNode* ASTSetSpan(ASTNode* node, HDSourceSpan span);

// Helper functions to create nodes (allocates memory)
ASTNode* ASTNewNumber(long long val);
ASTNode* ASTNewBoolean(int value);
ASTNode* ASTNewFloat(double val);
ASTNode* ASTNewString(const char* str, int len);
ASTNode* ASTNewVarRef(const char* name, int len);
ASTNode* ASTNewVarDecl(TypeSyntax* type, const char* name, int len, ASTNode* init);
ASTNode* ASTNewAssign(const char* name, int len, ASTNode* value);
ASTNode* ASTNewBinaryOp(TokenType op, ASTNode* left, ASTNode* right);
ASTNode* ASTNewUnaryOp(TokenType op, ASTNode* operand);
ASTNode* ASTNewLabel(const char* name, int len);
ASTNode* ASTNewGoto(const char* target, int len);
ASTNode* ASTNewTernaryOp(ASTNode* condition, ASTNode* true_expr,
                         ASTNode* false_expr);
ASTNode* ASTNewIndex(ASTNode* target, ASTNode* index);
ASTNode* ASTNewIndexAssign(ASTNode* target, ASTNode* index, ASTNode* value);
ASTNode* ASTNewArrayLenExpr(ASTNode* target);
ASTNode* ASTNewCall(const char* name, int len, ASTNode** args, int arg_count);
ASTNode* ASTNewArrayLiteral(ASTNode** elements, int element_count);
ASTNode* ASTNewBlock(ASTNode** stmts, int count);
ASTNode* ASTNewIf(ASTNode* cond, ASTNode* then_block, ASTNode* else_block);
ASTNode* ASTNewWhile(ASTNode* cond, ASTNode* body);
ASTNode* ASTNewFor(ASTNode* init, ASTNode* cond, ASTNode* inc, ASTNode* body);
ASTNode* ASTNewForeach(TypeSyntax* variable_type, const char* var_name,
                       int var_len, TypeSyntax* index_type,
                       const char* index_name, int index_len,
                       ASTNode* array_expr, ASTNode* body);
ASTNode* ASTNewFuncDecl(TypeSyntax* return_type, const char* name, int len,
                        ParameterSyntax* parameters, int parameter_count,
                        ASTNode* body);
ASTNode* ASTNewReturn(ASTNode* expr);

/* break and continue carry no payload. D's labelled forms name the loop
 * they leave, but AST_LABEL does not wrap a statement yet, so there is
 * nothing for a label to refer to and no field is reserved for one. */
ASTNode* ASTNewBreak(void);
ASTNode* ASTNewContinue(void);

/* Recursively print a parsed tree for front-end diagnostics. */
void ASTPrint(const ASTNode* node, int indent);

#endif
