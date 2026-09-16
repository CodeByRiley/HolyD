/* HolyD -> x86-64 assembly, GNU assembler syntax, Win64 ABI.
 *
 * See emit_asm.h for what this backend is for. Four things shape the output.
 *
 * Values travel as addresses. An HDValue is 56 bytes, and both Win64 and
 * SysV pass anything over 16 bytes in memory and return it through a hidden
 * pointer the caller supplies. So every expression is compiled as "write
 * your result at this address", every call that yields a value is handed a
 * destination, and every argument is the address of a slot the caller owns.
 * None of the register classification a smaller struct would need comes up.
 *
 * Scratch is a stack, not a sequence. Each expression is given a destination
 * and the index of the first free scratch slot above it; its children take
 * slots from there. A pre-pass computes the high-water mark per function, so
 * the frame is sized by a maximum rather than by counting call sites in an
 * order two separate walks have to agree on. That is deliberately unlike
 * emit_c.c's _tN temporaries: a maximum cannot drift out of step.
 *
 * The frame is fixed at entry and never moves. Locals, their bound bits, the
 * scratch region and the outgoing-argument area are carved out by one `sub`
 * in the prologue, so outgoing arguments are written at fixed offsets from
 * %rsp and everything else is addressed from %rbp.
 *
 * Only volatile registers are touched. Win64 makes RSI and RDI callee-saved,
 * unlike SysV, so the block copies use R10/R11 and this backend never has to
 * save or restore anything but RBP.
 */

#include "emit_asm.h"
#include "ffi.h"
#include "runtime.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Taken from the header the runtime is built from rather than written out,
 * so this backend cannot disagree with the runtime it links against. */
#define HD_VALUE_SIZE ((int)sizeof(HDValue))
#define HD_VALUE_I64 ((int)offsetof(HDValue, i64))

/* Every runtime entry point this backend calls fits in five argument slots,
 * so no frame ever needs a narrower outgoing area than that. */
#define HD_MIN_ARGS 5

/* A memory operand, spelled out. Cheap to copy and impossible to dangle,
 * which matters because these are built and discarded constantly. */
typedef struct {
  char text[96];
} Loc;

typedef struct {
  FILE *out;
  const HDResolution *resolution;
  ASTNode **functions;
  int function_count;

  /* The function being emitted. All frame offsets are negative from %rbp. */
  int scope; /* resolver function index, or -1 at the top level */
  int in_main;
  int locals_base;
  int bound_base;
  int scratch_base;
  int sret_off;
  int frame_size;
  int max_args; /* widest outgoing call, sizing the argument area */

  int next_label;
  /* Where a break and a continue in the innermost loop jump, or -1 outside
   * any loop. Saved and restored around each loop, so nesting works without
   * a separate stack. */
  int break_label;
  int continue_label;
  int had_error;
} Asm;

static void emit_expr(Asm *a, ASTNode *node, Loc dest, int next);
static void emit_stmt(Asm *a, ASTNode *node, int next);

static void emit_error(Asm *a, const char *message) {
  printf("Emit error: %s\n", message);
  a->had_error = 1;
}

static int name_is(const char *a, int a_len, const char *b) {
  int b_len = (int)strlen(b);
  return a_len == b_len && strncmp(a, b, (size_t)a_len) == 0;
}

static int find_function(Asm *a, const char *name, int len) {
  for (int i = 0; i < a->function_count; i++) {
    ASTNode *fn = a->functions[i];
    if (fn->as.function_decl.name_length == len &&
        strncmp(fn->as.function_decl.name, name, (size_t)len) == 0)
      return i;
  }
  return -1;
}

/* Mirrors is_print_builtin in compiler.c and emit_c.c, including the order
 * it is checked in: a print name wins over a native and over a user
 * function. */
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

/* ---------------- Names and locations ------------------------------------ */

static void sanitize_into(char *buf, size_t size, const char *name, int len) {
  size_t out = 0;
  for (int i = 0; i < len && out + 1 < size; i++) {
    char c = name[i];
    buf[out++] = (isalnum((unsigned char)c) || c == '_') ? c : '_';
  }
  buf[out] = '\0';
}

/* One symbol per global slot and one per function. The index keeps them
 * unique whatever the script called things; the name keeps the listing
 * readable. */
static void global_symbol(Asm *a, int slot, char *buf, size_t size) {
  const HDSymbol *symbol = HDResolutionSlotSymbol(a->resolution, -1, slot);
  char clean[40];
  if (!symbol) {
    snprintf(buf, size, "hd_g%d", slot);
    return;
  }
  sanitize_into(clean, sizeof(clean), symbol->name, symbol->name_length);
  snprintf(buf, size, "hd_g%d_%s", slot, clean);
}

static void function_symbol(Asm *a, int index, char *buf, size_t size) {
  ASTNode *fn = a->functions[index];
  char clean[40];
  sanitize_into(clean, sizeof(clean), fn->as.function_decl.name,
                fn->as.function_decl.name_length);
  snprintf(buf, size, "hd_fn_%d_%s", index, clean);
}

static Loc loc_frame(int offset) {
  Loc loc;
  snprintf(loc.text, sizeof(loc.text), "%d(%%rbp)", offset);
  return loc;
}

static Loc loc_rip(const char *symbol) {
  Loc loc;
  snprintf(loc.text, sizeof(loc.text), "%s(%%rip)", symbol);
  return loc;
}

static Loc loc_local(Asm *a, int slot) {
  return loc_frame(a->locals_base + slot * HD_VALUE_SIZE);
}

static Loc loc_bound_local(Asm *a, int slot) {
  return loc_frame(a->bound_base + slot * 8);
}

static int scratch_offset(Asm *a, int index) {
  return a->scratch_base + index * HD_VALUE_SIZE;
}

static Loc loc_scratch(Asm *a, int index) {
  return loc_frame(scratch_offset(a, index));
}

static Loc loc_global(Asm *a, int slot) {
  char symbol[64];
  global_symbol(a, slot, symbol, sizeof(symbol));
  return loc_rip(symbol);
}

static Loc loc_bound_global(Asm *a, int slot) {
  char symbol[64];
  char bound[80];
  global_symbol(a, slot, symbol, sizeof(symbol));
  snprintf(bound, sizeof(bound), "%s_bound", symbol);
  return loc_rip(bound);
}

/* Where a resolved name lives. Only meaningful for a symbol the resolver
 * gave a slot to; an external name has no home but the Environment. */
static Loc loc_symbol(Asm *a, const HDSymbol *symbol) {
  if (symbol->storage == HD_SYMBOL_LOCAL)
    return loc_local(a, symbol->slot);
  return loc_global(a, symbol->slot);
}

static Loc loc_symbol_bound(Asm *a, const HDSymbol *symbol) {
  if (symbol->storage == HD_SYMBOL_LOCAL)
    return loc_bound_local(a, symbol->slot);
  return loc_bound_global(a, symbol->slot);
}

/* Parameters are the one kind of local bound on entry, so they carry no bit
 * and end the fallback chain immediately. */
static int has_bound_bit(const HDSymbol *symbol) {
  return !(symbol->flags & HD_SYMBOL_PARAMETER);
}

