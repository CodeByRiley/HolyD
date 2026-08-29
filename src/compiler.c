#include "compiler.h"
#include "runtime.h"
#include "ffi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  HDProgram *program;
} Compiler;

static void chunk_init(BytecodeChunk *chunk) {
  chunk->code = NULL;
  chunk->count = 0;
  chunk->capacity = 0;
}

void HDProgramInit(HDProgram *program) {
  chunk_init(&program->main);
  program->functions = NULL;
  program->function_count = 0;
  program->function_capacity = 0;
  program->temp_counter = 0;
  program->had_error = 0;
}

static int grow_chunk(BytecodeChunk *chunk) {
  int new_capacity = chunk->capacity ? chunk->capacity * 2 : 32;
  Instruction *grown =
      (Instruction *)malloc(sizeof(Instruction) * new_capacity);
  if (!grown)
    return 0;
  for (int i = 0; i < chunk->count; i++)
    grown[i] = chunk->code[i];
  free(chunk->code);
  chunk->code = grown;
  chunk->capacity = new_capacity;
  return 1;
}

static int emit(BytecodeChunk *chunk, Instruction ins) {
  if (chunk->count >= chunk->capacity && !grow_chunk(chunk)) {
    printf("Compile error: out of memory while growing bytecode.\n");
    return -1;
  }
  chunk->code[chunk->count] = ins;
  return chunk->count++;
}

static Instruction make_ins(BytecodeOp op) {
  Instruction ins;
  ins.op = op;
  ins.i64 = 0;
  ins.f64 = 0.0;
  ins.text = NULL;
  ins.text_len = 0;
  ins.operand = 0;
  return ins;
}

static int emit_simple(BytecodeChunk *chunk, BytecodeOp op) {
  return emit(chunk, make_ins(op));
}

static int emit_text(BytecodeChunk *chunk, BytecodeOp op, const char *text,
                     int len) {
  Instruction ins = make_ins(op);
  ins.text = text;
  ins.text_len = len;
  return emit(chunk, ins);
}

static int emit_int(BytecodeChunk *chunk, long long value) {
  Instruction ins = make_ins(BC_STACK_PUSH_INT);
  ins.i64 = value;
  return emit(chunk, ins);
}

static int emit_float(BytecodeChunk *chunk, double value) {
  Instruction ins = make_ins(BC_STACK_PUSH_FLOAT);
  ins.f64 = value;
  return emit(chunk, ins);
}

static int emit_count(BytecodeChunk *chunk, BytecodeOp op, int count) {
  Instruction ins = make_ins(op);
  ins.operand = count;
  return emit(chunk, ins);
}

static int emit_call(BytecodeChunk *chunk, const char *name, int len,
                     int argc) {
  Instruction ins = make_ins(BC_FUNCTION_CALL);
  ins.text = name;
  ins.text_len = len;
  ins.operand = argc;
  return emit(chunk, ins);
}

static int emit_jump(BytecodeChunk *chunk, BytecodeOp op) {
  Instruction ins = make_ins(op);
  ins.operand = -1;
  return emit(chunk, ins);
}

static void patch_jump(BytecodeChunk *chunk, int jump_at) {
  if (jump_at >= 0 && jump_at < chunk->count) {
    chunk->code[jump_at].operand = chunk->count;
  }
}

static char *make_temp_name(Compiler *compiler, const char *suffix) {
  char *name = (char *)malloc(32);
  if (!name)
    return NULL;
  snprintf(name, 32, "$foreach_%d_%s", compiler->program->temp_counter++,
           suffix);
  return name;
}

static void compile_error(Compiler *compiler, const char *message) {
  printf("Compile error: %s\n", message);
  compiler->program->had_error = 1;
}

static int is_name(const char *a, int a_len, const char *b) {
  int b_len = (int)strlen(b);
  return a_len == b_len && strncmp(a, b, (size_t)a_len) == 0;
}

static HDFunction *program_find_function(HDProgram *program, const char *name,
                                         int len) {
  for (int i = 0; i < program->function_count; i++) {
    HDFunction *fn = &program->functions[i];
    if (fn->name_len == len && strncmp(fn->name, name, (size_t)len) == 0) {
      return fn;
    }
  }
  return NULL;
}

static int program_has_function(HDProgram *program, const char *name, int len) {
  return program_find_function(program, name, len) != NULL;
}

/* Does `chunk` already contain a call to `name`? Only the top-level chunk is
 * ever asked, so a call inside a function body does not count , which is what
 * keeps a recursive main() from suppressing its own entry-point call. */
