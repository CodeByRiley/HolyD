/* HolyD -> C. See emit_c.h for what this backend is and is not.
 *
 * Two things drive the shape of the output.
 *
 * Argument order. The VM evaluates arguments strictly left to right;
 * C leaves the order of function arguments, and of compound-literal
 * initialisers, unspecified. So no call is emitted as a plain C call with
 * expressions in it. Every call site gets its own HDValue array, filled by
 * a comma expression , which C *does* sequence left to right , and the call
 * reads the array:
 *
 *     (_t3[0] = <arg0>, _t3[1] = <arg1>, hd_fn_0_Add(_t3[0], _t3[1]))
 *
 * The arrays are function-local, one per call site, sized in a pre-pass
 * that walks the body in exactly the same order emission does. If the two
 * walks ever disagree the generated C fails to compile on an undeclared
 * _tN, which is a loud failure rather than a quiet miscompile.
 *
 * Scope. HolyD scopes variables to the function, not the block, and a
 * declaration takes effect where it is written: until it runs, the name
 * still means whatever the enclosing scope makes of it. So a resolved name
 * becomes a C object plus a bit saying whether it is bound yet, and reading
 * it walks the chain the resolver recorded , this frame, then a global of
 * the same name, then the Environment, which is where the FFI constants
 * live and where an undefined name is finally an error. Hoisting the
 * declarations instead would have been wrong for exactly the case that
 * chain exists to serve.
 *
 * Parameters are the one kind of local bound on entry, so they are a plain
 * C variable with no bit and no chain.
 */

#include "emit_c.h"
#include "ffi.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  FILE *out;
  const HDResolution *resolution;
  ASTNode **functions; /* AST_FUNC_DECL nodes, in source order */
  int function_count;
  int temp_counter;  /* reset per emitted C function */
  int store_counter; /* likewise, for the one-shot value an assignment holds */
  int scope;         /* resolver function index, or -1 at the top level */
  int had_error;
} Emitter;

static void emit_expression(Emitter *e, ASTNode *node);
static void emit_statement(Emitter *e, ASTNode *node, int indent);

static void emit_error(Emitter *e, const char *message) {
  printf("Emit error: %s\n", message);
  e->had_error = 1;
}

static int name_is(const char *a, int a_len, const char *b) {
  int b_len = (int)strlen(b);
  return a_len == b_len && strncmp(a, b, (size_t)a_len) == 0;
}

static int find_function(Emitter *e, const char *name, int len) {
  for (int i = 0; i < e->function_count; i++) {
    ASTNode *fn = e->functions[i];
    if (fn->as.function_decl.name_length == len &&
        strncmp(fn->as.function_decl.name, name, (size_t)len) == 0) {
      return i;
    }
  }
  return -1;
}

/* Mirrors is_print_builtin in compiler.c, including the order it is checked
 * in: a print name wins over a native and over a user function. */
static int is_print_builtin(const char *name, int len, int *add_newline) {
  if (name_is(name, len, "Print") || name_is(name, len, "print") ||
      name_is(name, len, "write")) {
    *add_newline = 0;
    return 1;
  }
  if (name_is(name, len, "PrintLn") || name_is(name, len, "println") ||
      name_is(name, len, "writeln")) {
    *add_newline = 1;
    return 1;
  }
  return 0;
}

static void indent_by(Emitter *e, int indent) {
  for (int i = 0; i < indent; i++)
    fputs("  ", e->out);
}

/* Source bytes verbatim into a C literal. Escapes are not resolved here ,
 * the runtime unescapes at print time, exactly as it does for the VM, so
 * what goes into .rodata is the same byte string the VM holds. Non-printing
 * bytes use three-digit octal, which cannot swallow a following digit the
 * way \x would. */
static void write_c_string(Emitter *e, const char *s, int len) {
  fputc('"', e->out);
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\\')
      fputs("\\\\", e->out);
    else if (c == '"')
      fputs("\\\"", e->out);
    else if (c >= 32 && c < 127)
      fputc((int)c, e->out);
    else
      fprintf(e->out, "\\%03o", c);
  }
  fputc('"', e->out);
}

/* The index keeps the C name unique whatever the script called the
 * function, and the prefix keeps it clear of C's own main. */
static void write_mangled(Emitter *e, int index, const char *name, int len) {
  fprintf(e->out, "hd_fn_%d_", index);
  for (int i = 0; i < len; i++) {
    char c = name[i];
    fputc((isalnum((unsigned char)c) || c == '_') ? c : '_', e->out);
  }
}

/* ---------------- Names ---------------------------------------------------- */

/* What a node names in the role it appears in, or NULL when the resolver
 * recorded nothing for it. */
static const HDSymbol *symbol_for(Emitter *e, const ASTNode *node,
                                  HDBindingRole role) {
  const HDBinding *binding = HDResolutionBinding(e->resolution, node, role);
  if (!binding)
    return NULL;
  return HDResolutionSymbol(e->resolution, binding->symbol_id);
}

/* One C identifier per slot. The slot number keeps it unique whatever the
 * script called the name, and the name keeps the output readable. */
static void write_slot_name(Emitter *e, const HDSymbol *symbol) {
  fprintf(e->out, symbol->storage == HD_SYMBOL_GLOBAL ? "hd_g%d_" : "hd_v%d_",
          symbol->slot);
  for (int i = 0; i < symbol->name_length; i++) {
    char c = symbol->name[i];
    fputc((isalnum((unsigned char)c) || c == '_') ? c : '_', e->out);
  }
}

/* C labels are function-scoped, so the resolver's index alone would do.
 * The name comes along because the generated C is meant to be read. */
static void write_label_name(Emitter *e, int label_index) {
  const HDLabel *label = HDResolutionLabel(e->resolution, label_index);
  fprintf(e->out, "hd_lbl_%d_", label_index);
  if (!label)
    return;
  for (int i = 0; i < label->name_length; i++) {
    char c = label->name[i];
    fputc((isalnum((unsigned char)c) || c == '_') ? c : '_', e->out);
  }
}