static const HDSymbol *global_fallback(Asm *a, const HDSymbol *symbol) {
  const HDSymbol *fallback =
      HDResolutionSymbol(a->resolution, symbol->fallback_id);
  if (fallback && fallback->storage == HD_SYMBOL_GLOBAL)
    return fallback;
  return NULL;
}

static const HDSymbol *symbol_for(Asm *a, const ASTNode *node,
                                  HDBindingRole role) {
  const HDBinding *binding = HDResolutionBinding(a->resolution, node, role);
  if (!binding)
    return NULL;
  return HDResolutionSymbol(a->resolution, binding->symbol_id);
}

/* ---------------- Constants ----------------------------------------------- */

/* Source bytes verbatim, as a byte list rather than a quoted string: the
 * runtime unescapes at print time exactly as it does for the VM, so what
 * reaches .rodata has to be the bytes the VM holds, and a byte list cannot
 * be reinterpreted by an assembler's own idea of escapes. */
static int emit_string_constant(Asm *a, const char *text, int length) {
  int label = a->next_label++;
  fputs("\t.section .rodata\n", a->out);
  fprintf(a->out, ".LC%d:\n", label);
  for (int i = 0; i < length; i++) {
    if (i % 16 == 0)
      fputs(i ? "\n\t.byte " : "\t.byte ", a->out);
    else
      fputs(", ", a->out);
    fprintf(a->out, "%d", (unsigned char)text[i]);
  }
  fputs(length > 0 ? "\n\t.byte 0\n" : "\t.byte 0\n", a->out);
  fputs("\t.text\n", a->out);
  return label;
}

/* The bit pattern, not the digits: there is no reason to route a double
 * through a second parser when the bits are already here. */
static int emit_double_constant(Asm *a, double value) {
  unsigned long long bits;
  int label = a->next_label++;
  memcpy(&bits, &value, sizeof(bits));
  fputs("\t.section .rodata\n\t.balign 8\n", a->out);
  fprintf(a->out, ".LC%d:\n\t.quad %llu\n", label, bits);
  fputs("\t.text\n", a->out);
  return label;
}

/* ---------------- Instructions -------------------------------------------- */

/* Register names always arrive as %s arguments, never inside a format
 * string, so a stray %r can never be read as a conversion. */
static const char *arg_reg64(int n) {
  static const char *regs[4] = {"%rcx", "%rdx", "%r8", "%r9"};
  return regs[n];
}

static const char *arg_reg32(int n) {
  static const char *regs[4] = {"%ecx", "%edx", "%r8d", "%r9d"};
  return regs[n];
}

/* Win64 numbers argument slots across the whole call: slots 0-3 are
 * RCX/RDX/R8/R9, and slot n from 4 up sits at 8n(%rsp), above the 32 bytes
 * of shadow space the register arguments still reserve. A value returned in
 * memory takes slot 0, which is why every argument here sits one slot
 * further along than the source suggests. */
static void arg_addr(Asm *a, int n, Loc loc) {
  if (n < 4) {
    fprintf(a->out, "\tleaq %s, %s\n", loc.text, arg_reg64(n));
  } else {
    fprintf(a->out, "\tleaq %s, %s\n", loc.text, "%rax");
    fprintf(a->out, "\tmovq %s, %d(%%rsp)\n", "%rax", n * 8);
  }
}

static void arg_i32(Asm *a, int n, int value) {
  if (n < 4)
    fprintf(a->out, "\tmovl $%d, %s\n", value, arg_reg32(n));
  else
    fprintf(a->out, "\tmovl $%d, %d(%%rsp)\n", value, n * 8);
}

static void arg_i64(Asm *a, int n, long long value) {
  if (n < 4) {
    fprintf(a->out, "\tmovabsq $%lld, %s\n", value, arg_reg64(n));
  } else {
    fprintf(a->out, "\tmovabsq $%lld, %s\n", value, "%rax");
    fprintf(a->out, "\tmovq %s, %d(%%rsp)\n", "%rax", n * 8);
  }
}

static void call(Asm *a, const char *symbol) {
  fprintf(a->out, "\tcall %s\n", symbol);
}

/* Seven quadwords from %r10 to %r11 through %rax, all volatile. Inline
 * rather than a helper call, because a call would need its own argument
 * area; the code size is the honest cost of boxing every value. */
static void copy_bytes(Asm *a) {
  for (int offset = 0; offset < HD_VALUE_SIZE; offset += 8) {
    fprintf(a->out, "\tmovq %d(%%r10), %s\n", offset, "%rax");
    fprintf(a->out, "\tmovq %s, %d(%%r11)\n", "%rax", offset);
  }
}

static void copy_value(Asm *a, Loc dest, Loc src) {
  fprintf(a->out, "\tleaq %s, %s\n", src.text, "%r10");
  fprintf(a->out, "\tleaq %s, %s\n", dest.text, "%r11");
  copy_bytes(a);
}

/* Through the hidden pointer the caller gave us, for `return`. */
static void copy_to_sret(Asm *a, Loc src) {
  fprintf(a->out, "\tleaq %s, %s\n", src.text, "%r10");
  fprintf(a->out, "\tmovq %d(%%rbp), %s\n", a->sret_off, "%r11");
  copy_bytes(a);
}

static void copy_from_pointer(Asm *a, const char *pointer, Loc dest) {
  fprintf(a->out, "\tmovq %s, %s\n", pointer, "%r10");
  fprintf(a->out, "\tleaq %s, %s\n", dest.text, "%r11");
  copy_bytes(a);
}

static void set_bound(Asm *a, Loc bound, int value) {
  fprintf(a->out, "\tmovq $%d, %s\n", value, bound.text);
}

static int new_label(Asm *a) { return a->next_label++; }

static void place_label(Asm *a, int label) {
  fprintf(a->out, ".L%d:\n", label);
}

static void jump(Asm *a, int label) { fprintf(a->out, "\tjmp .L%d\n", label); }

/* HDTruthy takes one HDValue and returns int, so the value goes by address
 * in slot 0 and the answer comes back in EAX. */
static void emit_truthy(Asm *a, Loc value) {
  arg_addr(a, 0, value);
  call(a, "HDTruthy");
  fputs("\ttestl %eax, %eax\n", a->out);
}

/* ---------------- Reading and writing names ------------------------------- */

static void emit_name_args(Asm *a, int first, const char *name, int length) {
  int label = emit_string_constant(a, name, length);
  char text[32];
  snprintf(text, sizeof(text), ".LC%d", label);
  arg_addr(a, first, loc_rip(text));
  arg_i32(a, first + 1, length);
}

/* dest = what the name means right now: this frame, then a global of the
 * same name, then the Environment, which is where the FFI constants live and
 * where an undefined name finally becomes an error. The same chain emit_c.c
 * writes as nested conditionals. */