static int chunk_calls(BytecodeChunk *chunk, const char *name, int len) {
  for (int i = 0; i < chunk->count; i++) {
    Instruction *ins = &chunk->code[i];
    if (ins->op == BC_FUNCTION_CALL && ins->text_len == len &&
        strncmp(ins->text, name, (size_t)len) == 0) {
      return 1;
    }
  }
  return 0;
}

static int is_empty_block(ASTNode *node) {
  return node && node->type == AST_BLOCK && node->as.block.statement_count == 0;
}

static void compile_expression(Compiler *compiler, BytecodeChunk *chunk,
                               ASTNode *node);
static void compile_statement(Compiler *compiler, BytecodeChunk *chunk,
                              ASTNode *node);

/* && and || evaluate their right operand only when the left did not
 * already decide the answer, so they cannot be compiled as an ordinary
 * binary op with both sides already on the stack. Both lower to the jump
 * the loops already use; no jump-if-true opcode is needed, and the result
 * is normalised to 0 or 1 the way C's operators are.
 *
 *   a && b            a || b
 *     <a>               <a>
 *     JF false          JF rhs
 *     <b>               PUSH 1
 *     JF false          JMP end
 *     PUSH 1     rhs:   <b>
 *     JMP end           JF false
 *   false:              PUSH 1
 *     PUSH 0            JMP end
 *   end:              false:
 *                       PUSH 0
 *                     end:
 */
static void compile_logical(Compiler *compiler, BytecodeChunk *chunk,
                            ASTNode *node, int is_and) {
  compile_expression(compiler, chunk, node->as.binary_op.left);

  if (is_and) {
    int left_false = emit_jump(chunk, BC_FLOW_JUMP_IF_FALSE);
    compile_expression(compiler, chunk, node->as.binary_op.right);
    int right_false = emit_jump(chunk, BC_FLOW_JUMP_IF_FALSE);

    emit_int(chunk, 1);
    int done = emit_jump(chunk, BC_FLOW_JUMP);
    patch_jump(chunk, left_false);
    patch_jump(chunk, right_false);
    emit_int(chunk, 0);
    patch_jump(chunk, done);
    return;
  }

  int try_right = emit_jump(chunk, BC_FLOW_JUMP_IF_FALSE);
  emit_int(chunk, 1);
  int done_true = emit_jump(chunk, BC_FLOW_JUMP);

  patch_jump(chunk, try_right);
  compile_expression(compiler, chunk, node->as.binary_op.right);
  int both_false = emit_jump(chunk, BC_FLOW_JUMP_IF_FALSE);
  emit_int(chunk, 1);
  int done_right = emit_jump(chunk, BC_FLOW_JUMP);

  patch_jump(chunk, both_false);
  emit_int(chunk, 0);
  patch_jump(chunk, done_true);
  patch_jump(chunk, done_right);
}

static void compile_binary_op(Compiler *compiler, BytecodeChunk *chunk,
                              ASTNode *node) {
  TokenType op = node->as.binary_op.operator_type;

  /* Short-circuiting has to branch around its right operand, so it is
   * handled before both sides get compiled. */
  if (op == TOKEN_ANDAND || op == TOKEN_OROR) {
    compile_logical(compiler, chunk, node, op == TOKEN_ANDAND);
    return;
  }

  compile_expression(compiler, chunk, node->as.binary_op.left);
  compile_expression(compiler, chunk, node->as.binary_op.right);

  switch (op) {
  case TOKEN_PLUS:
    emit_simple(chunk, BC_MATH_ADD);
    break;
  case TOKEN_MINUS:
    emit_simple(chunk, BC_MATH_SUB);
    break;
  case TOKEN_STAR:
    emit_simple(chunk, BC_MATH_MUL);
    break;
  case TOKEN_SLASH:
    emit_simple(chunk, BC_MATH_DIV);
    break;
  case TOKEN_EQEQ:
    emit_simple(chunk, BC_COMPARE_EQ);
    break;
  case TOKEN_NEQ:
    emit_simple(chunk, BC_COMPARE_NE);
    break;
  case TOKEN_LT:
    emit_simple(chunk, BC_COMPARE_LT);
    break;
  case TOKEN_GT:
    emit_simple(chunk, BC_COMPARE_GT);
    break;
  case TOKEN_LTEQ:
    emit_simple(chunk, BC_COMPARE_LE);
    break;
  case TOKEN_GTEQ:
    emit_simple(chunk, BC_COMPARE_GE);
    break;
  case TOKEN_TILDE:
    emit_simple(chunk, BC_ARRAY_CONCAT);
    break;
  case TOKEN_PERCENT:
    emit_simple(chunk, BC_MATH_MOD);
    break;
  case TOKEN_POW:
    emit_simple(chunk, BC_MATH_POW);
    break;
  /* Bitwise and is spelled TOKEN_AMPERSAND: the lexer emits one token for
   * '&' whether it is meant as address-of or as a bitwise operator, and
   * only the latter has a meaning today. */
  case TOKEN_AMPERSAND:
    emit_simple(chunk, BC_BIT_AND);
    break;
  case TOKEN_OR:
    emit_simple(chunk, BC_BIT_OR);
    break;
  case TOKEN_XOR:
    emit_simple(chunk, BC_BIT_XOR);
    break;
  case TOKEN_SHL:
    emit_simple(chunk, BC_BIT_SHL);
    break;
  case TOKEN_SHR:
    emit_simple(chunk, BC_BIT_SHR);
    break;
  case TOKEN_USHR:
    emit_simple(chunk, BC_BIT_USHR);
    break;
  default:
    compile_error(compiler, "unsupported binary operator.");
    break;
  }
}