/* The companion bit. Parameters have none: they are bound on entry. */
static int has_bound_bit(const HDSymbol *symbol) {
  return !(symbol->flags & HD_SYMBOL_PARAMETER);
}

static void write_bound_name(Emitter *e, const HDSymbol *symbol) {
  write_slot_name(e, symbol);
  fputs("_bound", e->out);
}

static void write_env_name(Emitter *e, const char *name, int length) {
  fputs("&hd_globals, ", e->out);
  write_c_string(e, name, length);
  fprintf(e->out, ", %d", length);
}

/* The global a local defers to while its own slot is unbound, or NULL. */
static const HDSymbol *global_fallback(Emitter *e, const HDSymbol *symbol) {
  const HDSymbol *fallback =
      HDResolutionSymbol(e->resolution, symbol->fallback_id);
  if (fallback && fallback->storage == HD_SYMBOL_GLOBAL)
    return fallback;
  return NULL;
}

/* An expression yielding what the name means right now. */
static void emit_load(Emitter *e, const HDSymbol *symbol, const char *name,
                      int length) {
  if (!symbol || symbol->storage == HD_SYMBOL_EXTERNAL) {
    fputs("HDLoadX(", e->out);
    write_env_name(e, name, length);
    fputc(')', e->out);
    return;
  }

  if (!has_bound_bit(symbol)) {
    write_slot_name(e, symbol);
    return;
  }

  fputc('(', e->out);
  write_bound_name(e, symbol);
  fputs(" ? ", e->out);
  write_slot_name(e, symbol);
  fputs(" : ", e->out);

  const HDSymbol *fallback = global_fallback(e, symbol);
  if (fallback) {
    fputc('(', e->out);
    write_bound_name(e, fallback);
    fputs(" ? ", e->out);
    write_slot_name(e, fallback);
    fputs(" : ", e->out);
  }
  fputs("HDLoadX(", e->out);
  write_env_name(e, name, length);
  fputc(')', e->out);
  if (fallback)
    fputc(')', e->out);
  fputc(')', e->out);
}

/* A declaration binds its own slot whatever else the name meant, so it is
 * an assignment to the slot plus its bit. Both halves are written by
 * emit_define_open / emit_define_close, with the value in between, so the
 * same pair serves an AST initialiser and the foreach lowering. */
static void emit_define_open(Emitter *e, const HDSymbol *symbol,
                             const char *name, int length, int indent) {
  indent_by(e, indent);
  if (!symbol || symbol->storage == HD_SYMBOL_EXTERNAL) {
    fputs("EnvDefine(", e->out);
    write_env_name(e, name, length);
    fputs(", ", e->out);
    return;
  }
  write_slot_name(e, symbol);
  fputs(" = ", e->out);
}

static void emit_define_close(Emitter *e, const HDSymbol *symbol) {
  if (!symbol || symbol->storage == HD_SYMBOL_EXTERNAL) {
    fputs(");\n", e->out);
    return;
  }
  fputs(";", e->out);
  if (has_bound_bit(symbol)) {
    fputc(' ', e->out);
    write_bound_name(e, symbol);
    fputs(" = 1;", e->out);
  }
  fputc('\n', e->out);
}

/* An assignment writes to whatever the name already means, and only binds a
 * new slot when the name means nothing yet. The value is evaluated once,
 * before the chain is walked, which is the order the VM uses. */
static void emit_store_open(Emitter *e, const HDSymbol *symbol,
                            const char *name, int length, int indent,
                            int *id) {
  indent_by(e, indent);
  if (!symbol || symbol->storage == HD_SYMBOL_EXTERNAL) {
    *id = -1;
    fputs("EnvSet(", e->out);
    write_env_name(e, name, length);
    fputs(", ", e->out);
    return;
  }
  if (!has_bound_bit(symbol)) {
    *id = -1;
    write_slot_name(e, symbol);
    fputs(" = ", e->out);
    return;
  }
  *id = e->store_counter++;
  fprintf(e->out, "{ HDValue _s%d = ", *id);
}

static void emit_store_close(Emitter *e, const HDSymbol *symbol,
                             const char *name, int length, int indent,
                             int id) {
  if (id < 0) {
    fputs(symbol && symbol->storage != HD_SYMBOL_EXTERNAL ? ";\n" : ");\n",
          e->out);
    return;
  }

  const HDSymbol *fallback = global_fallback(e, symbol);
  fputs(";\n", e->out);

  indent_by(e, indent + 1);
  fputs("if (", e->out);
  write_bound_name(e, symbol);
  fputs(") ", e->out);
  write_slot_name(e, symbol);
  fprintf(e->out, " = _s%d;\n", id);

  if (fallback) {
    indent_by(e, indent + 1);
    fputs("else if (", e->out);
    write_bound_name(e, fallback);
    fputs(") ", e->out);
    write_slot_name(e, fallback);
    fprintf(e->out, " = _s%d;\n", id);
  }

  indent_by(e, indent + 1);
  fputs("else if (EnvGet(", e->out);
  write_env_name(e, name, length);
  fputs(")) EnvSet(", e->out);
  write_env_name(e, name, length);
  fprintf(e->out, ", _s%d);\n", id);

  indent_by(e, indent + 1);
  fputs("else { ", e->out);
  write_slot_name(e, symbol);
  fprintf(e->out, " = _s%d; ", id);
  write_bound_name(e, symbol);
  fputs(" = 1; }\n", e->out);

  indent_by(e, indent);
  fputs("}\n", e->out);
}

/* ---------------- Call-site temporaries ---------------------------------- */

/* Pre-order, matching emit_expression: a node takes its id before its
 * children take theirs. */
static void count_expression(Emitter *e, ASTNode *node, int *max_argc,
                             int capacity);
static void count_statement(Emitter *e, ASTNode *node, int *max_argc,
                            int capacity);