static void emit_load(Asm *a, const HDSymbol *symbol, const char *name,
                      int length, Loc dest) {
  if (!symbol || symbol->storage == HD_SYMBOL_EXTERNAL) {
    arg_addr(a, 0, dest);
    arg_addr(a, 1, loc_rip("hd_globals"));
    emit_name_args(a, 2, name, length);
    call(a, "HDLoadX");
    return;
  }

  if (!has_bound_bit(symbol)) {
    copy_value(a, dest, loc_symbol(a, symbol));
    return;
  }

  const HDSymbol *fallback = global_fallback(a, symbol);
  int done = new_label(a);
  int unbound = new_label(a);

  fprintf(a->out, "\tcmpq $0, %s\n", loc_symbol_bound(a, symbol).text);
  fprintf(a->out, "\tje .L%d\n", unbound);
  copy_value(a, dest, loc_symbol(a, symbol));
  jump(a, done);

  place_label(a, unbound);
  if (fallback) {
    int external = new_label(a);
    fprintf(a->out, "\tcmpq $0, %s\n", loc_symbol_bound(a, fallback).text);
    fprintf(a->out, "\tje .L%d\n", external);
    copy_value(a, dest, loc_symbol(a, fallback));
    jump(a, done);
    place_label(a, external);
  }
  arg_addr(a, 0, dest);
  arg_addr(a, 1, loc_rip("hd_globals"));
  emit_name_args(a, 2, name, length);
  call(a, "HDLoadX");

  place_label(a, done);
}

/* An assignment writes to whatever the name already means, and only binds a
 * new slot when the name means nothing yet. */
static void emit_store(Asm *a, const HDSymbol *symbol, const char *name,
                       int length, Loc value) {
  if (!symbol || symbol->storage == HD_SYMBOL_EXTERNAL) {
    arg_addr(a, 0, loc_rip("hd_globals"));
    emit_name_args(a, 1, name, length);
    arg_addr(a, 3, value);
    call(a, "EnvSet");
    return;
  }

  if (!has_bound_bit(symbol)) {
    copy_value(a, loc_symbol(a, symbol), value);
    return;
  }

  const HDSymbol *fallback = global_fallback(a, symbol);
  int done = new_label(a);
  int own = new_label(a);
  int bind = new_label(a);

  fprintf(a->out, "\tcmpq $0, %s\n", loc_symbol_bound(a, symbol).text);
  fprintf(a->out, "\tjne .L%d\n", own);

  if (fallback) {
    int not_global = new_label(a);
    fprintf(a->out, "\tcmpq $0, %s\n", loc_symbol_bound(a, fallback).text);
    fprintf(a->out, "\tje .L%d\n", not_global);
    copy_value(a, loc_symbol(a, fallback), value);
    jump(a, done);
    place_label(a, not_global);
  }

  /* Nothing in the program has bound the name, so the Environment decides:
   * an FFI constant of that name is written through, and only a name nothing
   * knows about binds a slot here. */
  arg_addr(a, 0, loc_rip("hd_globals"));
  emit_name_args(a, 1, name, length);
  call(a, "EnvGet");
  fputs("\ttestq %rax, %rax\n", a->out);
  fprintf(a->out, "\tjz .L%d\n", bind);
  arg_addr(a, 0, loc_rip("hd_globals"));
  emit_name_args(a, 1, name, length);
  arg_addr(a, 3, value);
  call(a, "EnvSet");
  jump(a, done);

  place_label(a, bind);
  copy_value(a, loc_symbol(a, symbol), value);
  set_bound(a, loc_symbol_bound(a, symbol), 1);
  jump(a, done);

  place_label(a, own);
  copy_value(a, loc_symbol(a, symbol), value);
  place_label(a, done);
}

/* Binds a name to a value already computed, for the two variables a foreach
 * declares each turn. */
static void emit_bind(Asm *a, const HDSymbol *symbol, const char *name,
                      int length, Loc value) {
  if (!symbol || symbol->storage == HD_SYMBOL_EXTERNAL) {
    arg_addr(a, 0, loc_rip("hd_globals"));
    emit_name_args(a, 1, name, length);
    arg_addr(a, 3, value);
    call(a, "EnvDefine");
    return;
  }
  copy_value(a, loc_symbol(a, symbol), value);
  if (has_bound_bit(symbol))
    set_bound(a, loc_symbol_bound(a, symbol), 1);
}

/* ---------------- Scratch sizing ------------------------------------------ */

static int scratch_for_expr(Asm *a, ASTNode *node);
static int scratch_for_stmt(Asm *a, ASTNode *node);

static int max_int(int left, int right) { return left > right ? left : right; }

static void note_args(Asm *a, int slots) {
  if (slots > a->max_args)
    a->max_args = slots;
}

static int scratch_for_expr(Asm *a, ASTNode *node) {
  if (!node)
    return 0;
  switch (node->type) {
  case AST_NUMBER:
  case AST_FLOAT:
  case AST_STRING:
  case AST_VAR_REF:
    return 0;

  case AST_BINARY_OP: {
    TokenType op = node->as.binary_op.operator_type;
    int children = max_int(scratch_for_expr(a, node->as.binary_op.left),
                           scratch_for_expr(a, node->as.binary_op.right));
    /* Short-circuit reuses one slot for both sides; every other operator
     * needs both operands live at the call. */
    return (op == TOKEN_ANDAND || op == TOKEN_OROR) ? 1 + children
                                                    : 2 + children;
  }

  case AST_UNARY_OP:
    return (node->as.unary_op.operator_type == TOKEN_MINUS ? 2 : 1) +
           scratch_for_expr(a, node->as.unary_op.operand);

  case AST_CAST:
    return 1 + scratch_for_expr(a, node->as.cast.expression);

  case AST_TERNARY_OP:
    return max_int(
        1 + scratch_for_expr(a, node->as.ternary_op.condition),
        max_int(scratch_for_expr(a, node->as.ternary_op.true_expr),
                scratch_for_expr(a, node->as.ternary_op.false_expr)));

  case AST_CALL: {
    int argc = node->as.call.argument_count;
    int inner = 0;
    for (int i = 0; i < argc; i++)
      inner = max_int(inner, scratch_for_expr(a, node->as.call.arguments[i]));
    note_args(a, argc + 1);
    return argc + inner;
  }

  case AST_ARRAY_LITERAL: {
    int count = node->as.array_literal.element_count;
    int inner = 0;
    for (int i = 0; i < count; i++)
      inner = max_int(inner,
                      scratch_for_expr(a, node->as.array_literal.elements[i]));
    note_args(a, count + 1);
    return count + inner;
  }

  case AST_INDEX:
    return 2 + max_int(scratch_for_expr(a, node->as.index_expr.target),
                       scratch_for_expr(a, node->as.index_expr.index));

  case AST_ARRAY_LEN_EXPR:
    return 1 + scratch_for_expr(a, node->as.array_length_expr.target);

  default:
    return 0;
  }
}