static void compile_expression(Compiler *compiler, BytecodeChunk *chunk,
                               ASTNode *node) {
  if (!node) {
    emit_int(chunk, 0);
    return;
  }

  switch (node->type) {
  case AST_NUMBER:
    emit_int(chunk, node->as.integer_literal.value);
    break;

  case AST_FLOAT:
    emit_float(chunk, node->as.float_literal.value);
    break;

  case AST_STRING: {
    Instruction ins = make_ins(BC_STACK_PUSH_STRING);
    ins.text = node->as.string_literal.value;
    ins.text_len = node->as.string_literal.length;
    emit(chunk, ins);
    break;
  }

  case AST_VAR_REF:
    emit_text(chunk, BC_VARIABLE_LOAD, node->as.variable_ref.name, node->as.variable_ref.name_length);
    break;

  case AST_BINARY_OP:
    compile_binary_op(compiler, chunk, node);
    break;

  /* Unary minus is 0 - x rather than its own opcode; negation asks
   * HDTruthy, so it works on strings and arrays where a comparison
   * against zero would have been a type error. */
  case AST_UNARY_OP:
    if (node->as.unary_op.operator_type == TOKEN_MINUS) {
      emit_int(chunk, 0);
      compile_expression(compiler, chunk, node->as.unary_op.operand);
      emit_simple(chunk, BC_MATH_SUB);
    } else if (node->as.unary_op.operator_type == TOKEN_BANG) {
      compile_expression(compiler, chunk, node->as.unary_op.operand);
      emit_simple(chunk, BC_LOGIC_NOT);
    } else {
      compile_error(compiler, "unsupported unary operator.");
      emit_int(chunk, 0);
    }
    break;

  case AST_INDEX:
    compile_expression(compiler, chunk, node->as.index_expr.target);
    compile_expression(compiler, chunk, node->as.index_expr.index);
    emit_simple(chunk, BC_ARRAY_GET);
    break;

  case AST_ARRAY_LEN_EXPR:
    compile_expression(compiler, chunk, node->as.array_length_expr.target);
    emit_simple(chunk, BC_ARRAY_LENGTH);
    break;

  case AST_CALL:
    if (is_name(node->as.call.callee_name, node->as.call.callee_name_length, "[array]")) {
      for (int i = 0; i < node->as.call.argument_count; i++) {
        compile_expression(compiler, chunk, node->as.call.arguments[i]);
      }
      emit_count(chunk, BC_ARRAY_CREATE, node->as.call.argument_count);
      break;
    }

    for (int i = 0; i < node->as.call.argument_count; i++) {
      compile_expression(compiler, chunk, node->as.call.arguments[i]);
    }
    emit_call(chunk, node->as.call.callee_name, node->as.call.callee_name_length, node->as.call.argument_count);
    break;

  default:
    compile_error(compiler, "statement used where an expression was expected.");
    emit_int(chunk, 0);
    break;
  }
}

static void compile_block(Compiler *compiler, BytecodeChunk *chunk,
                          ASTNode *node) {
  if (!node)
    return;
  for (int i = 0; i < node->as.block.statement_count; i++) {
    compile_statement(compiler, chunk, node->as.block.statements[i]);
  }
}