static void count_call(Emitter *e, ASTNode *node, int argc, int *max_argc,
                       int capacity) {
  int id = e->temp_counter++;
  (void)node;
  /* The first pass runs with no table, only to learn how many sites there
   * are; the second fills it. */
  if (max_argc != NULL && id < capacity && argc > max_argc[id])
    max_argc[id] = argc;
}

static void count_expression(Emitter *e, ASTNode *node, int *max_argc,
                             int capacity) {
  if (!node)
    return;
  switch (node->type) {
  case AST_BINARY_OP:
    count_expression(e, node->as.binary_op.left, max_argc, capacity);
    count_expression(e, node->as.binary_op.right, max_argc, capacity);
    break;
  case AST_INDEX:
    count_expression(e, node->as.index_expr.target, max_argc, capacity);
    count_expression(e, node->as.index_expr.index, max_argc, capacity);
    break;
  case AST_UNARY_OP:
    count_expression(e, node->as.unary_op.operand, max_argc, capacity);
    break;
  case AST_TERNARY_OP:
    count_expression(e, node->as.ternary_op.condition, max_argc, capacity);
    count_expression(e, node->as.ternary_op.true_expr, max_argc, capacity);
    count_expression(e, node->as.ternary_op.false_expr, max_argc, capacity);
    break;
  case AST_ARRAY_LEN_EXPR:
    count_expression(e, node->as.array_length_expr.target, max_argc, capacity);
    break;
  case AST_CALL:
    count_call(e, node, node->as.call.argument_count, max_argc, capacity);
    for (int i = 0; i < node->as.call.argument_count; i++)
      count_expression(e, node->as.call.arguments[i], max_argc, capacity);
    break;
  default:
    break;
  }
}

static void count_statement(Emitter *e, ASTNode *node, int *max_argc,
                            int capacity) {
  if (!node)
    return;
  switch (node->type) {
  case AST_VAR_DECL:
    count_expression(e, node->as.variable_decl.initializer, max_argc,
                     capacity);
    break;
  case AST_ASSIGN:
    count_expression(e, node->as.assignment.value, max_argc, capacity);
    break;
  case AST_INDEX_ASSIGN:
    count_call(e, node, 3, max_argc, capacity); /* target, index, value */
    count_expression(e, node->as.index_assignment.target, max_argc, capacity);
    count_expression(e, node->as.index_assignment.index, max_argc, capacity);
    count_expression(e, node->as.index_assignment.value, max_argc, capacity);
    break;
  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      count_statement(e, node->as.block.statements[i], max_argc, capacity);
    break;
  case AST_IF:
    count_expression(e, node->as.if_statement.condition, max_argc, capacity);
    count_statement(e, node->as.if_statement.then_branch, max_argc, capacity);
    count_statement(e, node->as.if_statement.else_branch, max_argc, capacity);
    break;
  case AST_WHILE:
    count_expression(e, node->as.while_statement.condition, max_argc,
                     capacity);
    count_statement(e, node->as.while_statement.body, max_argc, capacity);
    break;
  case AST_FOR:
    count_statement(e, node->as.for_statement.initializer, max_argc, capacity);
    count_expression(e, node->as.for_statement.condition, max_argc, capacity);
    count_statement(e, node->as.for_statement.body, max_argc, capacity);
    count_statement(e, node->as.for_statement.increment, max_argc, capacity);
    break;
  case AST_FOREACH:
    count_call(e, node, 1, max_argc, capacity); /* the array snapshot */
    count_expression(e, node->as.foreach_statement.array_expression, max_argc, capacity);
    count_statement(e, node->as.foreach_statement.body, max_argc, capacity);
    break;
  case AST_RETURN:
    count_expression(e, node->as.return_statement.expression, max_argc, capacity);
    break;
  case AST_FUNC_DECL:
    break;
  case AST_LABEL:
  case AST_GOTO:
    /* Neither takes a call-site temporary. Spelled out rather than left to
     * the default, because this walk and emit_statement have to allocate
     * ids in lockstep. */
    break;
  case AST_STRING:
    count_call(e, node, 1, max_argc, capacity); /* the implicit Print */
    break;
  case AST_VAR_REF:
    /* A bare name in statement position is a zero-argument call when it
     * names a function, and an ordinary discarded load otherwise. */
    if (find_function(e, node->as.variable_ref.name,
                      node->as.variable_ref.name_length) >= 0)
      count_call(e, node, 0, max_argc, capacity);
    break;
  default:
    count_expression(e, node, max_argc, capacity);
    break;
  }
}

/* Declares one HDValue array per call site in `body`, and returns how many
 * there are so the caller can reset the counter before emitting. */
static int declare_temps(Emitter *e, ASTNode *body, int indent) {
  /* First walk counts the sites, second records each one's width. */
  e->temp_counter = 0;
  count_statement(e, body, NULL, 0);
  int count = e->temp_counter;

  int *max_argc = NULL;
  if (count > 0) {
    max_argc = (int *)calloc((size_t)count, sizeof(int));
    if (!max_argc) {
      emit_error(e, "out of memory while sizing call-site temporaries.");
      return 0;
    }
    e->temp_counter = 0;
    count_statement(e, body, max_argc, count);
  }

  for (int i = 0; i < count; i++) {
    /* A zero-argument call still needs a name to pass; size 1 keeps the
     * declaration legal without special-casing the call site. */
    int size = max_argc[i] > 0 ? max_argc[i] : 1;
    indent_by(e, indent);
    fprintf(e->out, "HDValue _t%d[%d];\n", i, size);
  }
  free(max_argc);

  e->temp_counter = 0;
  return count;
}

/* ---------------- Expressions --------------------------------------------- */

/* Fills _t<id> left to right, then hands it to whatever consumes it. The
 * comma operator is what makes the order defined. */