static int scratch_for_stmt(Asm *a, ASTNode *node) {
  if (!node)
    return 0;
  switch (node->type) {
  case AST_VAR_DECL:
    /* Evaluated straight into its slot when it has one; the extra slot is
     * for an external name, which has to be staged for EnvDefine. */
    return 1 + scratch_for_expr(a, node->as.variable_decl.initializer);

  case AST_ASSIGN:
    return 1 + scratch_for_expr(a, node->as.assignment.value);

  case AST_INDEX_ASSIGN:
    return 3 +
           max_int(scratch_for_expr(a, node->as.index_assignment.target),
                   max_int(scratch_for_expr(a, node->as.index_assignment.index),
                           scratch_for_expr(a, node->as.index_assignment.value)));

  case AST_BLOCK: {
    int most = 0;
    for (int i = 0; i < node->as.block.statement_count; i++)
      most = max_int(most, scratch_for_stmt(a, node->as.block.statements[i]));
    return most;
  }

  case AST_IF:
    return max_int(
        1 + scratch_for_expr(a, node->as.if_statement.condition),
        max_int(scratch_for_stmt(a, node->as.if_statement.then_branch),
                scratch_for_stmt(a, node->as.if_statement.else_branch)));

  case AST_WHILE:
    return max_int(1 + scratch_for_expr(a, node->as.while_statement.condition),
                   scratch_for_stmt(a, node->as.while_statement.body));

  case AST_FOR:
    return max_int(
        max_int(scratch_for_stmt(a, node->as.for_statement.initializer),
                1 + scratch_for_expr(a, node->as.for_statement.condition)),
        max_int(scratch_for_stmt(a, node->as.for_statement.body),
                scratch_for_stmt(a, node->as.for_statement.increment)));

  /* Four slots stay reserved for the whole loop: the array, the counter,
   * somewhere to stage each turn's length and element, and the boxed index
   * HDIndexX needs. The body builds above all of them, which is why this
   * adds rather than takes a maximum with them. */
  case AST_FOREACH:
    return 4 + max_int(scratch_for_expr(
                           a, node->as.foreach_statement.array_expression),
                       scratch_for_stmt(a, node->as.foreach_statement.body));

  case AST_RETURN:
    return 1 + scratch_for_expr(a, node->as.return_statement.expression);

  /* A bare string is HolyC's implicit print: one slot for the argument
   * array and one for the discarded result. */
  case AST_STRING:
    note_args(a, 4);
    return 2;

  case AST_VAR_REF:
    note_args(a, 1);
    return 1;

  case AST_FUNC_DECL:
  case AST_LABEL:
  case AST_GOTO:
  case AST_BREAK:
  case AST_CONTINUE:
    return 0;

  default:
    return 1 + scratch_for_expr(a, node);
  }
}

/* ---------------- Calls --------------------------------------------------- */

static int binop_code(TokenType op) {
  switch (op) {
  case TOKEN_PLUS: return HD_ADD;
  case TOKEN_MINUS: return HD_SUB;
  case TOKEN_STAR: return HD_MUL;
  case TOKEN_SLASH: return HD_DIV;
  case TOKEN_PERCENT: return HD_MOD;
  case TOKEN_POW: return HD_POW;
  case TOKEN_EQEQ: return HD_EQ;
  case TOKEN_NEQ: return HD_NE;
  case TOKEN_LT: return HD_LT;
  case TOKEN_GT: return HD_GT;
  case TOKEN_LTEQ: return HD_LE;
  case TOKEN_GTEQ: return HD_GE;
  case TOKEN_AMPERSAND: return HD_BAND;
  case TOKEN_OR: return HD_BOR;
  case TOKEN_XOR: return HD_BXOR;
  case TOKEN_SHL: return HD_SHL;
  case TOKEN_SHR: return HD_SHR;
  case TOKEN_USHR: return HD_USHR;
  case TOKEN_TILDE: return HD_CONCAT;
  default: return -1;
  }
}

/* Same precedence the VM uses: print, then native, then user function. */
static void emit_call(Asm *a, ASTNode *node, Loc dest, int next) {
  const char *name = node->as.call.callee_name;
  int length = node->as.call.callee_name_length;
  int argc = node->as.call.argument_count;
  int add_newline = 0;
  int index = -1;

  int is_print = is_print_builtin(name, length, &add_newline);
  int is_native = !is_print && ffi_lookup_native(name, length) != NULL;

  if (!is_print && !is_native) {
    index = find_function(a, name, length);
    if (index < 0) {
      /* Deferred to run time, as in the VM: a script that never reaches this
       * call still runs, and its arguments are never evaluated. */
      arg_addr(a, 0, dest);
      emit_name_args(a, 1, name, length);
      call(a, "HDUnknownFunctionX");
      return;
    }
    ASTNode *fn = a->functions[index];
    if (fn->as.function_decl.parameter_count != argc) {
      arg_addr(a, 0, dest);
      emit_name_args(a, 1, name, length);
      arg_i32(a, 3, fn->as.function_decl.parameter_count);
      arg_i32(a, 4, argc);
      call(a, "HDArityX");
      return;
    }
  }

  /* Arguments land in consecutive scratch slots, left to right: the order
   * the VM evaluates them in, and the contiguous array the runtime's
   * variadic entry points want. */
  for (int i = 0; i < argc; i++)
    emit_expr(a, node->as.call.arguments[i], loc_scratch(a, next + i),
              next + argc);

  Loc args = loc_scratch(a, next);

  if (is_print) {
    arg_addr(a, 0, dest);
    arg_i32(a, 1, argc);
    arg_addr(a, 2, args);
    arg_i32(a, 3, add_newline);
    call(a, "HDPrintN");
    return;
  }
  if (is_native) {
    arg_addr(a, 0, dest);
    emit_name_args(a, 1, name, length);
    arg_i32(a, 3, argc);
    arg_addr(a, 4, args);
    call(a, "HDNativeX");
    return;
  }

  char symbol[64];
  function_symbol(a, index, symbol, sizeof(symbol));
  arg_addr(a, 0, dest);
  for (int i = 0; i < argc; i++)
    arg_addr(a, i + 1, loc_scratch(a, next + i));
  call(a, symbol);
}

static void emit_array_literal(Asm *a, ASTNode *node, Loc dest, int next) {
  int count = node->as.array_literal.element_count;
  for (int i = 0; i < count; i++) {
    emit_expr(a, node->as.array_literal.elements[i], loc_scratch(a, next + i),
              next + count);
  }
  arg_addr(a, 0, dest);
  arg_i32(a, 1, count);
  arg_addr(a, 2, loc_scratch(a, next));
  call(a, "HDArrayNewX");
}

/* ---------------- Expressions --------------------------------------------- */