static void compile_foreach(Compiler *compiler, BytecodeChunk *chunk,
                            ASTNode *node) {
  char *array_name = make_temp_name(compiler, "array");
  char *index_name = make_temp_name(compiler, "index");
  if (!array_name || !index_name) {
    compile_error(compiler, "out of memory while naming foreach temporaries.");
    return;
  }

  compile_expression(compiler, chunk, node->as.foreach_statement.array_expression);
  emit_text(chunk, BC_VARIABLE_DEFINE, array_name, (int)strlen(array_name));
  emit_int(chunk, 0);
  emit_text(chunk, BC_VARIABLE_DEFINE, index_name, (int)strlen(index_name));

  int loop_start = chunk->count;
  emit_text(chunk, BC_VARIABLE_LOAD, index_name, (int)strlen(index_name));
  emit_text(chunk, BC_VARIABLE_LOAD, array_name, (int)strlen(array_name));
  emit_simple(chunk, BC_ARRAY_LENGTH);
  emit_simple(chunk, BC_COMPARE_LT);
  int exit_jump = emit_jump(chunk, BC_FLOW_JUMP_IF_FALSE);

  emit_text(chunk, BC_VARIABLE_LOAD, array_name, (int)strlen(array_name));
  emit_text(chunk, BC_VARIABLE_LOAD, index_name, (int)strlen(index_name));
  emit_simple(chunk, BC_ARRAY_GET);
  emit_text(chunk, BC_VARIABLE_DEFINE,
            node->as.foreach_statement.variable_name,
            node->as.foreach_statement.variable_name_length);

  if (node->as.foreach_statement.index_name != NULL) {
    emit_text(chunk, BC_VARIABLE_LOAD, index_name, (int)strlen(index_name));
    emit_text(chunk, BC_VARIABLE_DEFINE, node->as.foreach_statement.index_name, node->as.foreach_statement.index_name_length);
  }

  compile_statement(compiler, chunk, node->as.foreach_statement.body);

  emit_text(chunk, BC_VARIABLE_LOAD, index_name, (int)strlen(index_name));
  emit_int(chunk, 1);
  emit_simple(chunk, BC_MATH_ADD);
  emit_text(chunk, BC_VARIABLE_STORE, index_name, (int)strlen(index_name));

  Instruction jump_back = make_ins(BC_FLOW_JUMP);
  jump_back.operand = loop_start;
  emit(chunk, jump_back);
  patch_jump(chunk, exit_jump);
}

static void compile_statement(Compiler *compiler, BytecodeChunk *chunk,
                              ASTNode *node) {
  if (!node)
    return;

  switch (node->type) {
  case AST_VAR_DECL:
    compile_expression(compiler, chunk, node->as.variable_decl.initializer);
    emit_text(chunk, BC_VARIABLE_DEFINE, node->as.variable_decl.name,
              node->as.variable_decl.name_length);
    break;

  case AST_ASSIGN:
    compile_expression(compiler, chunk, node->as.assignment.value);
    emit_text(chunk, BC_VARIABLE_STORE, node->as.assignment.name,
              node->as.assignment.name_length);
    break;

  /* Array, index, value -> BC_ARRAY_SET. Elements are reached through
   * the shared HDValue.elements pointer, so writing through the copy
   * on the stack updates the array held by the environment. */
  case AST_INDEX_ASSIGN:
    compile_expression(compiler, chunk, node->as.index_assignment.target);
    compile_expression(compiler, chunk, node->as.index_assignment.index);
    compile_expression(compiler, chunk, node->as.index_assignment.value);
    emit_simple(chunk, BC_ARRAY_SET);
    break;

  case AST_BLOCK:
    compile_block(compiler, chunk, node);
    break;

  case AST_IF: {
    compile_expression(compiler, chunk, node->as.if_statement.condition);
    int else_jump = emit_jump(chunk, BC_FLOW_JUMP_IF_FALSE);
    compile_statement(compiler, chunk, node->as.if_statement.then_branch);
    int end_jump = emit_jump(chunk, BC_FLOW_JUMP);
    patch_jump(chunk, else_jump);
    if (node->as.if_statement.else_branch) {
      compile_statement(compiler, chunk, node->as.if_statement.else_branch);
    }
    patch_jump(chunk, end_jump);
    break;
  }

  case AST_WHILE: {
    int loop_start = chunk->count;
    compile_expression(compiler, chunk, node->as.while_statement.condition);
    int exit_jump = emit_jump(chunk, BC_FLOW_JUMP_IF_FALSE);
    compile_statement(compiler, chunk, node->as.while_statement.body);
    Instruction jump_back = make_ins(BC_FLOW_JUMP);
    jump_back.operand = loop_start;
    emit(chunk, jump_back);
    patch_jump(chunk, exit_jump);
    break;
  }

  case AST_FOR: {
    if (node->as.for_statement.initializer) {
      compile_statement(compiler, chunk, node->as.for_statement.initializer);
    }

    int loop_start = chunk->count;
    if (node->as.for_statement.condition) {
      compile_expression(compiler, chunk, node->as.for_statement.condition);
    } else {
      emit_int(chunk, 1);
    }
    int exit_jump = emit_jump(chunk, BC_FLOW_JUMP_IF_FALSE);

    compile_statement(compiler, chunk, node->as.for_statement.body);

    if (node->as.for_statement.increment) {
      compile_statement(compiler, chunk, node->as.for_statement.increment);
    }

    Instruction jump_back = make_ins(BC_FLOW_JUMP);
    jump_back.operand = loop_start;
    emit(chunk, jump_back);
    patch_jump(chunk, exit_jump);
    break;
  }

  case AST_FOREACH:
    compile_foreach(compiler, chunk, node);
    break;

  case AST_FUNC_DECL:
    break;

  case AST_RETURN:
    compile_expression(compiler, chunk, node->as.return_statement.expression);
    emit_simple(chunk, BC_FUNCTION_RETURN);
    break;

  case AST_STRING:
    compile_expression(compiler, chunk, node);
    emit_call(chunk, "Print", 5, 1);
    emit_simple(chunk, BC_STACK_POP);
    break;

  case AST_VAR_REF:
    if (program_has_function(compiler->program, node->as.variable_ref.name,
                             node->as.variable_ref.name_length)) {
      emit_call(chunk, node->as.variable_ref.name,
                node->as.variable_ref.name_length, 0);
    } else {
      compile_expression(compiler, chunk, node);
    }
    emit_simple(chunk, BC_STACK_POP);
    break;

  default:
    compile_expression(compiler, chunk, node);
    emit_simple(chunk, BC_STACK_POP);
    break;
  }
}