static void emit_args_into(Emitter *e, int id, ASTNode **args, int argc) {
  for (int i = 0; i < argc; i++) {
    fprintf(e->out, "_t%d[%d] = ", id, i);
    emit_expression(e, args[i]);
    fputs(", ", e->out);
  }
}

static void emit_call_expression(Emitter *e, ASTNode *node) {
  int id = e->temp_counter++;
  int argc = node->as.call.argument_count;

  /* An array literal reaches the compiler as a call to "[array]". */
  if (name_is(node->as.call.callee_name, node->as.call.callee_name_length, "[array]")) {
    fputc('(', e->out);
    emit_args_into(e, id, node->as.call.arguments, argc);
    fprintf(e->out, "HDArrayNewX(%d, _t%d))", argc, id);
    return;
  }

  int add_newline = 0;
  if (is_print_builtin(node->as.call.callee_name, node->as.call.callee_name_length, &add_newline)) {
    fputc('(', e->out);
    emit_args_into(e, id, node->as.call.arguments, argc);
    fprintf(e->out, "HDPrintN(%d, _t%d, %d))", argc, id, add_newline);
    return;
  }

  /* Same precedence the VM uses: print, then native, then user function. */
  if (ffi_lookup_native(node->as.call.callee_name, node->as.call.callee_name_length) != NULL) {
    fputc('(', e->out);
    emit_args_into(e, id, node->as.call.arguments, argc);
    fputs("HDNativeX(", e->out);
    write_c_string(e, node->as.call.callee_name, node->as.call.callee_name_length);
    fprintf(e->out, ", %d, %d, _t%d))", node->as.call.callee_name_length, argc, id);
    return;
  }

  int index = find_function(e, node->as.call.callee_name, node->as.call.callee_name_length);
  if (index < 0) {
    /* Deferred to run time, as in the VM: a script that never reaches this
     * call still runs. */
    fputs("(HDUnknownFunctionX(", e->out);
    write_c_string(e, node->as.call.callee_name, node->as.call.callee_name_length);
    fprintf(e->out, ", %d))", node->as.call.callee_name_length);
    return;
  }

  ASTNode *fn = e->functions[index];
  if (fn->as.function_decl.parameter_count != argc) {
    fputs("(HDArityX(", e->out);
    write_c_string(e, node->as.call.callee_name, node->as.call.callee_name_length);
    fprintf(e->out, ", %d, %d, %d))", node->as.call.callee_name_length, fn->as.function_decl.parameter_count, argc);
    return;
  }

  fputc('(', e->out);
  emit_args_into(e, id, node->as.call.arguments, argc);
  write_mangled(e, index, fn->as.function_decl.name, fn->as.function_decl.name_length);
  fputc('(', e->out);
  for (int i = 0; i < argc; i++) {
    if (i > 0)
      fputs(", ", e->out);
    fprintf(e->out, "_t%d[%d]", id, i);
  }
  fputs("))", e->out);
}

static const char *binop_name(TokenType op) {
  switch (op) {
  case TOKEN_PLUS:
    return "HD_ADD";
  case TOKEN_MINUS:
    return "HD_SUB";
  case TOKEN_STAR:
    return "HD_MUL";
  case TOKEN_SLASH:
    return "HD_DIV";
  case TOKEN_EQEQ:
    return "HD_EQ";
  case TOKEN_NEQ:
    return "HD_NE";
  case TOKEN_LT:
    return "HD_LT";
  case TOKEN_GT:
    return "HD_GT";
  case TOKEN_LTEQ:
    return "HD_LE";
  case TOKEN_GTEQ:
    return "HD_GE";
  case TOKEN_PERCENT:
    return "HD_MOD";
  case TOKEN_POW:
    return "HD_POW";
  case TOKEN_AMPERSAND:
    return "HD_BAND";
  case TOKEN_OR:
    return "HD_BOR";
  case TOKEN_XOR:
    return "HD_BXOR";
  case TOKEN_SHL:
    return "HD_SHL";
  case TOKEN_SHR:
    return "HD_SHR";
  case TOKEN_USHR:
    return "HD_USHR";
  case TOKEN_TILDE:
    return "HD_CONCAT";
  default:
    return NULL;
  }
}