static void emit_expr(Asm *a, ASTNode *node, Loc dest, int next) {
  if (a->had_error)
    return;
  if (!node) {
    arg_addr(a, 0, dest);
    arg_i64(a, 1, 0);
    call(a, "int_value");
    return;
  }

  switch (node->type) {
  case AST_NUMBER:
    arg_addr(a, 0, dest);
    arg_i64(a, 1, node->as.integer_literal.value);
    call(a, "int_value");
    break;

  case AST_FLOAT: {
    int label = emit_double_constant(a, node->as.float_literal.value);
    arg_addr(a, 0, dest);
    /* The double is argument slot 1, so it travels in XMM1 rather than
     * XMM0: Win64 numbers the vector registers by slot, and slot 0 is the
     * address of the returned value. */
    fprintf(a->out, "\tmovsd .LC%d(%%rip), %s\n", label, "%xmm1");
    call(a, "float_value");
    break;
  }

  case AST_STRING: {
    int label = emit_string_constant(a, node->as.string_literal.value,
                                     node->as.string_literal.length);
    char text[32];
    snprintf(text, sizeof(text), ".LC%d", label);
    arg_addr(a, 0, dest);
    arg_addr(a, 1, loc_rip(text));
    arg_i32(a, 2, node->as.string_literal.length);
    call(a, "string_value");
    break;
  }

  case AST_VAR_REF:
    emit_load(a, symbol_for(a, node, HD_BINDING_READ),
              node->as.variable_ref.name, node->as.variable_ref.name_length,
              dest);
    break;

  case AST_BINARY_OP: {
    TokenType op = node->as.binary_op.operator_type;

    /* && and || short-circuit through jumps exactly as the VM's do, and
     * yield 0 or 1 like its normalising pushes. */
    if (op == TOKEN_ANDAND || op == TOKEN_OROR) {
      Loc slot = loc_scratch(a, next);
      int settled = new_label(a);
      int join = new_label(a);

      emit_expr(a, node->as.binary_op.left, slot, next + 1);
      emit_truthy(a, slot);
      fprintf(a->out, op == TOKEN_ANDAND ? "\tjz .L%d\n" : "\tjnz .L%d\n",
              settled);

      emit_expr(a, node->as.binary_op.right, slot, next + 1);
      emit_truthy(a, slot);
      fputs("\tsetne %al\n\tmovzbl %al, %eax\n", a->out);
      jump(a, join);

      place_label(a, settled);
      if (op == TOKEN_ANDAND)
        fputs("\txorl %eax, %eax\n", a->out);
      else
        fputs("\tmovl $1, %eax\n", a->out);

      place_label(a, join);
      fputs("\tmovslq %eax, %rdx\n", a->out);
      arg_addr(a, 0, dest);
      call(a, "int_value");
      break;
    }

    int code = binop_code(op);
    if (code < 0) {
      emit_error(a, "unsupported binary operator.");
      break;
    }
    emit_expr(a, node->as.binary_op.left, loc_scratch(a, next), next + 2);
    emit_expr(a, node->as.binary_op.right, loc_scratch(a, next + 1), next + 2);
    arg_addr(a, 0, dest);
    arg_i32(a, 1, code);
    arg_addr(a, 2, loc_scratch(a, next));
    arg_addr(a, 3, loc_scratch(a, next + 1));
    call(a, "HDBinaryX");
    break;
  }

  /* Unary minus is 0 - x rather than its own operation, and negation asks
   * HDNot, which asks HDTruthy , both exactly as the VM does them. */
  case AST_UNARY_OP:
    if (node->as.unary_op.operator_type == TOKEN_MINUS) {
      arg_addr(a, 0, loc_scratch(a, next));
      arg_i64(a, 1, 0);
      call(a, "int_value");
      emit_expr(a, node->as.unary_op.operand, loc_scratch(a, next + 1),
                next + 2);
      arg_addr(a, 0, dest);
      arg_i32(a, 1, HD_SUB);
      arg_addr(a, 2, loc_scratch(a, next));
      arg_addr(a, 3, loc_scratch(a, next + 1));
      call(a, "HDBinaryX");
    } else if (node->as.unary_op.operator_type == TOKEN_BANG) {
      emit_expr(a, node->as.unary_op.operand, loc_scratch(a, next), next + 1);
      arg_addr(a, 0, dest);
      arg_addr(a, 1, loc_scratch(a, next));
      call(a, "HDNot");
    } else {
      emit_error(a, "unsupported unary operator.");
    }
    break;

  case AST_CAST: {
    HDCastKind kind;
    if (!HDCastKindFromTypeSyntax(node->as.cast.target_type, &kind)) {
      emit_error(a, "cast target must be Bool, an integer type, or F64.");
      break;
    }
    emit_expr(a, node->as.cast.expression, loc_scratch(a, next), next + 1);
    arg_addr(a, 0, dest);
    arg_i32(a, 1, (int)kind);
    arg_addr(a, 2, loc_scratch(a, next));
    call(a, "HDCastX");
    break;
  }

  case AST_TERNARY_OP: {
    int other = new_label(a);
    int join = new_label(a);
    emit_expr(a, node->as.ternary_op.condition, loc_scratch(a, next), next + 1);
    emit_truthy(a, loc_scratch(a, next));
    fprintf(a->out, "\tjz .L%d\n", other);
    emit_expr(a, node->as.ternary_op.true_expr, dest, next);
    jump(a, join);
    place_label(a, other);
    emit_expr(a, node->as.ternary_op.false_expr, dest, next);
    place_label(a, join);
    break;
  }

  case AST_CALL:
    emit_call(a, node, dest, next);
    break;

  case AST_ARRAY_LITERAL:
    emit_array_literal(a, node, dest, next);
    break;

  case AST_INDEX:
    emit_expr(a, node->as.index_expr.target, loc_scratch(a, next), next + 2);
    emit_expr(a, node->as.index_expr.index, loc_scratch(a, next + 1), next + 2);
    arg_addr(a, 0, dest);
    arg_addr(a, 1, loc_scratch(a, next));
    arg_addr(a, 2, loc_scratch(a, next + 1));
    call(a, "HDIndexX");
    break;

  case AST_ARRAY_LEN_EXPR:
    emit_expr(a, node->as.array_length_expr.target, loc_scratch(a, next),
              next + 1);
    arg_addr(a, 0, dest);
    arg_addr(a, 1, loc_scratch(a, next));
    call(a, "HDLengthX");
    break;

  default:
    emit_error(a, "statement used where an expression was expected.");
    break;
  }
}

/* ---------------- Statements ---------------------------------------------- */

static void emit_body(Asm *a, ASTNode *node, int next) {
  if (!node)
    return;
  if (node->type == AST_BLOCK) {
    for (int i = 0; i < node->as.block.statement_count; i++)
      emit_stmt(a, node->as.block.statements[i], next);
    return;
  }
  emit_stmt(a, node, next);
}

/* A declaration binds its own slot whatever else the name meant. Evaluating
 * straight into the slot is safe even when the initialiser reads the name it
 * declares: the read happens first and finds the slot still unbound, and the
 * runtime writes its result through the destination pointer only on the way
 * out. */
static void emit_define(Asm *a, const HDSymbol *symbol, const char *name,
                        int length, ASTNode *value, int next) {
  if (!symbol || symbol->storage == HD_SYMBOL_EXTERNAL) {
    emit_expr(a, value, loc_scratch(a, next), next + 1);
    emit_bind(a, symbol, name, length, loc_scratch(a, next));
    return;
  }
  emit_expr(a, value, loc_symbol(a, symbol), next);
  if (has_bound_bit(symbol))
    set_bound(a, loc_symbol_bound(a, symbol), 1);
}