static HDFunction *add_function(HDProgram *program, ASTNode *node) {
  if (program->function_count >= program->function_capacity) {
    int new_capacity =
        program->function_capacity ? program->function_capacity * 2 : 8;
    HDFunction *grown = (HDFunction *)malloc(sizeof(HDFunction) * new_capacity);
    if (!grown)
      return NULL;
    for (int i = 0; i < program->function_count; i++)
      grown[i] = program->functions[i];
    free(program->functions);
    program->functions = grown;
    program->function_capacity = new_capacity;
  }

  HDFunction *fn = &program->functions[program->function_count++];
  fn->name = node->as.function_decl.name;
  fn->name_len = node->as.function_decl.name_length;
  fn->parameters = node->as.function_decl.parameters;
  fn->param_count = node->as.function_decl.parameter_count;
  chunk_init(&fn->chunk);
  return fn;
}

int HDCompileProgram(ASTNode *ast, HDProgram *program) {
  Compiler compiler;
  compiler.program = program;

  if (!ast || ast->type != AST_BLOCK) {
    compile_error(&compiler, "program root is not a block.");
    return 0;
  }

  for (int i = 0; i < ast->as.block.statement_count; i++) {
    ASTNode *stmt = ast->as.block.statements[i];
    if (stmt && stmt->type == AST_FUNC_DECL) {
      HDFunction *fn = add_function(program, stmt);
      if (!fn) {
        compile_error(&compiler, "out of memory while adding function.");
        return 0;
      }
      compile_statement(&compiler, &fn->chunk, stmt->as.function_decl.body);
      emit_int(&fn->chunk, 0);
      emit_simple(&fn->chunk, BC_FUNCTION_RETURN);
    }
  }

  for (int i = 0; i < ast->as.block.statement_count; i++) {
    ASTNode *stmt = ast->as.block.statements[i];
    if (!stmt || stmt->type == AST_FUNC_DECL)
      continue;
    if (is_empty_block(stmt))
      continue;
    compile_statement(&compiler, &program->main, stmt);
  }

  /* Run the entry point after the top-level statements, so the globals a
   * file declares above its functions are defined by the time main() reads
   * them.
   *
   * The gate is "does the top level already call it", not "does the file
   * have any top-level statements". The latter read well for a file that is
   * nothing but declarations, but it meant a single `I64 W = 420;` above the
   * functions silently switched the entry point off: the program ran, drew
   * nothing, and exited 0. A file that calls main() itself still gets
   * exactly one call.
   *
   * An entry point that declares parameters is left alone , there is nothing
   * to pass it, and calling it with none is a runtime arity error. */
  const char *entry = program_has_function(program, "main", 4)   ? "main"
                      : program_has_function(program, "Main", 4) ? "Main"
                                                                 : NULL;
  if (entry != NULL) {
    HDFunction *fn = program_find_function(program, entry, 4);
    if (fn->param_count == 0 && !chunk_calls(&program->main, entry, 4)) {
      emit_call(&program->main, entry, 4, 0);
      emit_simple(&program->main, BC_STACK_POP);
    }
  }

  emit_int(&program->main, 0);
  emit_simple(&program->main, BC_FUNCTION_RETURN);

  return !program->had_error;
}

