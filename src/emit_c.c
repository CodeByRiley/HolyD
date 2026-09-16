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
#include "runtime.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  ASTNode *node;
  int base_index;
} CClass;

typedef struct {
  FILE *out;
  const HDResolution *resolution;
  const HDTypeCheck *types;
  unsigned char *unsafe_symbols;
  ASTNode **functions; /* AST_FUNC_DECL nodes, in source order */
  int function_count;
  CClass *classes;     /* AST_CLASS_DECL nodes, in source order */
  int class_count;
  /* Set only while emitting a class constructor or method.  Those bodies
   * are deliberately native C: their receiver and parameters do not go
   * through the boxed VM calling convention used by ordinary functions. */
  int class_index;
  ParameterSyntax *class_parameters;
  int class_parameter_count;
  int temp_counter;  /* reset per emitted C function */
  /* -1 when the innermost loop lowers to a C loop a bare `continue` is
   * right for; otherwise the id of the label before that loop's increment. */
  int continue_label;
  int label_counter;
  int store_counter; /* likewise, for the one-shot value an assignment holds */
  int scope;         /* resolver function index, or -1 at the top level */
  int had_error;
} Emitter;

static void emit_expression(Emitter *e, ASTNode *node);
static void emit_statement(Emitter *e, ASTNode *node, int indent);
static const char *direct_binop(TokenType op);

static int same_name(const char *a, int a_len, const char *b, int b_len) {
  return a_len == b_len && strncmp(a, b, (size_t)a_len) == 0;
}

static int find_class(Emitter *e, const char *name, int length) {
  for (int i = 0; i < e->class_count; i++) {
    ASTClassDecl *decl = &e->classes[i].node->as.class_decl;
    if (same_name(decl->name, decl->name_length, name, length))
      return i;
  }
  return -1;
}

static int class_from_syntax(Emitter *e, const TypeSyntax *type) {
  if (!type || type->kind != TYPE_SYNTAX_NAMED)
    return -1;
  return find_class(e, type->as.named.name, type->as.named.name_length);
}

static int class_from_type(Emitter *e, HDTypeId id) {
  const HDType *type = HDTypeGet(e->types, id);
  while (type && type->kind == HD_TYPE_QUALIFIED) {
    id = type->primary;
    type = HDTypeGet(e->types, id);
  }
  if (!type || type->kind != HD_TYPE_NAMED)
    return -1;
  return find_class(e, type->name, type->name_length);
}

static void write_class_name(Emitter *e, int index) {
  fprintf(e->out, "hd_class_%d", index);
}

static void write_constructor_name(Emitter *e, int class_index, int ordinal) {
  fprintf(e->out, "hd_new_%d_%d", class_index, ordinal);
}

static void write_initializer_name(Emitter *e, int class_index, int ordinal) {
  fprintf(e->out, "hd_init_%d_%d", class_index, ordinal);
}

static void write_method_name(Emitter *e, int class_index, int member_index) {
  fprintf(e->out, "hd_method_%d_%d", class_index, member_index);
}

/* Whether a `continue` in this statement belongs to the loop being emitted.
 * A nested loop captures its own, so the walk stops at one. It exists to
 * decide whether to write a continue label: an unreferenced label is a
 * warning under -Wall, which the transpiled C is compiled with. */
static int has_own_continue(const ASTNode *node) {
  if (!node)
    return 0;
  switch (node->type) {
  case AST_CONTINUE:
    return 1;
  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      if (has_own_continue(node->as.block.statements[i]))
        return 1;
    return 0;
  case AST_IF:
    return has_own_continue(node->as.if_statement.then_branch) ||
           has_own_continue(node->as.if_statement.else_branch);
  default:
    return 0;
  }
}

static void emit_error(Emitter *e, const char *message) {
  printf("Emit error: %s\n", message);
  e->had_error = 1;
}

static void class_error(Emitter *e, const ASTNode *site, const char *message,
                        const char *name, int name_length) {
  if (e->had_error) return;
  printf("Class error: ");
  if (message)
    printf(message, name_length, name);
  if (site && site->span.start_line)
    printf(" on line %zu", site->span.start_line);
  fputs(".\n", stdout);
  e->had_error = 1;
}

static void missing_member_error(Emitter *e, const ASTNode *site,
                                 int class_index, const char *kind,
                                 const char *name, int name_length,
                                 int argument_count) {
  if (e->had_error) return;
  if (class_index >= 0) {
    ASTClassDecl *decl = &e->classes[class_index].node->as.class_decl;
    printf("Class error: class '%.*s' has no %s '%.*s'",
           decl->name_length, decl->name, kind, name_length, name);
    if (strcmp(kind, "method") == 0)
      printf(" accepting %d argument%s", argument_count,
             argument_count == 1 ? "" : "s");
  } else {
    printf("Class error: cannot determine the receiver class for %s '%.*s'",
           kind, name_length, name);
  }
  if (site && site->span.start_line)
    printf(" on line %zu", site->span.start_line);
  fputs(".\n", stdout);
  e->had_error = 1;
}

static int name_is(const char *a, int a_len, const char *b) {
  int b_len = (int)strlen(b);
  return a_len == b_len && strncmp(a, b, (size_t)a_len) == 0;
}