static void emit_expression(Emitter *e, ASTNode *node) {
  if (!node) {
    fputs("int_value(0)", e->out);
    return;
  }

  switch (node->type) {
  case AST_NUMBER:
    fprintf(e->out, "int_value(%lldLL)", node->as.integer_literal.value);
    break;

  case AST_FLOAT:
    /* %.17g round-trips an IEEE double exactly. */
    fprintf(e->out, "float_value(%.17g)", node->as.float_literal.value);
    break;

  case AST_STRING:
    fputs("string_value(", e->out);
    write_c_string(e, node->as.string_literal.value, node->as.string_literal.length);
    fprintf(e->out, ", %d)", node->as.string_literal.length);
    break;

  case AST_VAR_REF:
    emit_load(e, symbol_for(e, node, HD_BINDING_READ),
              node->as.variable_ref.name,
              node->as.variable_ref.name_length);
    break;

  case AST_BINARY_OP: {
    /* C's && and || short-circuit exactly as the VM's jumps do, and yield
     * 0 or 1 like the VM's normalising pushes, so these lower to the C
     * operators rather than to a runtime call. */
    TokenType logical = node->as.binary_op.operator_type;
    if (logical == TOKEN_ANDAND || logical == TOKEN_OROR) {
      fputs("int_value(HDTruthy(", e->out);
      emit_expression(e, node->as.binary_op.left);
      fputs(logical == TOKEN_ANDAND ? ") && HDTruthy(" : ") || HDTruthy(",
            e->out);
      emit_expression(e, node->as.binary_op.right);
      fputs("))", e->out);
      break;
    }

    const char *op = binop_name(node->as.binary_op.operator_type);
    if (!op) {
      emit_error(e, "unsupported binary operator.");
      fputs("int_value(0)", e->out);
      break;
    }
    /* HDBinaryX takes its operands as arguments, so their order is
     * unspecified in C , but both are pure expressions here in the sense
     * that any calls inside them have already been sequenced into their own
     * temporaries. Nested calls sequence themselves; what is left is order
     * between two independent sub-expressions, which the VM also leaves
     * unobservable because neither can see the other's effects. */
    fprintf(e->out, "HDBinaryX(%s, ", op);
    emit_expression(e, node->as.binary_op.left);
    fputs(", ", e->out);
    emit_expression(e, node->as.binary_op.right);
    fputc(')', e->out);
    break;
  }

  case AST_UNARY_OP:
    if (node->as.unary_op.operator_type == TOKEN_MINUS) {
      fputs("HDBinaryX(HD_SUB, int_value(0), ", e->out);
      emit_expression(e, node->as.unary_op.operand);
      fputc(')', e->out);
    } else if (node->as.unary_op.operator_type == TOKEN_BANG) {
      fputs("HDNot(", e->out);
      emit_expression(e, node->as.unary_op.operand);
      fputc(')', e->out);
    } else {
      emit_error(e, "unsupported unary operator.");
      fputs("int_value(0)", e->out);
    }
    break;

  case AST_TERNARY_OP:
    fputs("(HDTruthy(", e->out);
    emit_expression(e, node->as.ternary_op.condition);
    fputs(") ? ", e->out);
    emit_expression(e, node->as.ternary_op.true_expr);
    fputs(" : ", e->out);
    emit_expression(e, node->as.ternary_op.false_expr);
    fputc(')', e->out);
    break;

  case AST_INDEX:
    fputs("HDIndexX(", e->out);
    emit_expression(e, node->as.index_expr.target);
    fputs(", ", e->out);
    emit_expression(e, node->as.index_expr.index);
    fputc(')', e->out);
    break;

  case AST_ARRAY_LEN_EXPR:
    fputs("HDLengthX(", e->out);
    emit_expression(e, node->as.array_length_expr.target);
    fputc(')', e->out);
    break;

  case AST_CALL:
    emit_call_expression(e, node);
    break;

  default:
    emit_error(e, "statement used where an expression was expected.");
    fputs("int_value(0)", e->out);
    break;
  }
}

/* ---------------- Statements ---------------------------------------------- */

static void emit_body(Emitter *e, ASTNode *node, int indent) {
  if (!node)
    return;
  if (node->type == AST_BLOCK) {
    for (int i = 0; i < node->as.block.statement_count; i++)
      emit_statement(e, node->as.block.statements[i], indent);
    return;
  }
  emit_statement(e, node, indent);
}