static int is_print_builtin(Instruction *ins, int *add_newline) {
  if (is_name(ins->text, ins->text_len, "Print") ||
      is_name(ins->text, ins->text_len, "print") ||
      is_name(ins->text, ins->text_len, "write")) {
    *add_newline = 0;
    return 1;
  }

  if (is_name(ins->text, ins->text_len, "PrintLn") ||
      is_name(ins->text, ins->text_len, "println") ||
      is_name(ins->text, ins->text_len, "writeln")) {
    *add_newline = 1;
    return 1;
  }

  return 0;
}

static HDFunction *find_function(HDProgram *program, const char *name,
                                 int len) {
  for (int i = 0; i < program->function_count; i++) {
    HDFunction *fn = &program->functions[i];
    if (fn->name_len == len && strncmp(fn->name, name, (size_t)len) == 0) {
      return fn;
    }
  }
  return NULL;
}

typedef struct {
  HDProgram *program;
  Environment *globals;
} VM;

/* The VM's opcodes and the runtime's operator enum are separate so that
 * generated C never has to include the bytecode definitions. One switch
 * bridges them. */
static HDBinOp binop_for(BytecodeOp op) {
  switch (op) {
  case BC_MATH_SUB:
    return HD_SUB;
  case BC_MATH_MUL:
    return HD_MUL;
  case BC_MATH_DIV:
    return HD_DIV;
  case BC_COMPARE_EQ:
    return HD_EQ;
  case BC_COMPARE_NE:
    return HD_NE;
  case BC_COMPARE_LT:
    return HD_LT;
  case BC_COMPARE_GT:
    return HD_GT;
  case BC_COMPARE_LE:
    return HD_LE;
  case BC_COMPARE_GE:
    return HD_GE;
  case BC_MATH_MOD:
    return HD_MOD;
  case BC_MATH_POW:
    return HD_POW;
  case BC_BIT_AND:
    return HD_BAND;
  case BC_BIT_OR:
    return HD_BOR;
  case BC_BIT_XOR:
    return HD_BXOR;
  case BC_BIT_SHL:
    return HD_SHL;
  case BC_BIT_SHR:
    return HD_SHR;
  case BC_BIT_USHR:
    return HD_USHR;
  case BC_ARRAY_CONCAT:
    return HD_CONCAT;
  case BC_MATH_ADD:
  default:
    return HD_ADD;
  }
}

static int run_chunk(VM *vm, BytecodeChunk *chunk, Environment *env,
                     HDValue *out);

static int call_function(VM *vm, Instruction *ins, HDValue *stack, int *sp) {
  int add_newline = 0;
  if (is_print_builtin(ins, &add_newline)) {
    if (*sp < ins->operand) {
      printf("Runtime error: stack underflow in print builtin.\n");
      return 0;
    }
    int first = *sp - ins->operand;
    HDValue result = HDPrintN(ins->operand, &stack[first], add_newline);
    *sp = first;
    stack[(*sp)++] = result;
    return 1;
  }

  NativeFn native_fn = ffi_lookup_native(ins->text, ins->text_len);
  if (native_fn != NULL) {
    if (*sp < ins->operand) {
      printf("Runtime error: stack underflow in native call.\n");
      return 0;
    }
    int first = *sp - ins->operand;
    HDValue result = native_fn(ins->operand, &stack[first]);
    *sp = first;
    stack[(*sp)++] = result;
    return 1;
  }

  HDFunction *fn = find_function(vm->program, ins->text, ins->text_len);
  if (!fn) {
    printf("Runtime error: unknown function '%.*s'.\n", ins->text_len,
           ins->text);
    return 0;
  }
  if (fn->param_count != ins->operand) {
    printf("Runtime error: function '%.*s' expects %d args, got %d.\n",
           fn->name_len, fn->name, fn->param_count, ins->operand);
    return 0;
  }
  if (*sp < ins->operand) {
    printf("Runtime error: stack underflow in function call.\n");
    return 0;
  }

  Environment local;
  EnvInitChild(&local, vm->globals);

  int first = *sp - ins->operand;
  for (int i = 0; i < fn->param_count; i++) {
    EnvDefine(&local, fn->parameters[i].name,
              (size_t)fn->parameters[i].name_length, stack[first + i]);
  }
  *sp = first;

  HDValue result = int_value(0);
  if (!run_chunk(vm, &fn->chunk, &local, &result))
    return 0;
  stack[(*sp)++] = result;
  return 1;
}

static int pop_value(HDValue *stack, int *sp, HDValue *out) {
  if (*sp <= 0) {
    printf("Runtime error: stack underflow.\n");
    return 0;
  }
  *out = stack[--(*sp)];
  return 1;
}

static int push_value(HDValue *stack, int *sp, HDValue value) {
  if (*sp >= 256) {
    printf("Runtime error: stack overflow.\n");
    return 0;
  }
  stack[(*sp)++] = value;
  return 1;
}