static void emit_stmt(Asm *a, ASTNode *node, int next) {
  if (!node || a->had_error)
    return;

  switch (node->type) {
  case AST_VAR_DECL:
    emit_define(a, symbol_for(a, node, HD_BINDING_DECLARATION),
                node->as.variable_decl.name,
                node->as.variable_decl.name_length,
                node->as.variable_decl.initializer, next);
    break;

  case AST_ASSIGN:
    emit_expr(a, node->as.assignment.value, loc_scratch(a, next), next + 1);
    emit_store(a, symbol_for(a, node, HD_BINDING_WRITE),
               node->as.assignment.name, node->as.assignment.name_length,
               loc_scratch(a, next));
    break;

  /* Target, index and value are three ordered evaluations. Elements are
   * reached through the shared elements pointer, so writing through the
   * copy on the stack updates the array the variable holds. */
  case AST_INDEX_ASSIGN:
    emit_expr(a, node->as.index_assignment.target, loc_scratch(a, next),
              next + 3);
    emit_expr(a, node->as.index_assignment.index, loc_scratch(a, next + 1),
              next + 3);
    emit_expr(a, node->as.index_assignment.value, loc_scratch(a, next + 2),
              next + 3);
    arg_addr(a, 0, loc_scratch(a, next));
    arg_addr(a, 1, loc_scratch(a, next + 1));
    arg_addr(a, 2, loc_scratch(a, next + 2));
    call(a, "HDIndexSetX");
    break;

  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      emit_stmt(a, node->as.block.statements[i], next);
    break;

  case AST_IF: {
    int other = new_label(a);
    int join = new_label(a);
    emit_expr(a, node->as.if_statement.condition, loc_scratch(a, next),
              next + 1);
    emit_truthy(a, loc_scratch(a, next));
    fprintf(a->out, "\tjz .L%d\n", other);
    emit_body(a, node->as.if_statement.then_branch, next);
    jump(a, join);
    place_label(a, other);
    emit_body(a, node->as.if_statement.else_branch, next);
    place_label(a, join);
    break;
  }

  case AST_WHILE: {
    int top = new_label(a);
    int done = new_label(a);
    int outer_break = a->break_label;
    int outer_continue = a->continue_label;
    place_label(a, top);
    emit_expr(a, node->as.while_statement.condition, loc_scratch(a, next),
              next + 1);
    emit_truthy(a, loc_scratch(a, next));
    fprintf(a->out, "\tjz .L%d\n", done);
    /* Nothing runs between the body and the test, so continue is the back
     * edge itself. */
    a->break_label = done;
    a->continue_label = top;
    emit_body(a, node->as.while_statement.body, next);
    a->break_label = outer_break;
    a->continue_label = outer_continue;
    jump(a, top);
    place_label(a, done);
    break;
  }

  /* Init, then test, body, increment, in that order, because that is the
   * order the VM runs them in. */
  case AST_FOR: {
    int top = new_label(a);
    int done = new_label(a);
    emit_stmt(a, node->as.for_statement.initializer, next);
    place_label(a, top);
    if (node->as.for_statement.condition) {
      emit_expr(a, node->as.for_statement.condition, loc_scratch(a, next),
                next + 1);
      emit_truthy(a, loc_scratch(a, next));
      fprintf(a->out, "\tjz .L%d\n", done);
    }
    int outer_break = a->break_label;
    int outer_continue = a->continue_label;
    int next_turn = new_label(a);
    a->break_label = done;
    a->continue_label = next_turn;
    emit_body(a, node->as.for_statement.body, next);
    a->break_label = outer_break;
    a->continue_label = outer_continue;
    /* In front of the increment, not the test: a continue that skipped it
     * would leave the loop variable alone and spin. */
    place_label(a, next_turn);
    emit_stmt(a, node->as.for_statement.increment, next);
    jump(a, top);
    place_label(a, done);
    break;
  }

  /* The array is snapshotted once and its length re-read each turn, which is
   * what the VM's desugaring does: it stores the array in a temporary and
   * applies BC_ARRAY_LENGTH to that temporary every iteration. The counter
   * is a plain integer in the first eight bytes of a scratch slot. Both stay
   * live for the whole loop, which is why the body builds above them.
   *
   * Nothing may jump into this body from outside; the resolver rejects that,
   * which is what lets the counter be initialised once here. */
  case AST_FOREACH: {
    Loc array = loc_scratch(a, next);
    Loc counter = loc_scratch(a, next + 1);
    Loc staging = loc_scratch(a, next + 2);
    Loc boxed = loc_scratch(a, next + 3);
    int top = new_label(a);
    int done = new_label(a);

    emit_expr(a, node->as.foreach_statement.array_expression, array, next + 4);
    fprintf(a->out, "\tmovq $0, %s\n", counter.text);

    place_label(a, top);
    arg_addr(a, 0, staging);
    arg_addr(a, 1, array);
    call(a, "HDLengthX");
    fprintf(a->out, "\tmovq %s, %s\n", counter.text, "%rax");
    fprintf(a->out, "\tcmpq %d(%%rbp), %s\n",
            scratch_offset(a, next + 2) + HD_VALUE_I64, "%rax");
    fprintf(a->out, "\tjge .L%d\n", done);

    /* The element is bound before the index, in that order. */
    arg_addr(a, 0, boxed);
    fprintf(a->out, "\tmovq %s, %s\n", counter.text, "%rdx");
    call(a, "int_value");
    arg_addr(a, 0, staging);
    arg_addr(a, 1, array);
    arg_addr(a, 2, boxed);
    call(a, "HDIndexX");
    emit_bind(a, symbol_for(a, node, HD_BINDING_FOREACH_VALUE),
              node->as.foreach_statement.variable_name,
              node->as.foreach_statement.variable_name_length, staging);

    if (node->as.foreach_statement.index_name != NULL) {
      arg_addr(a, 0, staging);
      fprintf(a->out, "\tmovq %s, %s\n", counter.text, "%rdx");
      call(a, "int_value");
      emit_bind(a, symbol_for(a, node, HD_BINDING_FOREACH_INDEX),
                node->as.foreach_statement.index_name,
                node->as.foreach_statement.index_name_length, staging);
    }

    int outer_break = a->break_label;
    int outer_continue = a->continue_label;
    int next_turn = new_label(a);
    a->break_label = done;
    a->continue_label = next_turn;
    emit_body(a, node->as.foreach_statement.body, next + 4);
    a->break_label = outer_break;
    a->continue_label = outer_continue;

    /* The counter is what ends this loop, so continue lands in front of the
     * bump rather than on the test. */
    place_label(a, next_turn);
    fprintf(a->out, "\tincq %s\n", counter.text);
    jump(a, top);
    place_label(a, done);
    break;
  }

  case AST_FUNC_DECL:
    break;

  case AST_LABEL:
    fprintf(a->out, ".Lg%d:\n", HDResolutionLabelIndex(a->resolution, node));
    break;

  /* The resolver has already refused either outside a loop, so a missing
   * target here would be a bug in this file rather than in the program. */
  case AST_BREAK:
  case AST_CONTINUE: {
    int target =
        node->type == AST_BREAK ? a->break_label : a->continue_label;
    if (target < 0) {
      emit_error(a, "break or continue outside a loop.");
      break;
    }
    jump(a, target);
    break;
  }

  case AST_GOTO: {
    int label_index = HDResolutionGotoLabel(a->resolution, node);
    if (label_index < 0) {
      emit_error(a, "goto was not resolved to a label.");
      break;
    }
    fprintf(a->out, "\tjmp .Lg%d\n", label_index);
    break;
  }

  case AST_RETURN:
    emit_expr(a, node->as.return_statement.expression, loc_scratch(a, next),
              next + 1);
    if (a->in_main) {
      /* The top level is C's main, which returns int. Matching emit_c.c,
       * whose translation of a top-level return is also not a HolyD value. */
      fputs("\txorl %eax, %eax\n\tleave\n\tret\n", a->out);
    } else {
      copy_to_sret(a, loc_scratch(a, next));
      fprintf(a->out, "\tmovq %d(%%rbp), %s\n", a->sret_off, "%rax");
      fputs("\tleave\n\tret\n", a->out);
    }
    break;

  /* A bare string statement is HolyC's implicit print. */
  case AST_STRING:
    emit_expr(a, node, loc_scratch(a, next), next + 2);
    arg_addr(a, 0, loc_scratch(a, next + 1));
    arg_i32(a, 1, 1);
    arg_addr(a, 2, loc_scratch(a, next));
    arg_i32(a, 3, 0);
    call(a, "HDPrintN");
    break;

  /* A bare name in statement position is a zero-argument call when it names
   * a function, and an ordinary discarded load otherwise. */
  case AST_VAR_REF:
    if (find_function(a, node->as.variable_ref.name,
                      node->as.variable_ref.name_length) >= 0) {
      ASTNode synthetic;
      synthetic.type = AST_CALL;
      synthetic.as.call.callee_name = node->as.variable_ref.name;
      synthetic.as.call.callee_name_length = node->as.variable_ref.name_length;
      synthetic.as.call.arguments = NULL;
      synthetic.as.call.argument_count = 0;
      emit_call(a, &synthetic, loc_scratch(a, next), next + 1);
    } else {
      emit_expr(a, node, loc_scratch(a, next), next + 1);
    }
    break;

  default:
    emit_expr(a, node, loc_scratch(a, next), next + 1);
    break;
  }
}