static int is_builtin_type_name(const char *name, int length) {
  return name_is(name, length, "U0") || name_is(name, length, "void") ||
         name_is(name, length, "I8") || name_is(name, length, "U8") ||
         name_is(name, length, "I16") || name_is(name, length, "U16") ||
         name_is(name, length, "I32") || name_is(name, length, "U32") ||
         name_is(name, length, "I64") || name_is(name, length, "U64") ||
         name_is(name, length, "int") || name_is(name, length, "uint") ||
         name_is(name, length, "long") || name_is(name, length, "ulong") ||
         name_is(name, length, "F64") || name_is(name, length, "double") ||
         name_is(name, length, "Bool") || name_is(name, length, "bool") ||
         name_is(name, length, "string") || name_is(name, length, "auto");
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

typedef enum {
  C_REP_BOXED,
  C_REP_I64,
  C_REP_F64,
  C_REP_CLASS
} CRepresentation;

static CRepresentation type_representation(const HDTypeCheck *types,
                                            HDTypeId id) {
  const HDType *type = HDTypeGet(types, id);
  while (type && type->kind == HD_TYPE_QUALIFIED) {
    id = type->primary;
    type = HDTypeGet(types, id);
  }
  if (!type)
    return C_REP_BOXED;
  switch (type->kind) {
  case HD_TYPE_BOOL:
  case HD_TYPE_I8:
  case HD_TYPE_U8:
  case HD_TYPE_I16:
  case HD_TYPE_U16:
  case HD_TYPE_I32:
  case HD_TYPE_U32:
  case HD_TYPE_I64:
  case HD_TYPE_U64:
    return C_REP_I64;
  case HD_TYPE_F64:
    return C_REP_F64;
  default:
    return C_REP_BOXED;
  }
}

static CRepresentation symbol_representation(Emitter *e,
                                              const HDSymbol *symbol) {
  if (!symbol || symbol->storage == HD_SYMBOL_EXTERNAL)
    return C_REP_BOXED;
  if (e->unsafe_symbols && e->unsafe_symbols[symbol->id])
    return C_REP_BOXED;
  HDTypeId type = HDTypeOfSymbol(e->types, symbol->id);
  if (class_from_type(e, type) >= 0)
    return C_REP_CLASS;
  return type_representation(e->types, type);
}

static CRepresentation node_representation(Emitter *e, const ASTNode *node) {
  if (!node)
    return C_REP_BOXED;
  HDTypeId type = HDTypeOfNode(e->types, node);
  return class_from_type(e, type) >= 0 ? C_REP_CLASS
                                        : type_representation(e->types, type);
}

/* ---------------- Classes ------------------------------------------------- */

/* Class metadata lives in the AST for now.  This small lookup layer keeps
 * the generated C independent of source identifiers (which may collide with
 * C keywords) and is also the natural place to grow overload/type checks. */
static ASTNode *find_field(Emitter *e, int class_index, const char *name,
                           int length, int *owner, int *member_index) {
  for (int current = class_index; current >= 0;
       current = e->classes[current].base_index) {
    ASTClassDecl *decl = &e->classes[current].node->as.class_decl;
    for (int i = 0; i < decl->member_count; i++) {
      ASTNode *member = decl->members[i];
      if (member && member->type == AST_VAR_DECL &&
          same_name(member->as.variable_decl.name,
                    member->as.variable_decl.name_length, name, length)) {
        if (owner) *owner = current;
        if (member_index) *member_index = i;
        return member;
      }
    }
  }
  return NULL;
}

static ASTNode *find_method(Emitter *e, int class_index, const char *name,
                            int length, int argc, int *owner,
                            int *member_index) {
  for (int current = class_index; current >= 0;
       current = e->classes[current].base_index) {
    ASTClassDecl *decl = &e->classes[current].node->as.class_decl;
    for (int i = 0; i < decl->member_count; i++) {
      ASTNode *member = decl->members[i];
      if (member && member->type == AST_FUNC_DECL &&
          member->as.function_decl.parameter_count == argc &&
          same_name(member->as.function_decl.name,
                    member->as.function_decl.name_length, name, length)) {
        if (owner) *owner = current;
        if (member_index) *member_index = i;
        return member;
      }
    }
  }
  return NULL;
}

static int own_constructor_count(Emitter *e, int class_index) {
  int count = 0;
  ASTClassDecl *decl = &e->classes[class_index].node->as.class_decl;
  for (int i = 0; i < decl->member_count; i++)
    if (decl->members[i] && decl->members[i]->type == AST_CONSTRUCTOR_DECL)
      count++;
  return count;
}

static ASTNode *own_constructor_at(Emitter *e, int class_index, int ordinal) {
  ASTClassDecl *decl = &e->classes[class_index].node->as.class_decl;
  for (int i = 0; i < decl->member_count; i++) {
    ASTNode *member = decl->members[i];
    if (member && member->type == AST_CONSTRUCTOR_DECL && ordinal-- == 0)
      return member;
  }
  return NULL;
}

/* A class without a constructor exposes its base constructors.  This is a
 * temporary convenience rule (and why `ColoredPoint(1)` works in the sample)
 * until the parser and semantic model grow explicit `super(...)` calls. */
static int constructor_count(Emitter *e, int class_index) {
  int own = own_constructor_count(e, class_index);
  if (own) return own;
  int base = e->classes[class_index].base_index;
  return base >= 0 ? constructor_count(e, base) : 1;
}

static ASTNode *constructor_parameters_from(Emitter *e, int class_index,
                                            int ordinal) {
  int own = own_constructor_count(e, class_index);
  if (own) return own_constructor_at(e, class_index, ordinal);
  int base = e->classes[class_index].base_index;
  return base >= 0 ? constructor_parameters_from(e, base, ordinal) : NULL;
}

static int constructor_for_arity(Emitter *e, int class_index, int argc) {
  int count = constructor_count(e, class_index);
  for (int i = 0; i < count; i++) {
    ASTNode *constructor = constructor_parameters_from(e, class_index, i);
    int parameter_count = constructor
                              ? constructor->as.constructor_decl.parameter_count
                              : 0;
    if (parameter_count == argc)
      return i;
  }
  return -1;
}

/* Class lowering needs concrete layouts, unlike the boxed runtime where an
 * unknown named type can remain opaque.  Check every named type before any C
 * is written so deleting `class Point` produces a source-level diagnostic,
 * not a later failure while looking up `p.method()`. */
static void validate_class_references(Emitter *e, const ASTNode *node);

static void validate_class_type(Emitter *e, const TypeSyntax *type,
                                const ASTNode *site) {
  if (!type) return;
  switch (type->kind) {
  case TYPE_SYNTAX_NAMED:
    if (!is_builtin_type_name(type->as.named.name,
                              type->as.named.name_length) &&
        find_class(e, type->as.named.name, type->as.named.name_length) < 0) {
      class_error(e, site, "class '%.*s' does not exist",
                  type->as.named.name, type->as.named.name_length);
    }
    break;
  case TYPE_SYNTAX_POINTER:
    validate_class_type(e, type->as.pointer.pointee, site);
    break;
  case TYPE_SYNTAX_STATIC_ARRAY:
    validate_class_type(e, type->as.static_array.element_type, site);
    validate_class_references(e, type->as.static_array.length_expression);
    break;
  case TYPE_SYNTAX_DYNAMIC_ARRAY:
    validate_class_type(e, type->as.dynamic_array.element_type, site);
    break;
  case TYPE_SYNTAX_ASSOC_ARRAY:
    validate_class_type(e, type->as.associative_array.value_type, site);
    validate_class_type(e, type->as.associative_array.key_type, site);
    break;
  case TYPE_SYNTAX_FUNCTION:
  case TYPE_SYNTAX_DELEGATE:
    validate_class_type(e, type->as.callable.return_type, site);
    for (int i = 0; i < type->as.callable.parameter_count; i++) {
      ParameterSyntax *parameter = &type->as.callable.parameters[i];
      validate_class_type(e, parameter->type, site);
      validate_class_references(e, parameter->default_value);
    }
    break;
  case TYPE_SYNTAX_QUALIFIED:
    validate_class_type(e, type->as.qualified.base_type, site);
    break;
  case TYPE_SYNTAX_TYPEOF:
    validate_class_references(e, type->as.typeof_expression.expression);
    break;
  }
}

static void validate_parameters(Emitter *e, ParameterSyntax *parameters,
                                int parameter_count, const ASTNode *site) {
  for (int i = 0; i < parameter_count; i++) {
    validate_class_type(e, parameters[i].type, site);
    validate_class_references(e, parameters[i].default_value);
  }
}

static void validate_class_references(Emitter *e, const ASTNode *node) {
  if (!node) return;
  switch (node->type) {
  case AST_VAR_DECL:
    validate_class_type(e, node->as.variable_decl.declared_type, node);
    validate_class_references(e, node->as.variable_decl.initializer);
    break;
  case AST_ASSIGN:
    validate_class_references(e, node->as.assignment.value);
    break;
  case AST_INDEX_ASSIGN:
    validate_class_references(e, node->as.index_assignment.target);
    validate_class_references(e, node->as.index_assignment.index);
    validate_class_references(e, node->as.index_assignment.value);
    break;
  case AST_MEMBER_ASSIGN:
    validate_class_references(e, node->as.member_assignment.target);
    validate_class_references(e, node->as.member_assignment.value);
    break;
  case AST_BINARY_OP:
    validate_class_references(e, node->as.binary_op.left);
    validate_class_references(e, node->as.binary_op.right);
    break;
  case AST_UNARY_OP:
    validate_class_references(e, node->as.unary_op.operand);
    break;
  case AST_CAST:
    validate_class_type(e, node->as.cast.target_type, node);
    validate_class_references(e, node->as.cast.expression);
    break;
  case AST_TERNARY_OP:
    validate_class_references(e, node->as.ternary_op.condition);
    validate_class_references(e, node->as.ternary_op.true_expr);
    validate_class_references(e, node->as.ternary_op.false_expr);
    break;
  case AST_CALL:
    for (int i = 0; i < node->as.call.argument_count; i++)
      validate_class_references(e, node->as.call.arguments[i]);
    break;
  case AST_MEMBER_ACCESS:
    validate_class_references(e, node->as.member_access.target);
    break;
  case AST_MEMBER_CALL:
    validate_class_references(e, node->as.member_call.target);
    for (int i = 0; i < node->as.member_call.argument_count; i++)
      validate_class_references(e, node->as.member_call.arguments[i]);
    break;
  case AST_ARRAY_LITERAL:
    for (int i = 0; i < node->as.array_literal.element_count; i++)
      validate_class_references(e, node->as.array_literal.elements[i]);
    break;
  case AST_INDEX:
    validate_class_references(e, node->as.index_expr.target);
    validate_class_references(e, node->as.index_expr.index);
    break;
  case AST_ARRAY_LEN_EXPR:
    validate_class_references(e, node->as.array_length_expr.target);
    break;
  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      validate_class_references(e, node->as.block.statements[i]);
    break;
  case AST_IF:
    validate_class_references(e, node->as.if_statement.condition);
    validate_class_references(e, node->as.if_statement.then_branch);
    validate_class_references(e, node->as.if_statement.else_branch);
    break;
  case AST_WHILE:
    validate_class_references(e, node->as.while_statement.condition);
    validate_class_references(e, node->as.while_statement.body);
    break;
  case AST_FOR:
    validate_class_references(e, node->as.for_statement.initializer);
    validate_class_references(e, node->as.for_statement.condition);
    validate_class_references(e, node->as.for_statement.increment);
    validate_class_references(e, node->as.for_statement.body);
    break;
  case AST_FOREACH:
    validate_class_type(e, node->as.foreach_statement.variable_type, node);
    validate_class_type(e, node->as.foreach_statement.index_type, node);
    validate_class_references(e, node->as.foreach_statement.array_expression);
    validate_class_references(e, node->as.foreach_statement.body);
    break;
  case AST_RETURN:
    validate_class_references(e, node->as.return_statement.expression);
    break;
  case AST_FUNC_DECL:
    validate_class_type(e, node->as.function_decl.return_type, node);
    validate_parameters(e, node->as.function_decl.parameters,
                        node->as.function_decl.parameter_count, node);
    validate_class_references(e, node->as.function_decl.body);
    break;
  case AST_CONSTRUCTOR_DECL:
    validate_parameters(e, node->as.constructor_decl.parameters,
                        node->as.constructor_decl.parameter_count, node);
    validate_class_references(e, node->as.constructor_decl.body);
    break;
  case AST_CLASS_DECL:
    validate_class_type(e, node->as.class_decl.base_type, node);
    for (int i = 0; i < node->as.class_decl.member_count; i++)
      validate_class_references(e, node->as.class_decl.members[i]);
    break;
  default:
    break;
  }
}

static int initializer_class(Emitter *e, int class_index) {
  while (class_index >= 0 && own_constructor_count(e, class_index) == 0)
    class_index = e->classes[class_index].base_index;
  return class_index;
}

static void write_field_name(Emitter *e, int owner, int member_index) {
  fprintf(e->out, "hd_field_%d_%d", owner, member_index);
}

static void write_type_syntax(Emitter *e, const TypeSyntax *type,
                              int permit_void) {
  int class_index = class_from_syntax(e, type);
  if (class_index >= 0) {
    write_class_name(e, class_index);
    fputs(" *", e->out);
    return;
  }
  if (type && type->kind == TYPE_SYNTAX_NAMED) {
    const char *name = type->as.named.name;
    int length = type->as.named.name_length;
    if (permit_void && (name_is(name, length, "U0") ||
                        name_is(name, length, "void"))) {
      fputs("void", e->out);
      return;
    }
    if (name_is(name, length, "F64") || name_is(name, length, "double")) {
      fputs("double", e->out);
      return;
    }
    if (name_is(name, length, "Bool") || name_is(name, length, "bool") ||
        name_is(name, length, "I8") || name_is(name, length, "U8") ||
        name_is(name, length, "I16") || name_is(name, length, "U16") ||
        name_is(name, length, "I32") || name_is(name, length, "U32") ||
        name_is(name, length, "I64") || name_is(name, length, "U64") ||
        name_is(name, length, "int")) {
      fputs("long long", e->out);
      return;
    }
  }
  emit_error(e, "class fields, parameters and results must currently use scalar or class types.");
  fputs("long long", e->out);
}

static int syntax_is_f64(Emitter *e, const TypeSyntax *type) {
  (void)e;
  return type && type->kind == TYPE_SYNTAX_NAMED &&
         (name_is(type->as.named.name, type->as.named.name_length, "F64") ||
          name_is(type->as.named.name, type->as.named.name_length, "double"));
}

static int class_of_expression(Emitter *e, ASTNode *node) {
  if (!node) return -1;
  switch (node->type) {
  case AST_THIS:
    return e->class_index;
  case AST_VAR_REF: {
    if (e->class_index >= 0) {
      for (int i = 0; i < e->class_parameter_count; i++) {
        ParameterSyntax *parameter = &e->class_parameters[i];
        if (parameter->name && same_name(parameter->name,
                                         parameter->name_length,
                                         node->as.variable_ref.name,
                                         node->as.variable_ref.name_length))
          return class_from_syntax(e, parameter->type);
      }
    }
    const HDSymbol *symbol = symbol_for(e, node, HD_BINDING_READ);
    return symbol ? class_from_type(e, HDTypeOfSymbol(e->types, symbol->id))
                  : -1;
  }
  case AST_CALL:
    return find_class(e, node->as.call.callee_name,
                      node->as.call.callee_name_length);
  case AST_MEMBER_ACCESS: {
    int target = class_of_expression(e, node->as.member_access.target);
    ASTNode *field = find_field(e, target, node->as.member_access.name,
                                node->as.member_access.name_length, NULL, NULL);
    return field ? class_from_syntax(e, field->as.variable_decl.declared_type)
                 : -1;
  }
  case AST_MEMBER_CALL: {
    int target = class_of_expression(e, node->as.member_call.target);
    ASTNode *method = find_method(e, target, node->as.member_call.name,
                                  node->as.member_call.name_length,
                                  node->as.member_call.argument_count,
                                  NULL, NULL);
    return method ? class_from_syntax(e, method->as.function_decl.return_type)
                  : -1;
  }
  default:
    return -1;
  }
}

static void emit_class_native_expression(Emitter *e, ASTNode *node);

static void emit_class_load(Emitter *e, ASTNode *node) {
  if (e->class_index >= 0) {
    for (int i = 0; i < e->class_parameter_count; i++) {
      ParameterSyntax *parameter = &e->class_parameters[i];
      if (parameter->name && same_name(parameter->name, parameter->name_length,
                                       node->as.variable_ref.name,
                                       node->as.variable_ref.name_length)) {
        fprintf(e->out, "p%d", i);
        return;
      }
    }
  }
  const HDSymbol *symbol = symbol_for(e, node, HD_BINDING_READ);
  if (symbol && symbol_representation(e, symbol) == C_REP_CLASS) {
    write_slot_name(e, symbol);
    return;
  }
  emit_error(e, "class expression names no class-valued parameter or variable.");
  fputs("NULL", e->out);
}

static void emit_field_lvalue(Emitter *e, ASTNode *target, int target_class,
                              int owner, int member_index) {
  fputc('(', e->out);
  emit_class_native_expression(e, target);
  fputc(')', e->out);
  for (int current = target_class; current >= 0 && current != owner;
       current = e->classes[current].base_index)
    fputs("->hd_base", e->out);
  fputs("->", e->out);
  write_field_name(e, owner, member_index);
}

static void emit_method_receiver(Emitter *e, ASTNode *target, int target_class,
                                 int owner) {
  if (target_class == owner) {
    emit_class_native_expression(e, target);
    return;
  }
  fputs("&(", e->out);
  emit_class_native_expression(e, target);
  fputc(')', e->out);
  for (int current = target_class; current >= 0 && current != owner;
       current = e->classes[current].base_index)
    fputs("->hd_base", e->out);
}

static void emit_constructor_call(Emitter *e, ASTNode *node) {
  int class_index = find_class(e, node->as.call.callee_name,
                               node->as.call.callee_name_length);
  int ordinal = constructor_for_arity(e, class_index,
                                      node->as.call.argument_count);
  if (ordinal < 0) {
    emit_error(e, "no class constructor matches this argument count.");
    fputs("NULL", e->out);
    return;
  }
  write_constructor_name(e, class_index, ordinal);
  fputc('(', e->out);
  for (int i = 0; i < node->as.call.argument_count; i++) {
    if (i) fputs(", ", e->out);
    emit_class_native_expression(e, node->as.call.arguments[i]);
  }
  fputc(')', e->out);
}

static void emit_class_native_expression(Emitter *e, ASTNode *node) {
  if (!node) { fputs("0", e->out); return; }
  switch (node->type) {
  case AST_NUMBER:
    fprintf(e->out, "%lldLL", node->as.integer_literal.value);
    break;
  case AST_FLOAT:
    fprintf(e->out, "%.17g", node->as.float_literal.value);
    break;
  case AST_THIS:
    if (e->class_index < 0) {
      emit_error(e, "'this' appears outside a class member.");
      fputs("NULL", e->out);
    } else fputs("self", e->out);
    break;
  case AST_VAR_REF:
    if (class_of_expression(e, node) >= 0) emit_class_load(e, node);
    else {
      int found = 0;
      if (e->class_index >= 0) {
        for (int i = 0; i < e->class_parameter_count; i++) {
          ParameterSyntax *parameter = &e->class_parameters[i];
          if (parameter->name && same_name(parameter->name, parameter->name_length,
                                           node->as.variable_ref.name,
                                           node->as.variable_ref.name_length)) {
            fprintf(e->out, "p%d", i);
            found = 1;
            break;
          }
        }
      }
      if (!found) {
        const HDSymbol *symbol = symbol_for(e, node, HD_BINDING_READ);
        if (symbol && symbol_representation(e, symbol) == C_REP_I64) {
          write_slot_name(e, symbol);
          found = 1;
        }
      }
      if (!found) {
        emit_error(e, "class member expression names an unsupported local.");
        fputs("0", e->out);
      }
    }
    break;
  case AST_BINARY_OP: {
    const char *op = direct_binop(node->as.binary_op.operator_type);
    if (!op) {
      emit_error(e, "class member uses an unsupported binary operator.");
      fputs("0", e->out);
      break;
    }
    fputc('(', e->out);
    emit_class_native_expression(e, node->as.binary_op.left);
    fprintf(e->out, " %s ", op);
    emit_class_native_expression(e, node->as.binary_op.right);
    fputc(')', e->out);
    break;
  }
  case AST_UNARY_OP:
    if (node->as.unary_op.operator_type == TOKEN_MINUS) {
      fputs("(-", e->out);
      emit_class_native_expression(e, node->as.unary_op.operand);
      fputc(')', e->out);
    } else {
      emit_error(e, "class member uses an unsupported unary operator.");
      fputs("0", e->out);
    }
    break;
  case AST_CAST: {
    HDCastKind kind;
    if (!HDCastKindFromTypeSyntax(node->as.cast.target_type, &kind)) {
      emit_error(e, "class casts require Bool, an integer type, or F64.");
      fputs("0", e->out);
      break;
    }
    if (kind == HD_CAST_FLOAT) fputs("((double)", e->out);
    else if (kind == HD_CAST_INT) fputs("((long long)", e->out);
    else fputs("((", e->out);
    emit_class_native_expression(e, node->as.cast.expression);
    fputs(kind == HD_CAST_BOOL ? ") ? 1LL : 0LL)" : ")", e->out);
    break;
  }
  case AST_CALL:
    if (find_class(e, node->as.call.callee_name,
                   node->as.call.callee_name_length) >= 0)
      emit_constructor_call(e, node);
    else {
      emit_error(e, "class member calls an unsupported non-constructor.");
      fputs("0", e->out);
    }
    break;
  case AST_MEMBER_ACCESS: {
    int target = class_of_expression(e, node->as.member_access.target);
    int owner = -1, member_index = -1;
    if (!find_field(e, target, node->as.member_access.name,
                    node->as.member_access.name_length, &owner, &member_index)) {
      missing_member_error(e, node, target, "field",
                           node->as.member_access.name,
                           node->as.member_access.name_length, 0);
      fputs("0", e->out);
    } else emit_field_lvalue(e, node->as.member_access.target, target, owner,
                              member_index);
    break;
  }
  case AST_MEMBER_CALL: {
    int target = class_of_expression(e, node->as.member_call.target);
    int owner = -1, member_index = -1;
    ASTNode *method = find_method(e, target, node->as.member_call.name,
                                  node->as.member_call.name_length,
                                  node->as.member_call.argument_count,
                                  &owner, &member_index);
    if (!method) {
      missing_member_error(e, node, target, "method",
                           node->as.member_call.name,
                           node->as.member_call.name_length,
                           node->as.member_call.argument_count);
      fputs("0", e->out);
      break;
    }
    write_method_name(e, owner, member_index);
    fputc('(', e->out);
    emit_method_receiver(e, node->as.member_call.target, target, owner);
    for (int i = 0; i < node->as.member_call.argument_count; i++) {
      fputs(", ", e->out);
      emit_class_native_expression(e, node->as.member_call.arguments[i]);
    }
    fputc(')', e->out);
    break;
  }
  default:
    emit_error(e, "unsupported expression in native class code.");
    fputs("0", e->out);
    break;
  }
}

/* Ordinary HolyD expressions still return HDValue.  A class field/method is
 * native inside the generated struct code, so this adapter boxes scalar
 * results at the boundary where the existing runtime (for example PrintLn)
 * expects an HDValue. */
static void emit_class_member_boxed(Emitter *e, ASTNode *node) {
  const TypeSyntax *type = NULL;
  if (node->type == AST_MEMBER_ACCESS) {
    int target = class_of_expression(e, node->as.member_access.target);
    ASTNode *field = find_field(e, target, node->as.member_access.name,
                                node->as.member_access.name_length, NULL, NULL);
    type = field ? field->as.variable_decl.declared_type : NULL;
  } else if (node->type == AST_MEMBER_CALL) {
    int target = class_of_expression(e, node->as.member_call.target);
    ASTNode *method = find_method(e, target, node->as.member_call.name,
                                  node->as.member_call.name_length,
                                  node->as.member_call.argument_count,
                                  NULL, NULL);
    type = method ? method->as.function_decl.return_type : NULL;
  }
  if (class_from_syntax(e, type) >= 0) {
    emit_error(e, "using a class reference as a boxed value is not implemented yet.");
    fputs("int_value(0)", e->out);
    return;
  }
  fputs(syntax_is_f64(e, type) ? "float_value(" : "int_value(", e->out);
  emit_class_native_expression(e, node);
  fputc(')', e->out);
}

static int expression_representation_is_stable(Emitter *e, ASTNode *node,
                                               CRepresentation expected) {
  if (!node)
    return 0;
  /* Constructor calls are typed by the class table rather than the general
   * checker for this first lowering slice, so recognise them before asking
   * the scalar checker for the expression's representation. */
  if (expected == C_REP_CLASS && node->type == AST_CALL &&
      find_class(e, node->as.call.callee_name,
                 node->as.call.callee_name_length) >= 0)
    return 1;
  if (node_representation(e, node) != expected)
    return 0;
  switch (node->type) {
  case AST_NUMBER:
    return expected == C_REP_I64;
  case AST_FLOAT:
    return expected == C_REP_F64;
  case AST_VAR_REF:
    return symbol_representation(
               e, symbol_for(e, node, HD_BINDING_READ)) == expected;
  case AST_BINARY_OP: {
    TokenType op = node->as.binary_op.operator_type;
    if (op == TOKEN_TILDE)
      return 0;
    CRepresentation left = node_representation(e, node->as.binary_op.left);
    CRepresentation right = node_representation(e, node->as.binary_op.right);
    return left != C_REP_BOXED && right != C_REP_BOXED &&
           expression_representation_is_stable(e, node->as.binary_op.left,
                                               left) &&
           expression_representation_is_stable(e, node->as.binary_op.right,
                                               right);
  }
  case AST_UNARY_OP:
    if (node->as.unary_op.operator_type == TOKEN_BANG)
      return expected == C_REP_I64;
    return expression_representation_is_stable(
        e, node->as.unary_op.operand,
        node_representation(e, node->as.unary_op.operand));
  case AST_TERNARY_OP:
    return expression_representation_is_stable(
               e, node->as.ternary_op.true_expr, expected) &&
           expression_representation_is_stable(
               e, node->as.ternary_op.false_expr, expected);
  case AST_ARRAY_LEN_EXPR:
    return expected == C_REP_I64;
  case AST_CALL: {
    int newline = 0;
    return expected == C_REP_I64 &&
           is_print_builtin(node->as.call.callee_name,
                            node->as.call.callee_name_length, &newline);
  }
  default:
    return 0;
  }
}

/* The checker intentionally accepts legacy boxed-runtime mismatches. Before
 * choosing native storage, prove that every value which can enter a slot has
 * the representation its semantic type predicts. This is a monotone pass:
 * when one slot becomes unsafe, dependent slots become unsafe on the next
 * iteration. Parameters are treated as typed function boundaries; call ABI
 * specialization will make that contract explicit in the next step. */
static int analyze_native_slots(Emitter *e) {
  int count = e->resolution->symbol_count;
  e->unsafe_symbols = (unsigned char *)calloc((size_t)count, 1);
  if (count && !e->unsafe_symbols)
    return 0;

  for (int i = 0; i < count; i++) {
    const HDSymbol *symbol = &e->resolution->symbols[i];
    if (symbol->flags & HD_SYMBOL_FOREACH_VALUE)
      e->unsafe_symbols[i] = 1;
  }

  int changed;
  do {
    changed = 0;
    for (int i = 0; i < count; i++) {
      const HDSymbol *symbol = &e->resolution->symbols[i];
      CRepresentation representation = symbol_representation(e, symbol);
      if (representation == C_REP_BOXED ||
          (symbol->flags & HD_SYMBOL_PARAMETER) ||
          (symbol->flags & HD_SYMBOL_FOREACH_INDEX))
        continue;

      ASTNode *declaration = (ASTNode *)symbol->declaration;
      ASTNode *value = NULL;
      if (declaration && declaration->type == AST_VAR_DECL)
        value = declaration->as.variable_decl.initializer;
      else if (declaration && declaration->type == AST_ASSIGN)
        value = declaration->as.assignment.value;
      if (!value ||
          !expression_representation_is_stable(e, value, representation)) {
        e->unsafe_symbols[i] = 1;
        changed = 1;
      }
    }

    for (int i = 0; i < e->resolution->binding_count; i++) {
      const HDBinding *binding = &e->resolution->bindings[i];
      if (binding->role != HD_BINDING_WRITE)
        continue;
      const HDSymbol *symbol =
          HDResolutionSymbol(e->resolution, binding->symbol_id);
      CRepresentation representation = symbol_representation(e, symbol);
      ASTNode *assignment = (ASTNode *)binding->node;
      if (representation != C_REP_BOXED && assignment &&
          assignment->type == AST_ASSIGN &&
          !expression_representation_is_stable(
              e, assignment->as.assignment.value, representation)) {
        e->unsafe_symbols[symbol->id] = 1;
        changed = 1;
      }
    }
  } while (changed);
  return 1;
}

static void emit_boxed_slot(Emitter *e, const HDSymbol *symbol) {
  CRepresentation representation = symbol_representation(e, symbol);
  if (representation == C_REP_I64)
    fputs("int_value(", e->out);
  else if (representation == C_REP_F64)
    fputs("float_value(", e->out);
  write_slot_name(e, symbol);
  if (representation != C_REP_BOXED)
    fputc(')', e->out);
}

static void emit_unbox_open(Emitter *e, const HDSymbol *symbol) {
  CRepresentation representation = symbol_representation(e, symbol);
  if (representation == C_REP_I64)
    fputc('(', e->out);
  else if (representation == C_REP_F64)
    fputs("HDAsDouble(", e->out);
}

static void emit_unbox_close(Emitter *e, const HDSymbol *symbol) {
  CRepresentation representation = symbol_representation(e, symbol);
  if (representation == C_REP_I64)
    fputs(").i64", e->out);
  else if (representation == C_REP_F64)
    fputc(')', e->out);
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
    emit_boxed_slot(e, symbol);
    return;
  }

  fputc('(', e->out);
  write_bound_name(e, symbol);
  fputs(" ? ", e->out);
  emit_boxed_slot(e, symbol);
  fputs(" : ", e->out);

  const HDSymbol *fallback = global_fallback(e, symbol);
  if (fallback) {
    fputc('(', e->out);
    write_bound_name(e, fallback);
    fputs(" ? ", e->out);
    emit_boxed_slot(e, fallback);
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
  emit_unbox_open(e, symbol);
}

static void emit_define_close(Emitter *e, const HDSymbol *symbol) {
  if (!symbol || symbol->storage == HD_SYMBOL_EXTERNAL) {
    fputs(");\n", e->out);
    return;
  }
  emit_unbox_close(e, symbol);
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
    emit_unbox_open(e, symbol);
    return;
  }
  *id = e->store_counter++;
  fprintf(e->out, "{ HDValue _s%d = ", *id);
}