static int run_chunk(VM *vm, BytecodeChunk *chunk, Environment *env,
                     HDValue *out) {
  HDValue stack[256];
  int sp = 0;
  int ip = 0;

  while (ip < chunk->count) {
    Instruction *ins = &chunk->code[ip++];
    HDValue left, right, value;

    switch (ins->op) {
    case BC_STACK_PUSH_INT:
      if (!push_value(stack, &sp, int_value(ins->i64)))
        return 0;
      break;

    case BC_STACK_PUSH_FLOAT:
      if (!push_value(stack, &sp, float_value(ins->f64)))
        return 0;
      break;

    case BC_STACK_PUSH_STRING:
      if (!push_value(stack, &sp, string_value(ins->text, ins->text_len)))
        return 0;
      break;

    case BC_VARIABLE_LOAD: {
      HDValue *found = EnvGet(env, ins->text, (size_t)ins->text_len);
      if (!found) {
        printf("Runtime error: undefined variable '%.*s'.\n", ins->text_len,
               ins->text);
        return 0;
      }
      if (!push_value(stack, &sp, *found))
        return 0;
      break;
    }

    case BC_VARIABLE_DEFINE:
      if (!pop_value(stack, &sp, &value))
        return 0;
      EnvDefine(env, ins->text, (size_t)ins->text_len, value);
      break;

    case BC_VARIABLE_STORE:
      if (!pop_value(stack, &sp, &value))
        return 0;
      EnvSet(env, ins->text, (size_t)ins->text_len, value);
      break;

    case BC_MATH_ADD:
    case BC_MATH_SUB:
    case BC_MATH_MUL:
    case BC_MATH_DIV:
    case BC_MATH_MOD:
    case BC_MATH_POW:
    case BC_BIT_AND:
    case BC_BIT_OR:
    case BC_BIT_XOR:
    case BC_BIT_SHL:
    case BC_BIT_SHR:
    case BC_BIT_USHR:
    case BC_COMPARE_EQ:
    case BC_COMPARE_NE:
    case BC_COMPARE_LT:
    case BC_COMPARE_GT:
    case BC_COMPARE_LE:
    case BC_COMPARE_GE:
    case BC_ARRAY_CONCAT:
      if (!pop_value(stack, &sp, &right) || !pop_value(stack, &sp, &left))
        return 0;
      if (!HDBinary(binop_for(ins->op), left, right, &value))
        return 0;
      if (!push_value(stack, &sp, value))
        return 0;
      break;

    case BC_ARRAY_CREATE: {
      if (sp < ins->operand) {
        printf("Runtime error: stack underflow while creating array.");
        return 0;
      }
      int first = sp - ins->operand;
      if (!HDArrayNew(ins->operand, &stack[first], &value))
        return 0;
      sp = first;
      if (!push_value(stack, &sp, value))
        return 0;
      break;
    }

    case BC_ARRAY_LENGTH:
      if (!pop_value(stack, &sp, &left))
        return 0;
      if (!HDLength(left, &value))
        return 0;
      if (!push_value(stack, &sp, value))
        return 0;
      break;

    case BC_ARRAY_GET:
      if (!pop_value(stack, &sp, &right) || !pop_value(stack, &sp, &left))
        return 0;
      if (!HDIndex(left, right, &value))
        return 0;
      if (!push_value(stack, &sp, value))
        return 0;
      break;

    case BC_ARRAY_SET:
      if (!pop_value(stack, &sp, &value) || !pop_value(stack, &sp, &right) ||
          !pop_value(stack, &sp, &left))
        return 0;
      if (!HDIndexSet(left, right, value))
        return 0;
      break;

    case BC_LOGIC_NOT:
      if (!pop_value(stack, &sp, &value))
        return 0;
      if (!push_value(stack, &sp, HDNot(value)))
        return 0;
      break;

    case BC_FLOW_JUMP:
      ip = ins->operand;
      break;

    case BC_FLOW_JUMP_IF_FALSE:
      if (!pop_value(stack, &sp, &value))
        return 0;
      if (!HDTruthy(value))
        ip = ins->operand;
      break;

    case BC_FUNCTION_CALL:
      if (!call_function(vm, ins, stack, &sp))
        return 0;
      break;

    case BC_STACK_POP:
      if (!pop_value(stack, &sp, &value))
        return 0;
      break;

    case BC_FUNCTION_RETURN:
      if (sp > 0) {
        *out = stack[--sp];
      } else {
        *out = int_value(0);
      }
      return 1;
    }
  }

  *out = int_value(0);
  return 1;
}

