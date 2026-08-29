#ifndef HOLYD_COMPILER_H
#define HOLYD_COMPILER_H

#include "ast/ast.h"
#include "eval.h"
#include <stdint.h>

typedef enum {
  BC_STACK_PUSH_INT,
  BC_STACK_PUSH_FLOAT,
  BC_STACK_PUSH_STRING,
  BC_VARIABLE_LOAD,
  BC_VARIABLE_DEFINE,			/* := */
  BC_VARIABLE_STORE,			/* = */
  BC_MATH_ADD,						/* + */
  BC_MATH_SUB,						/* - */
  BC_MATH_MUL,						/* * */
  BC_MATH_DIV,						/* / */
  BC_MATH_MOD,						/* % */
  BC_MATH_POW,						/* ^^ */
  BC_COMPARE_EQ, 					/* == */
  BC_COMPARE_NE,					/* != */
  BC_COMPARE_LT,					/* < */
  BC_COMPARE_GT,					/* > */
  BC_COMPARE_LE,					/* <= */
  BC_COMPARE_GE,					/* >= */
  BC_LOGIC_NOT,						/* ! */
  BC_BIT_AND,							/* & */
  BC_BIT_OR,							/* | */
  BC_BIT_XOR,							/* ^ */
  BC_BIT_SHL,							/* << */
  BC_BIT_SHR,							/* >> signed */
  BC_BIT_USHR,						/* >>> unsigned */
  BC_ARRAY_CONCAT,
  BC_ARRAY_CREATE,
  BC_ARRAY_LENGTH,
  BC_ARRAY_GET,
  BC_ARRAY_SET,
  BC_FLOW_JUMP,
  BC_FLOW_JUMP_IF_FALSE,
  BC_FUNCTION_CALL,
  BC_STACK_POP,
  BC_FUNCTION_RETURN
} BytecodeOp;

typedef struct {
  BytecodeOp op;
  long long i64;
  double f64;
  const char *text;
  int text_len;
  int operand;
} Instruction;

typedef struct {
  Instruction *code;
  int count;
  int capacity;
} BytecodeChunk;

typedef struct {
  const char *name;
  int name_len;
  ParameterSyntax *parameters;
  int param_count;
  BytecodeChunk chunk;
} HDFunction;

typedef struct {
  BytecodeChunk main;
  HDFunction *functions;
  int function_count;
  int function_capacity;
  int temp_counter;
  int had_error;
} HDProgram;

void HDProgramInit(HDProgram *program);
int HDCompileProgram(ASTNode *ast, HDProgram *program);
int HDRunProgram(HDProgram *program);
void HDDumpProgram(HDProgram *program);

HDValue int_value(long long v);
HDValue string_value(const char *s, int len);
HDValue float_value(double v);

#endif