static void emit_store_close(Emitter *e, const HDSymbol *symbol,
                             const char *name, int length, int indent,
                             int id) {
  if (id < 0) {
    if (symbol && symbol->storage != HD_SYMBOL_EXTERNAL)
      emit_unbox_close(e, symbol);
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
  fputs(" = ", e->out);
  emit_unbox_open(e, symbol);
  fprintf(e->out, "_s%d", id);
  emit_unbox_close(e, symbol);
  fputs(";\n", e->out);

  if (fallback) {
    indent_by(e, indent + 1);
    fputs("else if (", e->out);
    write_bound_name(e, fallback);
    fputs(") ", e->out);
    write_slot_name(e, fallback);
    fputs(" = ", e->out);
    emit_unbox_open(e, fallback);
    fprintf(e->out, "_s%d", id);
    emit_unbox_close(e, fallback);
    fputs(";\n", e->out);
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
  fputs(" = ", e->out);
  emit_unbox_open(e, symbol);
  fprintf(e->out, "_s%d", id);
  emit_unbox_close(e, symbol);
  fputs("; ", e->out);
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
  case AST_CAST:
    count_expression(e, node->as.cast.expression, max_argc, capacity);
    break;
  case AST_TERNARY_OP:
    count_expression(e, node->as.ternary_op.condition, max_argc, capacity);
    count_expression(e, node->as.ternary_op.true_expr, max_argc, capacity);
    count_expression(e, node->as.ternary_op.false_expr, max_argc, capacity);
    break;
  case AST_ARRAY_LEN_EXPR:
    count_expression(e, node->as.array_length_expr.target, max_argc, capacity);
    break;
  case AST_MEMBER_ACCESS:
    count_expression(e, node->as.member_access.target, max_argc, capacity);
    break;
  case AST_MEMBER_CALL:
    count_expression(e, node->as.member_call.target, max_argc, capacity);
    for (int i = 0; i < node->as.member_call.argument_count; i++)
      count_expression(e, node->as.member_call.arguments[i], max_argc, capacity);
    break;
  case AST_CALL:
    /* A class constructor lowers directly to a native factory and therefore
     * needs no HDValue argument array. */
    if (find_class(e, node->as.call.callee_name,
                   node->as.call.callee_name_length) < 0)
      count_call(e, node, node->as.call.argument_count, max_argc, capacity);
    for (int i = 0; i < node->as.call.argument_count; i++)
      count_expression(e, node->as.call.arguments[i], max_argc, capacity);
    break;
  case AST_ARRAY_LITERAL:
    count_call(e, node, node->as.array_literal.element_count, max_argc,
               capacity);
    for (int i = 0; i < node->as.array_literal.element_count; i++)
      count_expression(e, node->as.array_literal.elements[i], max_argc,
                       capacity);
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
  case AST_MEMBER_ASSIGN:
    count_expression(e, node->as.member_assignment.target, max_argc, capacity);
    count_expression(e, node->as.member_assignment.value, max_argc, capacity);
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
  case AST_BREAK:
  case AST_CONTINUE:
    /* None of these takes a call-site temporary. Spelled out rather than
     * left to the default, because this walk and emit_statement have to
     * allocate ids in lockstep. */
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

static void emit_array_expression(Emitter *e, ASTNode *node) {
  int id = e->temp_counter++;
  int count = node->as.array_literal.element_count;
  fputc('(', e->out);
  emit_args_into(e, id, node->as.array_literal.elements, count);
  fprintf(e->out, "HDArrayNewX(%d, _t%d))", count, id);
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

static const char *direct_binop(TokenType op) {
  switch (op) {
  case TOKEN_PLUS: return "+";
  case TOKEN_MINUS: return "-";
  case TOKEN_STAR: return "*";
  case TOKEN_EQEQ: return "==";
  case TOKEN_NEQ: return "!=";
  case TOKEN_LT: return "<";
  case TOKEN_GT: return ">";
  case TOKEN_LTEQ: return "<=";
  case TOKEN_GTEQ: return ">=";
  default: return NULL;
  }
}

static int direct_scalar_expression(Emitter *e, ASTNode *node) {
  if (!node || node_representation(e, node) == C_REP_BOXED)
    return 0;
  switch (node->type) {
  case AST_BINARY_OP:
    {
      CRepresentation left =
          node_representation(e, node->as.binary_op.left);
      CRepresentation right =
          node_representation(e, node->as.binary_op.right);
      return direct_binop(node->as.binary_op.operator_type) != NULL &&
             left != C_REP_BOXED && right != C_REP_BOXED &&
             expression_representation_is_stable(
                 e, node->as.binary_op.left, left) &&
             expression_representation_is_stable(
                 e, node->as.binary_op.right, right);
    }
  case AST_UNARY_OP:
    return node->as.unary_op.operator_type == TOKEN_MINUS &&
           expression_representation_is_stable(
               e, node->as.unary_op.operand,
               node_representation(e, node->as.unary_op.operand));
  case AST_TERNARY_OP:
    /* The boxed runtime preserves the selected arm's representation. A
     * mixed I64/F64 conditional therefore cannot be specialized yet even
     * though semantic common-type inference reports F64. */
    return expression_representation_is_stable(
               e, node->as.ternary_op.true_expr,
               node_representation(e, node)) &&
           expression_representation_is_stable(
               e, node->as.ternary_op.false_expr,
               node_representation(e, node));
  default:
    return 0;
  }
}

static void emit_scalar_value(Emitter *e, ASTNode *node,
                              CRepresentation wanted);

static void emit_direct_scalar_body(Emitter *e, ASTNode *node) {
  CRepresentation result = node_representation(e, node);
  switch (node->type) {
  case AST_BINARY_OP: {
    const char *op = direct_binop(node->as.binary_op.operator_type);
    CRepresentation left = node_representation(e, node->as.binary_op.left);
    CRepresentation right = node_representation(e, node->as.binary_op.right);
    int comparison = node->as.binary_op.operator_type == TOKEN_EQEQ ||
                     node->as.binary_op.operator_type == TOKEN_NEQ ||
                     node->as.binary_op.operator_type == TOKEN_LT ||
                     node->as.binary_op.operator_type == TOKEN_GT ||
                     node->as.binary_op.operator_type == TOKEN_LTEQ ||
                     node->as.binary_op.operator_type == TOKEN_GTEQ;
    CRepresentation operands =
        left == C_REP_F64 || right == C_REP_F64 ? C_REP_F64 : C_REP_I64;
    if (!comparison)
      operands = result;
    fputc('(', e->out);
    emit_scalar_value(e, node->as.binary_op.left, operands);
    fprintf(e->out, " %s ", op);
    emit_scalar_value(e, node->as.binary_op.right, operands);
    fputc(')', e->out);
    break;

  }
  case AST_UNARY_OP:
    fputs("(-", e->out);
    emit_scalar_value(e, node->as.unary_op.operand, result);
    fputc(')', e->out);
    break;
  case AST_TERNARY_OP:
    fputs("(HDTruthy(", e->out);
    emit_expression(e, node->as.ternary_op.condition);
    fputs(") ? ", e->out);
    emit_scalar_value(e, node->as.ternary_op.true_expr, result);
    fputs(" : ", e->out);
    emit_scalar_value(e, node->as.ternary_op.false_expr, result);
    fputc(')', e->out);
    break;
  default:
    emit_error(e, "internal direct-scalar mismatch.");
    fputc('0', e->out);
    break;
  }
}

/* Produce an unboxed scalar at a typed boundary. Directly read parameters,
 * literals, and recursively-specialized arithmetic; less certain constructs
 * still use their boxed implementation and are unwrapped once. */
static void emit_scalar_value(Emitter *e, ASTNode *node,
                              CRepresentation wanted) {
  CRepresentation actual = node_representation(e, node);
  int convert = actual != wanted && actual != C_REP_BOXED;
  if (convert)
    fputs(wanted == C_REP_F64 ? "((double)" : "((long long)", e->out);

  if (node && node->type == AST_NUMBER) {
    fprintf(e->out, "%lldLL", node->as.integer_literal.value);
  } else if (node && node->type == AST_FLOAT) {
    fprintf(e->out, "((double)%.17g)", node->as.float_literal.value);
  } else if (node && node->type == AST_VAR_REF) {
    const HDSymbol *symbol = symbol_for(e, node, HD_BINDING_READ);
    CRepresentation symbol_rep = symbol_representation(e, symbol);
    if (symbol_rep != C_REP_BOXED) {
      if (!has_bound_bit(symbol)) {
        write_slot_name(e, symbol);
      } else {
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
          if (symbol_representation(e, fallback) == wanted) {
            write_slot_name(e, fallback);
          } else if (wanted == C_REP_F64) {
            fputs("HDAsDouble(", e->out);
            emit_boxed_slot(e, fallback);
            fputc(')', e->out);
          } else {
            fputc('(', e->out);
            emit_boxed_slot(e, fallback);
            fputs(").i64", e->out);
          }
          fputs(" : ", e->out);
        }
        if (wanted == C_REP_F64)
          fputs("HDAsDouble(HDLoadX(", e->out);
        else
          fputs("(HDLoadX(", e->out);
        write_env_name(e, node->as.variable_ref.name,
                       node->as.variable_ref.name_length);
        fputs(wanted == C_REP_F64 ? "))" : ")).i64", e->out);
        if (fallback)
          fputc(')', e->out);
        fputc(')', e->out);
      }
    } else if (wanted == C_REP_F64) {
      fputs("HDAsDouble(", e->out);
      emit_expression(e, node);
      fputc(')', e->out);
    } else {
      fputc('(', e->out);
      emit_expression(e, node);
      fputs(").i64", e->out);
    }
  } else if (direct_scalar_expression(e, node)) {
    emit_direct_scalar_body(e, node);
  } else if (wanted == C_REP_F64) {
    fputs("HDAsDouble(", e->out);
    emit_expression(e, node);
    fputc(')', e->out);
  } else {
    fputc('(', e->out);
    emit_expression(e, node);
    fputs(").i64", e->out);
  }

  if (convert)
    fputc(')', e->out);
}

static void emit_boxed_direct_scalar(Emitter *e, ASTNode *node) {
  CRepresentation representation = node_representation(e, node);
  fputs(representation == C_REP_F64 ? "float_value(" : "int_value(", e->out);
  emit_direct_scalar_body(e, node);
  fputc(')', e->out);
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

  case AST_MEMBER_ACCESS:
  case AST_MEMBER_CALL:
    emit_class_member_boxed(e, node);
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

    if (direct_scalar_expression(e, node)) {
      emit_boxed_direct_scalar(e, node);
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
      if (direct_scalar_expression(e, node))
        emit_boxed_direct_scalar(e, node);
      else {
        fputs("HDBinaryX(HD_SUB, int_value(0), ", e->out);
        emit_expression(e, node->as.unary_op.operand);
        fputc(')', e->out);
      }
    } else if (node->as.unary_op.operator_type == TOKEN_BANG) {
      fputs("HDNot(", e->out);
      emit_expression(e, node->as.unary_op.operand);
      fputc(')', e->out);
    } else {
      emit_error(e, "unsupported unary operator.");
      fputs("int_value(0)", e->out);
    }
    break;

  case AST_CAST: {
    HDCastKind kind;
    if (!HDCastKindFromTypeSyntax(node->as.cast.target_type, &kind)) {
      emit_error(e, "cast target must be Bool, an integer type, or F64.");
      fputs("int_value(0)", e->out);
      break;
    }
    fprintf(e->out, "HDCastX(%d, ", (int)kind);
    emit_expression(e, node->as.cast.expression);
    fputc(')', e->out);
    break;
  }

  case AST_TERNARY_OP:
    if (direct_scalar_expression(e, node)) {
      emit_boxed_direct_scalar(e, node);
    } else {
      fputs("(HDTruthy(", e->out);
      emit_expression(e, node->as.ternary_op.condition);
      fputs(") ? ", e->out);
      emit_expression(e, node->as.ternary_op.true_expr);
      fputs(" : ", e->out);
      emit_expression(e, node->as.ternary_op.false_expr);
      fputc(')', e->out);
    }
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

  case AST_ARRAY_LITERAL:
    emit_array_expression(e, node);
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
    CRepresentation representation = symbol_representation(e, symbol);
    if (representation == C_REP_CLASS) {
      indent_by(e, indent);
      write_slot_name(e, symbol);
      fputs(" = ", e->out);
      if (node->as.variable_decl.initializer)
        emit_class_native_expression(e, node->as.variable_decl.initializer);
      else
        fputs("NULL", e->out);
      fputs(";", e->out);
      if (has_bound_bit(symbol)) {
        fputc(' ', e->out);
        write_bound_name(e, symbol);
        fputs(" = 1;", e->out);
      }
      fputc('\n', e->out);
    } else if (representation != C_REP_BOXED) {
      indent_by(e, indent);
      write_slot_name(e, symbol);
      fputs(" = ", e->out);
      emit_scalar_value(e, node->as.variable_decl.initializer, representation);
      fputs(";", e->out);
      if (has_bound_bit(symbol)) {
        fputc(' ', e->out);
        write_bound_name(e, symbol);
        fputs(" = 1;", e->out);
      }
      fputc('\n', e->out);
    } else {
      emit_define_open(e, symbol, node->as.variable_decl.name,
                       node->as.variable_decl.name_length, indent);
      emit_expression(e, node->as.variable_decl.initializer);
      emit_define_close(e, symbol);
    }
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

  case AST_MEMBER_ASSIGN: {
    ASTNode *target = node->as.member_assignment.target;
    if (!target || target->type != AST_MEMBER_ACCESS) {
      emit_error(e, "member assignment target is not a member access.");
      break;
    }
    int target_class = class_of_expression(e, target->as.member_access.target);
    int owner = -1, member_index = -1;
    ASTNode *field = find_field(e, target_class, target->as.member_access.name,
                                target->as.member_access.name_length, &owner,
                                &member_index);
    if (!field) {
      missing_member_error(e, node, target_class, "field",
                           target->as.member_access.name,
                           target->as.member_access.name_length, 0);
      break;
    }
    indent_by(e, indent);
    emit_field_lvalue(e, target->as.member_access.target, target_class, owner,
                      member_index);
    fputs(" = ", e->out);
    emit_class_native_expression(e, node->as.member_assignment.value);
    fputs(";\n", e->out);
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

  case AST_WHILE: {
    int outer_continue = e->continue_label;
    e->continue_label = -1; /* nothing sits between body and test to skip */
    indent_by(e, indent);
    fputs("while (HDTruthy(", e->out);
    emit_expression(e, node->as.while_statement.condition);
    fputs(")) {\n", e->out);
    emit_body(e, node->as.while_statement.body, indent + 1);
    indent_by(e, indent);
    fputs("}\n", e->out);
    e->continue_label = outer_continue;
    break;
  }

  /* Written as init + while rather than a C for, because the init and the
   * increment are statements in HolyD, and because it keeps the order the
   * VM uses visible: condition, body, increment. */
  case AST_FOR: {
    int outer_continue = e->continue_label;
    /* The increment is emitted at the end of the body, so a bare C
     * `continue` would jump over it and spin. Continues land on a label in
     * front of it instead, which is where the VM patches them too. */
    e->continue_label = has_own_continue(node->as.for_statement.body)
                            ? e->label_counter++
                            : -1;
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
    if (e->continue_label >= 0) {
      indent_by(e, indent + 2);
      fprintf(e->out, "_c%d: ;\n", e->continue_label);
    }
    if (node->as.for_statement.increment)
      emit_statement(e, node->as.for_statement.increment, indent + 2);
    indent_by(e, indent + 1);
    fputs("}\n", e->out);
    indent_by(e, indent);
    fputs("}\n", e->out);
    e->continue_label = outer_continue;
    break;
  }

  /* The array is snapshotted once and the length re-read each turn, which
   * is what the VM's desugaring does: it stores the array in a temporary
   * and applies BC_ARRAY_LENGTH to that temporary every iteration. The
   * element is defined before the index, in that order. */
  case AST_FOREACH: {
    int id = e->temp_counter++;
    int outer_continue = e->continue_label;
    /* Same reason as for: the counter bump is at the end of the body, and
     * skipping it would never terminate. */
    e->continue_label = has_own_continue(node->as.foreach_statement.body)
                            ? e->label_counter++
                            : -1;
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

    if (e->continue_label >= 0) {
      indent_by(e, indent + 2);
      fprintf(e->out, "_c%d: ;\n", e->continue_label);
    }
    indent_by(e, indent + 2);
    fprintf(e->out, "_fe%d++;\n", id);
    indent_by(e, indent + 1);
    fputs("}\n", e->out);
    indent_by(e, indent);
    fputs("}\n", e->out);
    e->continue_label = outer_continue;
    break;
  }

  case AST_FUNC_DECL:
    break;

  /* Every HolyD loop lowers to a C loop whose body holds the HolyD body, so
   * break is C's break in all three cases. Continue is only C's continue
   * when there is nothing after the body to run first. */
  case AST_BREAK:
    indent_by(e, indent);
    fputs("break;\n", e->out);
    break;

  case AST_CONTINUE:
    indent_by(e, indent);
    if (e->continue_label >= 0)
      fprintf(e->out, "goto _c%d;\n", e->continue_label);
    else
      fputs("continue;\n", e->out);
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
  case AST_CAST:
    return expr_calls(e, node->as.cast.expression, name, len);
  case AST_ARRAY_LEN_EXPR:
    return expr_calls(e, node->as.array_length_expr.target, name, len);
  case AST_CALL:
    if (node->as.call.callee_name_length == len &&
        strncmp(node->as.call.callee_name, name, (size_t)len) == 0) {
      return 1;
    }
    for (int i = 0; i < node->as.call.argument_count; i++)
      if (expr_calls(e, node->as.call.arguments[i], name, len))
        return 1;
    return 0;
  case AST_ARRAY_LITERAL:
    for (int i = 0; i < node->as.array_literal.element_count; i++)
      if (expr_calls(e, node->as.array_literal.elements[i], name, len))
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

static void emit_parameter_list(Emitter *e, ParameterSyntax *parameters,
                                int parameter_count, int with_self,
                                int self_class) {
  int wrote = 0;
  if (with_self) {
    write_class_name(e, self_class);
    fputs(" *self", e->out);
    wrote = 1;
  }
  for (int i = 0; i < parameter_count; i++) {
    if (wrote) fputs(", ", e->out);
    write_type_syntax(e, parameters[i].type, 0);
    fprintf(e->out, " p%d", i);
    wrote = 1;
  }
  if (!wrote) fputs("void", e->out);
}

static void emit_class_statement(Emitter *e, ASTNode *node, int indent,
                                 int returns_void) {
  if (!node) return;
  switch (node->type) {
  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      emit_class_statement(e, node->as.block.statements[i], indent,
                           returns_void);
    break;
  case AST_MEMBER_ASSIGN: {
    ASTNode *target = node->as.member_assignment.target;
    if (!target || target->type != AST_MEMBER_ACCESS) {
      emit_error(e, "class assignment target is not a field.");
      break;
    }
    int target_class = class_of_expression(e, target->as.member_access.target);
    int owner = -1, member_index = -1;
    ASTNode *field = find_field(e, target_class, target->as.member_access.name,
                                target->as.member_access.name_length, &owner,
                                &member_index);
    if (!field) {
      missing_member_error(e, node, target_class, "field",
                           target->as.member_access.name,
                           target->as.member_access.name_length, 0);
      break;
    }
    indent_by(e, indent);
    emit_field_lvalue(e, target->as.member_access.target, target_class, owner,
                      member_index);
    fputs(" = ", e->out);
    emit_class_native_expression(e, node->as.member_assignment.value);
    fputs(";\n", e->out);
    break;
  }
  case AST_VAR_DECL:
    /* A class body has no resolver frame yet, so its locals use a stable
     * member-local spelling.  This is intentionally small but enough for
     * constructor temporaries without leaking them into global resolution. */
    indent_by(e, indent);
    write_type_syntax(e, node->as.variable_decl.declared_type, 0);
    fprintf(e->out, " hd_local_%.*s", node->as.variable_decl.name_length,
            node->as.variable_decl.name);
    if (node->as.variable_decl.initializer) {
      fputs(" = ", e->out);
      emit_class_native_expression(e, node->as.variable_decl.initializer);
    }
    fputs(";\n", e->out);
    break;
  case AST_RETURN:
    indent_by(e, indent);
    if (returns_void) {
      fputs("return;\n", e->out);
    } else {
      fputs("return ", e->out);
      emit_class_native_expression(e, node->as.return_statement.expression);
      fputs(";\n", e->out);
    }
    break;
  case AST_IF:
    indent_by(e, indent);
    fputs("if (", e->out);
    emit_class_native_expression(e, node->as.if_statement.condition);
    fputs(") {\n", e->out);
    emit_class_statement(e, node->as.if_statement.then_branch, indent + 1,
                         returns_void);
    indent_by(e, indent);
    fputs("}\n", e->out);
    if (node->as.if_statement.else_branch) {
      indent_by(e, indent);
      fputs("else {\n", e->out);
      emit_class_statement(e, node->as.if_statement.else_branch, indent + 1,
                           returns_void);
      indent_by(e, indent);
      fputs("}\n", e->out);
    }
    break;
  case AST_MEMBER_CALL:
  case AST_CALL:
    indent_by(e, indent);
    fputs("(void)", e->out);
    emit_class_native_expression(e, node);
    fputs(";\n", e->out);
    break;
  default:
    emit_error(e, "unsupported statement in a class member.");
    break;
  }
}

static void emit_field_initializers(Emitter *e, int class_index, int indent) {
  ASTClassDecl *decl = &e->classes[class_index].node->as.class_decl;
  for (int i = 0; i < decl->member_count; i++) {
    ASTNode *field = decl->members[i];
    if (!field || field->type != AST_VAR_DECL ||
        !field->as.variable_decl.initializer)
      continue;
    indent_by(e, indent);
    fputs("self->", e->out);
    write_field_name(e, class_index, i);
    fputs(" = ", e->out);
    emit_class_native_expression(e, field->as.variable_decl.initializer);
    fputs(";\n", e->out);
  }
}

static void emit_class_layouts(Emitter *e) {
  for (int c = 0; c < e->class_count; c++) {
    fputs("typedef struct ", e->out);
    write_class_name(e, c);
    fputc(' ', e->out);
    write_class_name(e, c);
    fputs(";\n", e->out);
  }
  if (e->class_count) fputc('\n', e->out);

  for (int c = 0; c < e->class_count; c++) {
    int base = e->classes[c].base_index;
    if (base > c) {
      emit_error(e, "a class base must be declared before its derived class for C emission.");
      continue;
    }
    fputs("struct ", e->out);
    write_class_name(e, c);
    fputs(" {\n", e->out);
    if (base >= 0) {
      fputs("  ", e->out);
      write_class_name(e, base);
      fputs(" hd_base;\n", e->out);
    }
    ASTClassDecl *decl = &e->classes[c].node->as.class_decl;
    for (int i = 0; i < decl->member_count; i++) {
      ASTNode *field = decl->members[i];
      if (!field || field->type != AST_VAR_DECL) continue;
      fputs("  ", e->out);
      write_type_syntax(e, field->as.variable_decl.declared_type, 0);
      fputc(' ', e->out);
      write_field_name(e, c, i);
      fputs(";\n", e->out);
    }
    fputs("};\n\n", e->out);
  }
}

static void emit_class_prototypes(Emitter *e) {
  for (int c = 0; c < e->class_count; c++) {
    int constructors = constructor_count(e, c);
    for (int ordinal = 0; ordinal < constructors; ordinal++) {
      ASTNode *constructor = constructor_parameters_from(e, c, ordinal);
      ParameterSyntax *parameters = constructor
                                       ? constructor->as.constructor_decl.parameters
                                       : NULL;
      int parameter_count = constructor
                                ? constructor->as.constructor_decl.parameter_count
                                : 0;
      fputs("static HD_CLASS_UNUSED ", e->out);
      write_class_name(e, c);
      fputs(" *", e->out);
      write_constructor_name(e, c, ordinal);
      fputc('(', e->out);
      emit_parameter_list(e, parameters, parameter_count, 0, c);
      fputs(");\n", e->out);
    }
    ASTClassDecl *decl = &e->classes[c].node->as.class_decl;
    for (int i = 0; i < decl->member_count; i++) {
      ASTNode *method = decl->members[i];
      if (!method || method->type != AST_FUNC_DECL) continue;
      fputs("static HD_CLASS_UNUSED ", e->out);
      write_type_syntax(e, method->as.function_decl.return_type, 1);
      fputc(' ', e->out);
      write_method_name(e, c, i);
      fputc('(', e->out);
      emit_parameter_list(e, method->as.function_decl.parameters,
                          method->as.function_decl.parameter_count, 1, c);
      fputs(");\n", e->out);
    }
  }
  if (e->class_count) fputc('\n', e->out);
}

static void emit_initializer_receiver(Emitter *e, int class_index,
                                      int owner) {
  fputs("&self->hd_base", e->out);
  for (int current = e->classes[class_index].base_index;
       current >= 0 && current != owner;
       current = e->classes[current].base_index)
    fputs(".hd_base", e->out);
}

static void emit_class_definitions(Emitter *e) {
  for (int c = 0; c < e->class_count; c++) {
    int constructors = constructor_count(e, c);
    for (int ordinal = 0; ordinal < constructors; ordinal++) {
      int owner = initializer_class(e, c);
      ASTNode *constructor = constructor_parameters_from(e, c, ordinal);
      ParameterSyntax *parameters = constructor
                                       ? constructor->as.constructor_decl.parameters
                                       : NULL;
      int parameter_count = constructor
                                ? constructor->as.constructor_decl.parameter_count
                                : 0;
      if (owner == c && constructor) {
        fputs("static void ", e->out);
        write_initializer_name(e, c, ordinal);
        fputc('(', e->out);
        emit_parameter_list(e, parameters, parameter_count, 1, c);
        fputs(") {\n", e->out);
        e->class_index = c;
        e->class_parameters = parameters;
        e->class_parameter_count = parameter_count;
        emit_field_initializers(e, c, 1);
        emit_class_statement(e, constructor->as.constructor_decl.body, 1, 1);
        e->class_index = -1;
        e->class_parameters = NULL;
        e->class_parameter_count = 0;
        fputs("}\n\n", e->out);
      }

      fputs("static HD_CLASS_UNUSED ", e->out);
      write_class_name(e, c);
      fputs(" *", e->out);
      write_constructor_name(e, c, ordinal);
      fputc('(', e->out);
      emit_parameter_list(e, parameters, parameter_count, 0, c);
      fputs(") {\n  ", e->out);
      write_class_name(e, c);
      fputs(" *self = calloc(1, sizeof(*self));\n  if (!self) return NULL;\n",
            e->out);
      if (owner == c && constructor) {
        fputs("  ", e->out);
        write_initializer_name(e, c, ordinal);
        fputs("(self", e->out);
      } else if (owner >= 0) {
        fputs("  ", e->out);
        write_initializer_name(e, owner, ordinal);
        fputc('(', e->out);
        emit_initializer_receiver(e, c, owner);
      } else {
        fputs("  /* implicit zero-argument constructor */", e->out);
      }
      if ((owner == c && constructor) || owner >= 0) {
        for (int i = 0; i < parameter_count; i++) fprintf(e->out, ", p%d", i);
        fputs(");\n", e->out);
      } else fputc('\n', e->out);
      if (owner != c)
        emit_field_initializers(e, c, 1);
      fputs("  return self;\n}\n\n", e->out);
    }

    ASTClassDecl *decl = &e->classes[c].node->as.class_decl;
    for (int i = 0; i < decl->member_count; i++) {
      ASTNode *method = decl->members[i];
      if (!method || method->type != AST_FUNC_DECL) continue;
      int returns_void = method->as.function_decl.return_type &&
                         method->as.function_decl.return_type->kind == TYPE_SYNTAX_NAMED &&
                         (name_is(method->as.function_decl.return_type->as.named.name,
                                  method->as.function_decl.return_type->as.named.name_length,
                                  "U0") ||
                          name_is(method->as.function_decl.return_type->as.named.name,
                                  method->as.function_decl.return_type->as.named.name_length,
                                  "void"));
      fputs("static HD_CLASS_UNUSED ", e->out);
      write_type_syntax(e, method->as.function_decl.return_type, 1);
      fputc(' ', e->out);
      write_method_name(e, c, i);
      fputc('(', e->out);
      emit_parameter_list(e, method->as.function_decl.parameters,
                          method->as.function_decl.parameter_count, 1, c);
      fputs(") {\n", e->out);
      e->class_index = c;
      e->class_parameters = method->as.function_decl.parameters;
      e->class_parameter_count = method->as.function_decl.parameter_count;
      fputs("  (void)self;\n", e->out);
      emit_class_statement(e, method->as.function_decl.body, 1, returns_void);
      if (returns_void) fputs("  return;\n", e->out);
      else fputs("  return 0;\n", e->out);
      e->class_index = -1;
      e->class_parameters = NULL;
      e->class_parameter_count = 0;
      fputs("}\n\n", e->out);
    }
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
    CRepresentation representation = symbol_representation(e, symbol);
    if (representation == C_REP_CLASS) {
      write_class_name(e, class_from_type(e, HDTypeOfSymbol(e->types, symbol->id)));
      fputs(" *", e->out);
    } else {
      fputs(representation == C_REP_I64 ? "long long "
            : representation == C_REP_F64 ? "double "
                                          : "HDValue ",
            e->out);
    }
    write_slot_name(e, symbol);
    if (parameter >= 0) {
      fputs(" = ", e->out);
      if (representation == C_REP_I64)
        fprintf(e->out, "p%d.i64;\n", parameter);
      else if (representation == C_REP_F64)
        fprintf(e->out, "HDAsDouble(p%d);\n", parameter);
      else
        fprintf(e->out, "p%d;\n", parameter);
    } else if (representation == C_REP_BOXED) {
      fputs(" = {0};\n", e->out);
    } else if (representation == C_REP_CLASS) {
      fputs(" = NULL;\n", e->out);
    } else {
      fputs(" = 0;\n", e->out);
    }

    if (has_bound_bit(symbol)) {
      indent_by(e, indent);
      fputs("int ", e->out);
      write_bound_name(e, symbol);
      fputs(" = 0;\n", e->out);
      if (representation == C_REP_CLASS) {
        indent_by(e, indent);
        fputs("(void)", e->out);
        write_bound_name(e, symbol);
        fputs(";\n", e->out);
      }
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

int HDEmitC(ASTNode *ast, const HDResolution *resolution,
            const HDTypeCheck *types, FILE *out, const char *source_name) {
  Emitter e;
  e.out = out;
  e.resolution = resolution;
  e.types = types;
  e.unsafe_symbols = NULL;
  e.functions = NULL;
  e.function_count = 0;
  e.classes = NULL;
  e.class_count = 0;
  e.class_index = -1;
  e.class_parameters = NULL;
  e.class_parameter_count = 0;
  e.temp_counter = 0;
  e.store_counter = 0;
  e.continue_label = -1;
  e.label_counter = 0;
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

  e.classes = (CClass *)calloc((size_t)ast->as.block.statement_count + 1,
                               sizeof(CClass));
  if (!e.classes) {
    emit_error(&e, "out of memory while collecting classes.");
    free(e.functions);
    return 0;
  }
  for (int i = 0; i < ast->as.block.statement_count; i++) {
    ASTNode *stmt = ast->as.block.statements[i];
    if (stmt && stmt->type == AST_CLASS_DECL) {
      e.classes[e.class_count].node = stmt;
      e.classes[e.class_count].base_index = -1;
      e.class_count++;
    }
  }
  for (int i = 0; i < e.class_count; i++) {
    TypeSyntax *base = e.classes[i].node->as.class_decl.base_type;
    if (!base) continue;
    e.classes[i].base_index = class_from_syntax(&e, base);
  }

  if (e.class_count > 0)
    validate_class_references(&e, ast);
  if (e.had_error) {
    free(e.classes);
    free(e.functions);
    return 0;
  }

  if (!analyze_native_slots(&e)) {
    emit_error(&e, "out of memory while selecting native scalar slots.");
    free(e.functions);
    free(e.classes);
    return 0;
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
  fputs("#if defined(__GNUC__) || defined(__clang__)\n"
        "#define HD_CLASS_UNUSED __attribute__((unused))\n"
        "#else\n"
        "#define HD_CLASS_UNUSED\n"
        "#endif\n\n", out);
  /* The Environment is no longer where variables live. It holds the FFI
   * constants, and it is the last link in a name's fallback chain, which is
   * both where those constants are found and where an undefined name
   * finally becomes an error. */
  fputs("static Environment hd_globals;\n\n", out);

  emit_class_layouts(&e);
  emit_class_prototypes(&e);
  emit_class_definitions(&e);

  /* One static per global slot. Zero-initialised, so nothing is bound until
   * a declaration or an assignment runs, exactly as in the VM. */
  if (resolution->global_slot_count > 0) {
    for (int slot = 0; slot < resolution->global_slot_count; slot++) {
      const HDSymbol *symbol = HDResolutionSlotSymbol(resolution, -1, slot);
      if (!symbol)
        continue;
      CRepresentation representation = symbol_representation(&e, symbol);
      if (representation == C_REP_CLASS) {
        fputs("static ", out);
        write_class_name(&e, class_from_type(&e, HDTypeOfSymbol(types, symbol->id)));
        fputs(" *", out);
      } else {
        fputs(representation == C_REP_I64 ? "static long long "
              : representation == C_REP_F64 ? "static double "
                                            : "static HDValue ",
              out);
      }
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
      free(e.unsafe_symbols);
      free(e.functions);
      free(e.classes);
      return 0;
    }
    for (int i = 0; i < ast->as.block.statement_count; i++) {
      ASTNode *stmt = ast->as.block.statements[i];
      if (!stmt || stmt->type == AST_FUNC_DECL || stmt->type == AST_CLASS_DECL)
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
  free(e.unsafe_symbols);
  free(e.functions);
  free(e.classes);
  return !e.had_error;
}