int HDRunProgram(HDProgram *program) {
  Environment globals;
  EnvInit(&globals);
  /* Before the program, so a script that assigns over EV_KEY_DOWN wins. */
  ffi_define_globals(&globals);

  VM vm;
  vm.program = program;
  vm.globals = &globals;

  HDValue result = int_value(0);
  return run_chunk(&vm, &program->main, &globals, &result);
}

static const char *op_name(BytecodeOp op) {
  switch (op) {
  case BC_STACK_PUSH_INT:
    return "STACK_PUSH_INT";
  case BC_STACK_PUSH_FLOAT:
    return "STACK_PUSH_FLOAT";
  case BC_STACK_PUSH_STRING:
    return "STACK_PUSH_STRING";
  case BC_VARIABLE_LOAD:
    return "VARIABLE_LOAD";
  case BC_VARIABLE_DEFINE:
    return "VARIABLE_DEFINE";
  case BC_VARIABLE_STORE:
    return "VARIABLE_STORE";
  case BC_MATH_ADD:
    return "MATH_ADD";
  case BC_MATH_SUB:
    return "MATH_SUB";
  case BC_MATH_MUL:
    return "MATH_MUL";
  case BC_MATH_DIV:
    return "MATH_DIV";
  case BC_COMPARE_EQ:
    return "COMPARE_EQ";
  case BC_COMPARE_NE:
    return "COMPARE_NE";
  case BC_COMPARE_LT:
    return "COMPARE_LT";
  case BC_COMPARE_GT:
    return "COMPARE_GT";
  case BC_COMPARE_LE:
    return "COMPARE_LE";
  case BC_COMPARE_GE:
    return "COMPARE_GE";
  case BC_MATH_MOD:
    return "MATH_MOD";
  case BC_MATH_POW:
    return "MATH_POW";
  case BC_LOGIC_NOT:
    return "LOGIC_NOT";
  case BC_BIT_AND:
    return "BIT_AND";
  case BC_BIT_OR:
    return "BIT_OR";
  case BC_BIT_XOR:
    return "BIT_XOR";
  case BC_BIT_SHL:
    return "BIT_SHL";
  case BC_BIT_SHR:
    return "BIT_SHR";
  case BC_BIT_USHR:
    return "BIT_USHR";
  case BC_ARRAY_CONCAT:
    return "ARRAY_CONCAT";
  case BC_ARRAY_CREATE:
    return "ARRAY_CREATE";
  case BC_ARRAY_LENGTH:
    return "ARRAY_LENGTH";
  case BC_ARRAY_GET:
    return "ARRAY_GET";
  case BC_ARRAY_SET:
    return "ARRAY_SET";
  case BC_FLOW_JUMP:
    return "FLOW_JUMP";
  case BC_FLOW_JUMP_IF_FALSE:
    return "FLOW_JUMP_IF_FALSE";
  case BC_FUNCTION_CALL:
    return "FUNCTION_CALL";
  case BC_STACK_POP:
    return "STACK_POP";
  case BC_FUNCTION_RETURN:
    return "FUNCTION_RETURN";
  default:
    return "UNKNOWN";
  }
}

static void dump_chunk(const char *name, BytecodeChunk *chunk) {
  printf("== %s ==\n", name);
  for (int i = 0; i < chunk->count; i++) {
    Instruction *ins = &chunk->code[i];
    printf("%04d %-22s", i, op_name(ins->op));
    if (ins->op == BC_STACK_PUSH_INT) {
      printf(" %lld", ins->i64);
    } else if (ins->op == BC_STACK_PUSH_FLOAT) {
      printf(" ");
      HDPrintDouble(ins->f64);
    } else if (ins->op == BC_STACK_PUSH_STRING) {
      printf(" \"%.*s\"", ins->text_len, ins->text);
    } else if (ins->op == BC_VARIABLE_LOAD || ins->op == BC_VARIABLE_DEFINE ||
               ins->op == BC_VARIABLE_STORE || ins->op == BC_FUNCTION_CALL) {
      printf(" %.*s", ins->text_len, ins->text);
      if (ins->op == BC_FUNCTION_CALL)
        printf(" argc=%d", ins->operand);
    } else if (ins->op == BC_ARRAY_CREATE || ins->op == BC_FLOW_JUMP ||
               ins->op == BC_FLOW_JUMP_IF_FALSE) {
      printf(" %d", ins->operand);
    }
    printf("\n");
  }
}

void HDDumpProgram(HDProgram *program) {
  for (int i = 0; i < program->function_count; i++) {
    char name[64];
    int len = program->functions[i].name_len;
    if (len > 48)
      len = 48;
    memcpy(name, program->functions[i].name, (size_t)len);
    name[len] = '\0';
    dump_chunk(name, &program->functions[i].chunk);
  }
  dump_chunk("main", &program->main);
}