static void emit_statement(Emitter *e, ASTNode *node, int indent) {
  if (!node)
    return;

  switch (node->type) {
  case AST_VAR_DECL: {
    const HDSymbol *symbol = symbol_for(e, node, HD_BINDING_DECLARATION);
    emit_define_open(e, symbol, node->as.variable_decl.name,
                     node->as.variable_decl.name_length, indent);
    emit_expression(e, node->as.variable_decl.initializer);
    emit_define_close(e, symbol);
    break;
  }

  case AST_ASSIGN: {
    const HDSymbol *symbol = symbol_for(e, node, HD_BINDING_WRITE);
    int id;
    emit_store_open(e, symbol, node->as.assignment.name,
                    node->as.assignment.name_length, indent, &id);
    emit_expression(e, node->as.assignment.value);
    emit_store_close(e, symbol, node->as.assignment.name,
                     node->as.assignment.name_length, indent, id);
    break;
  }

  case AST_INDEX_ASSIGN: {
    /* Target, index and value are three ordered evaluations, so they go
     * through a temp array like a three-argument call. */
    int id = e->temp_counter++;
    indent_by(e, indent);
    fprintf(e->out, "_t%d[0] = ", id);
    emit_expression(e, node->as.index_assignment.target);
    fprintf(e->out, ", _t%d[1] = ", id);
    emit_expression(e, node->as.index_assignment.index);
    fprintf(e->out, ", _t%d[2] = ", id);
    emit_expression(e, node->as.index_assignment.value);
    fprintf(e->out, ", HDIndexSetX(_t%d[0], _t%d[1], _t%d[2]);\n", id, id, id);
    break;
  }

  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      emit_statement(e, node->as.block.statements[i], indent);
    break;

  case AST_IF:
    indent_by(e, indent);
    fputs("if (HDTruthy(", e->out);
    emit_expression(e, node->as.if_statement.condition);
    fputs(")) {\n", e->out);
    emit_body(e, node->as.if_statement.then_branch, indent + 1);
    indent_by(e, indent);
    fputs("}\n", e->out);
    if (node->as.if_statement.else_branch) {
      indent_by(e, indent);
      fputs("else {\n", e->out);
      emit_body(e, node->as.if_statement.else_branch, indent + 1);
      indent_by(e, indent);
      fputs("}\n", e->out);
    }
    break;

  case AST_WHILE:
    indent_by(e, indent);
    fputs("while (HDTruthy(", e->out);
    emit_expression(e, node->as.while_statement.condition);
    fputs(")) {\n", e->out);
    emit_body(e, node->as.while_statement.body, indent + 1);
    indent_by(e, indent);
    fputs("}\n", e->out);
    break;

  /* Written as init + while rather than a C for, because the init and the
   * increment are statements in HolyD, and because it keeps the order the
   * VM uses visible: condition, body, increment. */
  case AST_FOR:
    indent_by(e, indent);
    fputs("{\n", e->out);
    if (node->as.for_statement.initializer)
      emit_statement(e, node->as.for_statement.initializer, indent + 1);
    indent_by(e, indent + 1);
    if (node->as.for_statement.condition) {
      fputs("while (HDTruthy(", e->out);
      emit_expression(e, node->as.for_statement.condition);
      fputs(")) {\n", e->out);
    } else {
      fputs("while (1) {\n", e->out);
    }
    emit_body(e, node->as.for_statement.body, indent + 2);
    if (node->as.for_statement.increment)
      emit_statement(e, node->as.for_statement.increment, indent + 2);
    indent_by(e, indent + 1);
    fputs("}\n", e->out);
    indent_by(e, indent);
    fputs("}\n", e->out);
    break;

  /* The array is snapshotted once and the length re-read each turn, which
   * is what the VM's desugaring does: it stores the array in a temporary
   * and applies BC_ARRAY_LENGTH to that temporary every iteration. The
   * element is defined before the index, in that order. */
  case AST_FOREACH: {
    int id = e->temp_counter++;
    indent_by(e, indent);
    fputs("{\n", e->out);
    indent_by(e, indent + 1);
    fprintf(e->out, "_t%d[0] = ", id);
    emit_expression(e, node->as.foreach_statement.array_expression);
    fputs(";\n", e->out);
    indent_by(e, indent + 1);
    fprintf(e->out, "long long _fe%d = 0;\n", id);
    indent_by(e, indent + 1);
    fprintf(e->out,
            "while (_fe%d < HDLengthX(_t%d[0]).i64) {\n", id, id);

    const HDSymbol *value_symbol =
        symbol_for(e, node, HD_BINDING_FOREACH_VALUE);
    emit_define_open(e, value_symbol, node->as.foreach_statement.variable_name,
                     node->as.foreach_statement.variable_name_length,
                     indent + 2);
    fprintf(e->out, "HDIndexX(_t%d[0], int_value(_fe%d))", id, id);
    emit_define_close(e, value_symbol);

    if (node->as.foreach_statement.index_name != NULL) {
      const HDSymbol *index_symbol =
          symbol_for(e, node, HD_BINDING_FOREACH_INDEX);
      emit_define_open(e, index_symbol, node->as.foreach_statement.index_name,
                       node->as.foreach_statement.index_name_length,
                       indent + 2);
      fprintf(e->out, "int_value(_fe%d)", id);
      emit_define_close(e, index_symbol);
    }

    emit_body(e, node->as.foreach_statement.body, indent + 2);

    indent_by(e, indent + 2);
    fprintf(e->out, "_fe%d++;\n", id);
    indent_by(e, indent + 1);
    fputs("}\n", e->out);
    indent_by(e, indent);
    fputs("}\n", e->out);
    break;
  }

  case AST_FUNC_DECL:
    break;

  /* A trailing `;` because C wants a statement after a label, and a label
   * is allowed to be the last thing in a block. */
  case AST_LABEL:
    indent_by(e, indent);
    write_label_name(e, HDResolutionLabelIndex(e->resolution, node));
    fputs(": ;\n", e->out);
    break;

  /* The resolver has already established that the label exists, is in this
   * function, and is not inside a foreach body this goto sits outside of ,
   * which is what makes a plain C goto a faithful lowering. */
  case AST_GOTO: {
    int label_index = HDResolutionGotoLabel(e->resolution, node);
    if (label_index < 0) {
      emit_error(e, "goto was not resolved to a label.");
      break;
    }
    indent_by(e, indent);
    fputs("goto ", e->out);
    write_label_name(e, label_index);
    fputs(";\n", e->out);
    break;
  }

  case AST_RETURN:
    indent_by(e, indent);
    fputs("return ", e->out);
    emit_expression(e, node->as.return_statement.expression);
    fputs(";\n", e->out);
    break;

  /* A bare string statement is HolyC's implicit print. */
  case AST_STRING: {
    int id = e->temp_counter++;
    indent_by(e, indent);
    fprintf(e->out, "_t%d[0] = ", id);
    emit_expression(e, node);
    fprintf(e->out, ", HDPrintN(1, _t%d, 0);\n", id);
    break;
  }

  case AST_VAR_REF: {
    int index = find_function(e, node->as.variable_ref.name, node->as.variable_ref.name_length);
    indent_by(e, indent);
    if (index >= 0) {
      ASTNode *fn = e->functions[index];
      e->temp_counter++; /* the slot count_statement reserved */
      if (fn->as.function_decl.parameter_count != 0) {
        fputs("(void)HDArityX(", e->out);
        write_c_string(e, node->as.variable_ref.name, node->as.variable_ref.name_length);
        fprintf(e->out, ", %d, %d, 0);\n", node->as.variable_ref.name_length,
                fn->as.function_decl.parameter_count);
      } else {
        fputs("(void)", e->out);
        write_mangled(e, index, fn->as.function_decl.name, fn->as.function_decl.name_length);
        fputs("();\n", e->out);
      }
    } else {
      fputs("(void)", e->out);
      emit_expression(e, node);
      fputs(";\n", e->out);
    }
    break;
  }

  default:
    indent_by(e, indent);
    fputs("(void)", e->out);
    emit_expression(e, node);
    fputs(";\n", e->out);
    break;
  }
}

/* ---------------- Entry point detection ----------------------------------- */

/* Does a top-level statement call `name`? Mirrors chunk_calls, which scans
 * only the top-level chunk , a call inside a function body is a different
 * chunk and does not count, which is what lets a recursive main() still be
 * started. Statement position matters: a bare name is a call only when it
 * names a function, so this walks statements rather than every node. */
static int stmt_calls(Emitter *e, ASTNode *node, const char *name, int len);

static int expr_calls(Emitter *e, ASTNode *node, const char *name, int len) {
  if (!node)
    return 0;
  switch (node->type) {
  case AST_BINARY_OP:
    return expr_calls(e, node->as.binary_op.left, name, len) ||
           expr_calls(e, node->as.binary_op.right, name, len);
  case AST_INDEX:
    return expr_calls(e, node->as.index_expr.target, name, len) ||
           expr_calls(e, node->as.index_expr.index, name, len);
  case AST_UNARY_OP:
    return expr_calls(e, node->as.unary_op.operand, name, len);
  case AST_ARRAY_LEN_EXPR:
    return expr_calls(e, node->as.array_length_expr.target, name, len);
  case AST_CALL:
    if (name_is(node->as.call.callee_name, node->as.call.callee_name_length, "[array]")) {
      /* not a call to a named function, but its arguments still are */
    } else if (node->as.call.callee_name_length == len &&
               strncmp(node->as.call.callee_name, name, (size_t)len) == 0) {
      return 1;
    }
    for (int i = 0; i < node->as.call.argument_count; i++)
      if (expr_calls(e, node->as.call.arguments[i], name, len))
        return 1;
    return 0;
  default:
    return 0;
  }
}

