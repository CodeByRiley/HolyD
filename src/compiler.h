#ifndef HOLYD_COMPILER_H
#define HOLYD_COMPILER_H

#include "ast/ast.h"
#include "eval.h"
#include "resolve.h"
#include <stdint.h>

typedef enum {
  BC_STACK_PUSH_INT,
  BC_STACK_PUSH_FLOAT,
  BC_STACK_PUSH_STRING,

  /* Name-keyed access to the runtime Environment. What is left on this
   * path after resolution: the temporaries the foreach lowering invents,
   * which have no source name and so no symbol. */
  BC_VARIABLE_LOAD,
  BC_VARIABLE_DEFINE,         /* := */
  BC_VARIABLE_STORE,          /* = */

  /* Slot-keyed access to a frame. `operand` is the slot; `fallback` is the
   * global slot a local defers to while its own is still inactive, or -1;
   * `text` is the source name, which is both the last link in that chain
   * (the FFI constants live in the Environment) and the error message.
   *
   * DEFINE activates a slot, which is what makes a declaration take effect
   * where it is written rather than at the top of the frame. STORE walks
   * the chain and only activates when nothing along it is bound, which is
   * how an assignment to an undeclared name creates one. */
  BC_LOCAL_LOAD,
  BC_LOCAL_DEFINE,
  BC_LOCAL_STORE,
  BC_GLOBAL_LOAD,
  BC_GLOBAL_DEFINE,
  BC_GLOBAL_STORE,

  BC_MATH_ADD,                /* + */
  BC_MATH_SUB,                /* - */
  BC_MATH_MUL,                /* * */
  BC_MATH_DIV,                /* / */
  BC_MATH_MOD,                /* % */
  BC_MATH_POW,                /* ^^ */

  BC_COMPARE_EQ,              /* == */
  BC_COMPARE_NE,              /* != */
  BC_COMPARE_LT,              /* < */
  BC_COMPARE_GT,              /* > */
  BC_COMPARE_LE,              /* <= */
  BC_COMPARE_GE,              /* >= */

  BC_LOGIC_NOT,               /* ! */

  BC_BIT_AND,                 /* & */
  BC_BIT_OR,                  /* | */
  BC_BIT_XOR,                 /* ^ */
  BC_BIT_SHL,                 /* << */
  BC_BIT_SHR,                 /* >> signed */
  BC_BIT_USHR,                /* >>> unsigned */

  BC_ARRAY_CONCAT,
  BC_ARRAY_CREATE,
  BC_ARRAY_LENGTH,
  BC_ARRAY_GET,
  BC_ARRAY_SET,

  /* Unified Control Flow */
  BC_FLOW_JUMP,               /* Unconditional jump (used by goto, loops, ternary) */
  BC_FLOW_JUMP_IF_FALSE,      /* Conditional jump */

  BC_FUNCTION_CALL,
  BC_FUNCTION_RETURN,
  BC_STACK_POP
} BytecodeOp;

typedef struct {
  BytecodeOp op;
  long long i64;
  double f64;
  const char *text;
  int text_len;
  int operand;
  int fallback;
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
  /* Where each parameter lands in the frame. Borrowed from the resolution,
   * which outlives the program, the way `parameters` is borrowed from the
   * AST. Not simply 0..param_count-1: two parameters of the same name share
   * one slot, and the VM has to write them both there to keep the later one
   * winning the way a repeated EnvDefine did. */
  const int *param_slots;
  int frame_slot_count;
  BytecodeChunk chunk;
} HDFunction;

typedef struct {
  BytecodeChunk main;
  HDFunction *functions;
  int function_count;
  int function_capacity;
  int temp_counter;
  int global_slot_count;
  int had_error;
} HDProgram;

/* Labels are keyed by the resolver's index rather than by name: the
 * resolver has already rejected a duplicate label and a goto that names
 * none, so there is nothing left here for a name to decide. */
typedef struct {
  int label_index;
  int target_ip;
} LabelSymbol;

/* A jump emitted before its label was reached, waiting for the address. */
typedef struct {
  int label_index;
  int jump_at;
} PendingGoto;

void HDProgramInit(HDProgram *program);
int HDCompileProgram(ASTNode *ast, const HDResolution *resolution,
                     HDProgram *program);
int HDRunProgram(HDProgram *program);
void HDDumpProgram(HDProgram *program);

HDValue int_value(long long v);
HDValue string_value(const char *s, int len);
HDValue float_value(double v);

#endif