/* ---------------- Frames -------------------------------------------------- */

static int align_up(int value, int alignment) {
  int remainder = value % alignment;
  return remainder ? value + (alignment - remainder) : value;
}

/* Carves the frame and writes the prologue. Everything the body needs is
 * reserved here, so no instruction after this one moves %rsp , which is what
 * lets outgoing arguments live at fixed offsets from it. */
static void open_frame(Asm *a, int slot_count, int scratch_count) {
  int offset = 0;
  offset -= 8;
  a->sret_off = offset;
  offset -= slot_count * HD_VALUE_SIZE;
  a->locals_base = offset;
  offset -= slot_count * 8;
  a->bound_base = offset;
  offset -= scratch_count * HD_VALUE_SIZE;
  a->scratch_base = offset;

  int variables = align_up(-offset, 16);
  int outgoing = align_up(32 + max_int(0, a->max_args - 4) * 8, 16);
  a->frame_size = variables + outgoing;

  fputs("\tpushq %rbp\n\tmovq %rsp, %rbp\n", a->out);
  fprintf(a->out, "\tsubq $%d, %s\n", a->frame_size, "%rsp");
}

/* Every non-parameter slot starts unbound, which is what keeps a
 * declaration from taking effect before it runs. */
static void clear_bound_bits(Asm *a, int slot_count) {
  for (int slot = 0; slot < slot_count; slot++) {
    const HDSymbol *symbol =
        HDResolutionSlotSymbol(a->resolution, a->scope, slot);
    if (symbol && !has_bound_bit(symbol))
      continue;
    set_bound(a, loc_bound_local(a, slot), 0);
  }
}

static void emit_function(Asm *a, int index) {
  ASTNode *fn = a->functions[index];
  const HDResolvedFunction *scope = NULL;
  char symbol[64];
  int slot_count = 0;

  a->scope = HDResolutionFunctionIndex(a->resolution, fn);
  a->in_main = 0;
  if (a->scope >= 0) {
    scope = &a->resolution->functions[a->scope];
    slot_count = scope->frame_slot_count;
  }

  a->max_args = HD_MIN_ARGS;
  /* At least one, because the implicit `return 0` stages its value in
   * scratch slot 0 even when the body needed none. */
  int scratch = max_int(1, scratch_for_stmt(a, fn->as.function_decl.body));

  function_symbol(a, index, symbol, sizeof(symbol));
  fprintf(a->out, "\n%s:\n", symbol);
  open_frame(a, slot_count, scratch);

  /* The hidden pointer to the caller's destination, kept for `return`. */
  fprintf(a->out, "\tmovq %s, %d(%%rbp)\n", "%rcx", a->sret_off);

  clear_bound_bits(a, slot_count);

  /* Parameters arrive as addresses, one argument slot later than their
   * source position because slot 0 is the returned value. Two parameters of
   * the same name share a slot, so copying them in order leaves the later
   * one holding it, as repeated EnvDefine calls did in the VM. */
  for (int i = 0; i < fn->as.function_decl.parameter_count; i++) {
    int arg = i + 1;
    int slot = scope && scope->parameter_slots ? scope->parameter_slots[i] : i;
    if (slot < 0 || slot >= slot_count)
      continue;
    if (arg < 4) {
      copy_from_pointer(a, arg_reg64(arg), loc_local(a, slot));
    } else {
      fprintf(a->out, "\tmovq %d(%%rbp), %s\n", 16 + 8 * arg, "%rax");
      copy_from_pointer(a, "%rax", loc_local(a, slot));
    }
  }

  emit_body(a, fn->as.function_decl.body, 0);

  /* Falling off the end is the VM's implicit `return 0`. */
  arg_addr(a, 0, loc_scratch(a, 0));
  arg_i64(a, 1, 0);
  call(a, "int_value");
  copy_to_sret(a, loc_scratch(a, 0));
  fprintf(a->out, "\tmovq %d(%%rbp), %s\n", a->sret_off, "%rax");
  fputs("\tleave\n\tret\n", a->out);
  a->scope = -1;
}

/* ---------------- Entry point --------------------------------------------- */

static int calls_name(ASTNode *node, const char *name, int len);

static int any_calls_name(ASTNode **nodes, int count, const char *name,
                          int len) {
  for (int i = 0; i < count; i++)
    if (calls_name(nodes[i], name, len))
      return 1;
  return 0;
}

/* Whether `node` contains a call to `name` anywhere. A bare mention of the
 * name in statement position counts, because that is a zero-argument call. */