static int stmt_calls(Emitter *e, ASTNode *node, const char *name, int len) {
  if (!node)
    return 0;
  switch (node->type) {
  /* Separate cases: a declaration keeps its expression in
   * variable_decl.initializer and an assignment in assignment.value, and
   * those are different members of the same union. Sharing one label reads
   * whichever member the other node type never wrote. */
  case AST_VAR_DECL:
    return expr_calls(e, node->as.variable_decl.initializer, name, len);
  case AST_ASSIGN:
    return expr_calls(e, node->as.assignment.value, name, len);
  case AST_INDEX_ASSIGN:
    return expr_calls(e, node->as.index_assignment.target, name, len) ||
           expr_calls(e, node->as.index_assignment.index, name, len) ||
           expr_calls(e, node->as.index_assignment.value, name, len);
  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      if (stmt_calls(e, node->as.block.statements[i], name, len))
        return 1;
    return 0;
  case AST_IF:
    return expr_calls(e, node->as.if_statement.condition, name, len) ||
           stmt_calls(e, node->as.if_statement.then_branch, name, len) ||
           stmt_calls(e, node->as.if_statement.else_branch, name, len);
  case AST_WHILE:
    return expr_calls(e, node->as.while_statement.condition, name, len) ||
           stmt_calls(e, node->as.while_statement.body, name, len);
  case AST_FOR:
    return stmt_calls(e, node->as.for_statement.initializer, name, len) ||
           expr_calls(e, node->as.for_statement.condition, name, len) ||
           stmt_calls(e, node->as.for_statement.body, name, len) ||
           stmt_calls(e, node->as.for_statement.increment, name, len);
  case AST_FOREACH:
    return expr_calls(e, node->as.foreach_statement.array_expression, name, len) ||
           stmt_calls(e, node->as.foreach_statement.body, name, len);
  case AST_RETURN:
    return expr_calls(e, node->as.return_statement.expression, name, len);
  case AST_FUNC_DECL:
    return 0;
  case AST_VAR_REF:
    /* Only a call when it names a function. */
    return find_function(e, node->as.variable_ref.name, node->as.variable_ref.name_length) >= 0 &&
           node->as.variable_ref.name_length == len &&
           strncmp(node->as.variable_ref.name, name, (size_t)len) == 0;
  default:
    return expr_calls(e, node, name, len);
  }
}

/* ---------------- Translation unit ---------------------------------------- */

/* One C declaration per frame slot. Parameters take their argument and no
 * bound bit; everything else starts unbound, so a read before the
 * declaration runs falls through to the enclosing scope the way it does in
 * the VM.
 *
 * Two parameters of the same name resolve to one symbol and so share a
 * slot. Initialising from the last of them leaves it holding the slot, as
 * repeated EnvDefine calls did. */
static void declare_frame(Emitter *e, const HDResolvedFunction *scope,
                          int indent) {
  for (int slot = 0; slot < scope->frame_slot_count; slot++) {
    const HDSymbol *symbol =
        HDResolutionSlotSymbol(e->resolution, e->scope, slot);
    if (!symbol)
      continue;

    int parameter = -1;
    for (int i = 0; i < scope->parameter_count; i++) {
      if (scope->parameter_slots[i] == slot)
        parameter = i;
    }

    indent_by(e, indent);
    fputs("HDValue ", e->out);
    write_slot_name(e, symbol);
    if (parameter >= 0)
      fprintf(e->out, " = p%d;\n", parameter);
    else
      fputs(" = {0};\n", e->out);

    if (has_bound_bit(symbol)) {
      indent_by(e, indent);
      fputs("int ", e->out);
      write_bound_name(e, symbol);
      fputs(" = 0;\n", e->out);
    }

    /* A slot the body only ever writes is still a slot. Say so, rather than
     * hand the C compiler an unused-variable warning about it. */
    if (!HDResolutionSymbolIsRead(e->resolution, symbol->id)) {
      indent_by(e, indent);
      fputs("(void)", e->out);
      write_slot_name(e, symbol);
      fputs(";\n", e->out);
    }
  }
}

static void emit_function(Emitter *e, int index) {
  ASTNode *fn = e->functions[index];

  fputs("static HDValue ", e->out);
  write_mangled(e, index, fn->as.function_decl.name, fn->as.function_decl.name_length);
  fputc('(', e->out);
  if (fn->as.function_decl.parameter_count == 0) {
    fputs("void", e->out);
  } else {
    for (int i = 0; i < fn->as.function_decl.parameter_count; i++)
      fprintf(e->out, "%sHDValue p%d", i ? ", " : "", i);
  }
  fputs(") {\n", e->out);

  e->scope = HDResolutionFunctionIndex(e->resolution, fn);
  e->store_counter = 0;
  if (e->scope >= 0)
    declare_frame(e, &e->resolution->functions[e->scope], 1);

  declare_temps(e, fn->as.function_decl.body, 1);
  fputc('\n', e->out);

  emit_body(e, fn->as.function_decl.body, 1);

  /* Falling off the end is the VM's implicit `return 0`. */
  fputs("  return int_value(0);\n", e->out);
  fputs("}\n\n", e->out);
  e->scope = -1;
}