static int calls_name(ASTNode *node, const char *name, int len) {
  if (!node)
    return 0;
  switch (node->type) {
  case AST_CALL:
    if (node->as.call.callee_name_length == len &&
        strncmp(node->as.call.callee_name, name, (size_t)len) == 0)
      return 1;
    return any_calls_name(node->as.call.arguments,
                          node->as.call.argument_count, name, len);
  case AST_ARRAY_LITERAL:
    return any_calls_name(node->as.array_literal.elements,
                          node->as.array_literal.element_count, name, len);
  case AST_VAR_REF:
    return node->as.variable_ref.name_length == len &&
           strncmp(node->as.variable_ref.name, name, (size_t)len) == 0;
  case AST_BINARY_OP:
    return calls_name(node->as.binary_op.left, name, len) ||
           calls_name(node->as.binary_op.right, name, len);
  case AST_UNARY_OP:
    return calls_name(node->as.unary_op.operand, name, len);
  case AST_CAST:
    return calls_name(node->as.cast.expression, name, len);
  case AST_TERNARY_OP:
    return calls_name(node->as.ternary_op.condition, name, len) ||
           calls_name(node->as.ternary_op.true_expr, name, len) ||
           calls_name(node->as.ternary_op.false_expr, name, len);
  case AST_INDEX:
    return calls_name(node->as.index_expr.target, name, len) ||
           calls_name(node->as.index_expr.index, name, len);
  case AST_ARRAY_LEN_EXPR:
    return calls_name(node->as.array_length_expr.target, name, len);
  case AST_VAR_DECL:
    return calls_name(node->as.variable_decl.initializer, name, len);
  case AST_ASSIGN:
    return calls_name(node->as.assignment.value, name, len);
  case AST_INDEX_ASSIGN:
    return calls_name(node->as.index_assignment.target, name, len) ||
           calls_name(node->as.index_assignment.index, name, len) ||
           calls_name(node->as.index_assignment.value, name, len);
  case AST_BLOCK:
    return any_calls_name(node->as.block.statements,
                          node->as.block.statement_count, name, len);
  case AST_IF:
    return calls_name(node->as.if_statement.condition, name, len) ||
           calls_name(node->as.if_statement.then_branch, name, len) ||
           calls_name(node->as.if_statement.else_branch, name, len);
  case AST_WHILE:
    return calls_name(node->as.while_statement.condition, name, len) ||
           calls_name(node->as.while_statement.body, name, len);
  case AST_FOR:
    return calls_name(node->as.for_statement.initializer, name, len) ||
           calls_name(node->as.for_statement.condition, name, len) ||
           calls_name(node->as.for_statement.increment, name, len) ||
           calls_name(node->as.for_statement.body, name, len);
  case AST_FOREACH:
    return calls_name(node->as.foreach_statement.array_expression, name, len) ||
           calls_name(node->as.foreach_statement.body, name, len);
  case AST_RETURN:
    return calls_name(node->as.return_statement.expression, name, len);
  default:
    return 0;
  }
}

int HDEmitAsm(ASTNode *ast, const HDResolution *resolution, FILE *out,
              const char *source_name) {
  Asm a;
  memset(&a, 0, sizeof(a));
  a.out = out;
  a.resolution = resolution;
  a.scope = -1;
  a.break_label = -1;
  a.continue_label = -1;

  if (!ast || ast->type != AST_BLOCK) {
    emit_error(&a, "program root is not a block.");
    return 0;
  }
  if (HD_VALUE_SIZE % 8 != 0) {
    emit_error(&a, "HDValue is not a whole number of quadwords.");
    return 0;
  }

  /* Collect functions first: a call can name one declared further down. */
  a.functions = (ASTNode **)malloc(sizeof(ASTNode *) *
                                   (size_t)(ast->as.block.statement_count + 1));
  if (!a.functions) {
    emit_error(&a, "out of memory while collecting functions.");
    return 0;
  }
  for (int i = 0; i < ast->as.block.statement_count; i++) {
    ASTNode *stmt = ast->as.block.statements[i];
    if (stmt && stmt->type == AST_FUNC_DECL)
      a.functions[a.function_count++] = stmt;
  }

  fprintf(out, "/* Generated by holyd --emit-asm from %s. Do not edit.\n",
          source_name);
  fputs(" *\n"
        " * x86-64, GNU assembler syntax, Win64 calling convention. Build\n"
        " * alongside the HolyD runtime, from the repository root:\n"
        " *\n"
        " *   gcc -std=gnu11 -O2 -I src <this file> \\\n"
        " *       src/runtime.c src/eval.c src/ffi.c src/ffi_win32.c \\\n"
        " *       src/platform/standalone/gfx.c src/platform/standalone/bmp.c \\\n"
        " *       -o program.exe -lgdi32 -luser32 -lws2_32\n"
        " */\n",
        out);
  fprintf(out, "\t.file \"%s\"\n", source_name);

  /* The Environment is no longer where variables live. It holds the FFI
   * constants, and it is the last link in a name's fallback chain. Its size
   * comes from the header the runtime is built from, so the two cannot
   * disagree about it. */
  fputs("\n\t.bss\n\t.balign 8\nhd_globals:\n", out);
  fprintf(out, "\t.space %d\n", (int)sizeof(Environment));

  /* One object per global slot, plus the bit saying whether the declaration
   * that binds it has run. .bss is zeroed, so nothing starts bound. */
  for (int slot = 0; slot < resolution->global_slot_count; slot++) {
    char symbol[64];
    global_symbol(&a, slot, symbol, sizeof(symbol));
    fprintf(out, "\t.balign 8\n%s:\n\t.space %d\n", symbol, HD_VALUE_SIZE);
    fprintf(out, "\t.balign 8\n%s_bound:\n\t.space 8\n", symbol);
  }

  fputs("\n\t.text\n", out);

  for (int i = 0; i < a.function_count; i++)
    emit_function(&a, i);

  /* The top level runs in C's main, exactly as the VM's main chunk does. */
  ASTNode top;
  top.type = AST_BLOCK;
  top.as.block.statements = NULL;
  top.as.block.statement_count = 0;
  {
    int count = 0;
    ASTNode **stmts = (ASTNode **)malloc(
        sizeof(ASTNode *) * (size_t)(ast->as.block.statement_count + 1));
    if (!stmts) {
      emit_error(&a, "out of memory while collecting top-level statements.");
      free(a.functions);
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

  a.scope = -1;
  a.in_main = 1;
  a.max_args = HD_MIN_ARGS;
  /* At least one, for the discarded result of the entry-point call. */
  int scratch = max_int(1, scratch_for_stmt(&a, &top));

  fputs("\n\t.globl main\nmain:\n", out);
  open_frame(&a, 0, scratch);
  arg_addr(&a, 0, loc_rip("hd_globals"));
  call(&a, "EnvInit");
  /* Before the program, so a script that assigns over EV_KEY_DOWN wins. */
  arg_addr(&a, 0, loc_rip("hd_globals"));
  call(&a, "ffi_define_globals");

  for (int i = 0; i < top.as.block.statement_count; i++)
    emit_stmt(&a, top.as.block.statements[i], 0);

  /* Entry point, on the same terms as the VM: main then Main, only when it
   * takes no parameters and the top level has not already called it. */
  int entry = find_function(&a, "main", 4);
  if (entry < 0)
    entry = find_function(&a, "Main", 4);
  if (entry >= 0) {
    ASTNode *fn = a.functions[entry];
    int already = any_calls_name(top.as.block.statements,
                                 top.as.block.statement_count,
                                 fn->as.function_decl.name,
                                 fn->as.function_decl.name_length);
    if (fn->as.function_decl.parameter_count == 0 && !already) {
      char symbol[64];
      function_symbol(&a, entry, symbol, sizeof(symbol));
      arg_addr(&a, 0, loc_scratch(&a, 0));
      call(&a, symbol);
    }
  }

  fputs("\txorl %eax, %eax\n\tleave\n\tret\n", out);

  free(top.as.block.statements);
  free(a.functions);
  return !a.had_error;
}