int HDEmitC(ASTNode *ast, const HDResolution *resolution, FILE *out,
            const char *source_name) {
  Emitter e;
  e.out = out;
  e.resolution = resolution;
  e.functions = NULL;
  e.function_count = 0;
  e.temp_counter = 0;
  e.store_counter = 0;
  e.scope = -1;
  e.had_error = 0;

  if (!ast || ast->type != AST_BLOCK) {
    emit_error(&e, "program root is not a block.");
    return 0;
  }

  /* Collect functions first: a call can name one declared further down. */
  e.functions = (ASTNode **)malloc(sizeof(ASTNode *) * (size_t)ast->as.block.statement_count
                                   + sizeof(ASTNode *));
  if (!e.functions) {
    emit_error(&e, "out of memory while collecting functions.");
    return 0;
  }
  for (int i = 0; i < ast->as.block.statement_count; i++) {
    ASTNode *stmt = ast->as.block.statements[i];
    if (stmt && stmt->type == AST_FUNC_DECL)
      e.functions[e.function_count++] = stmt;
  }

  fprintf(out, "/* Generated by holyd --emit-c from %s. Do not edit.\n",
          source_name);
  fputs(" *\n"
        " * Build alongside the HolyD runtime, from the repository root:\n"
        " *\n"
        " *   gcc -std=gnu11 -O2 -I src <this file> \\\n"
        " *       src/runtime.c src/eval.c src/ffi.c src/ffi_win32.c \\\n"
        " *       src/platform/standalone/gfx.c src/platform/standalone/bmp.c \\\n"
        " *       -o program.exe -lgdi32 -luser32 -lws2_32\n"
        " */\n\n",
        out);
  fputs("#include \"runtime.h\"\n", out);
  fputs("#include \"ffi.h\"\n", out);
  fputs("#include <stdlib.h>\n\n", out);
  /* The Environment is no longer where variables live. It holds the FFI
   * constants, and it is the last link in a name's fallback chain, which is
   * both where those constants are found and where an undefined name
   * finally becomes an error. */
  fputs("static Environment hd_globals;\n\n", out);

  /* One static per global slot. Zero-initialised, so nothing is bound until
   * a declaration or an assignment runs, exactly as in the VM. */
  if (resolution->global_slot_count > 0) {
    for (int slot = 0; slot < resolution->global_slot_count; slot++) {
      const HDSymbol *symbol = HDResolutionSlotSymbol(resolution, -1, slot);
      if (!symbol)
        continue;
      fputs("static HDValue ", out);
      write_slot_name(&e, symbol);
      fputs(";\n", out);
      fputs("static int ", out);
      write_bound_name(&e, symbol);
      fputs(";\n", out);
    }
    fputc('\n', out);
  }

  /* Prototypes ahead of bodies, so functions may call each other in any
   * order and recursion works. */
  if (e.function_count > 0) {
    for (int i = 0; i < e.function_count; i++) {
      ASTNode *fn = e.functions[i];
      fputs("static HDValue ", out);
      write_mangled(&e, i, fn->as.function_decl.name, fn->as.function_decl.name_length);
      fputc('(', out);
      if (fn->as.function_decl.parameter_count == 0) {
        fputs("void", out);
      } else {
        for (int p = 0; p < fn->as.function_decl.parameter_count; p++)
          fprintf(out, "%sHDValue", p ? ", " : "");
      }
      fputs(");\n", out);
    }
    fputc('\n', out);
  }

  for (int i = 0; i < e.function_count; i++)
    emit_function(&e, i);

  /* The top level runs in the globals environment itself, exactly as the
   * VM's main chunk does. */
  fputs("int main(void) {\n", out);
  fputs("  EnvInit(&hd_globals);\n", out);
  fputs("  /* Before the program, so a script that assigns over EV_KEY_DOWN "
        "wins. */\n",
        out);
  fputs("  ffi_define_globals(&hd_globals);\n", out);

  /* One synthetic block holding every top-level statement, so the temp
   * pre-pass sees them the way it sees a function body. */
  ASTNode top;
  top.type = AST_BLOCK;
  top.as.block.statements = NULL;
  top.as.block.statement_count = 0;
  {
    int count = 0;
    ASTNode **stmts =
        (ASTNode **)malloc(sizeof(ASTNode *) * (size_t)(ast->as.block.statement_count + 1));
    if (!stmts) {
      emit_error(&e, "out of memory while collecting top-level statements.");
      free(e.functions);
      return 0;
    }
    for (int i = 0; i < ast->as.block.statement_count; i++) {
      ASTNode *stmt = ast->as.block.statements[i];
      if (!stmt || stmt->type == AST_FUNC_DECL)
        continue;
      if (stmt->type == AST_BLOCK && stmt->as.block.statement_count == 0)
        continue;
      stmts[count++] = stmt;
    }
    top.as.block.statements = stmts;
    top.as.block.statement_count = count;
  }

  e.scope = -1;
  e.store_counter = 0;
  declare_temps(&e, &top, 1);
  fputc('\n', out);

  for (int i = 0; i < top.as.block.statement_count; i++)
    emit_statement(&e, top.as.block.statements[i], 1);

  /* Entry point, on the same terms as the VM: main then Main, only when it
   * takes no parameters and the top level has not already called it. */
  int entry = find_function(&e, "main", 4);
  if (entry < 0)
    entry = find_function(&e, "Main", 4);
  if (entry >= 0) {
    ASTNode *fn = e.functions[entry];
    int already = 0;
    for (int i = 0; i < top.as.block.statement_count && !already; i++)
      already = stmt_calls(&e, top.as.block.statements[i], fn->as.function_decl.name,
                           fn->as.function_decl.name_length);
    if (fn->as.function_decl.parameter_count == 0 && !already) {
      fputs("  (void)", out);
      write_mangled(&e, entry, fn->as.function_decl.name, fn->as.function_decl.name_length);
      fputs("();\n", out);
    }
  }

  fputs("  return 0;\n", out);
  fputs("}\n", out);

  free(top.as.block.statements);
  free(e.functions);
  return !e.had_error;
}
