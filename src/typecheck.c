#include "typecheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TYPE_ERROR_ID,
  TYPE_UNKNOWN_ID,
  TYPE_AUTO_ID,
  TYPE_VOID_ID,
  TYPE_BOOL_ID,
  TYPE_I8_ID,
  TYPE_U8_ID,
  TYPE_I16_ID,
  TYPE_U16_ID,
  TYPE_I32_ID,
  TYPE_U32_ID,
  TYPE_I64_ID,
  TYPE_U64_ID,
  TYPE_F64_ID,
  TYPE_STRING_ID,
  TYPE_BUILTIN_COUNT
};

typedef struct {
  HDTypeCheck *result;
  const HDResolution *resolution;
  ASTNode *program;
} Checker;

static int grow(void **items, int *capacity, size_t item_size) {
  int next = *capacity ? *capacity * 2 : 32;
  void *memory = realloc(*items, (size_t)next * item_size);
  if (!memory)
    return 0;
  *items = memory;
  *capacity = next;
  return 1;
}

void HDTypeCheckInit(HDTypeCheck *result) {
  memset(result, 0, sizeof(*result));
}

void HDTypeCheckFree(HDTypeCheck *result) {
  if (!result)
    return;
  for (int i = 0; i < result->type_count; i++)
    free(result->types[i].parameters);
  free(result->types);
  free(result->nodes);
  free(result->symbol_types);
  HDTypeCheckInit(result);
}

static HDTypeId add_type(HDTypeCheck *result, HDType type) {
  if (result->type_count >= result->type_capacity &&
      !grow((void **)&result->types, &result->type_capacity, sizeof(HDType))) {
    result->had_error = 1;
    return TYPE_ERROR_ID;
  }
  result->types[result->type_count] = type;
  return result->type_count++;
}

static int same_type(const HDType *a, const HDType *b) {
  if (a->kind != b->kind || a->primary != b->primary ||
      a->secondary != b->secondary || a->name_length != b->name_length ||
      a->has_array_length != b->has_array_length ||
      a->array_length != b->array_length ||
      a->parameter_count != b->parameter_count || a->qualifier != b->qualifier)
    return 0;
  if (a->name_length && strncmp(a->name, b->name, (size_t)a->name_length) != 0)
    return 0;
  for (int i = 0; i < a->parameter_count; i++)
    if (a->parameters[i] != b->parameters[i])
      return 0;
  return 1;
}

static HDTypeId intern_type(HDTypeCheck *result, HDType type) {
  for (int i = 0; i < result->type_count; i++)
    if (same_type(&result->types[i], &type))
      return i;

  if (type.parameter_count > 0) {
    HDTypeId *copy = malloc(sizeof(HDTypeId) * (size_t)type.parameter_count);
    if (!copy) {
      result->had_error = 1;
      return TYPE_ERROR_ID;
    }
    memcpy(copy, type.parameters,
           sizeof(HDTypeId) * (size_t)type.parameter_count);
    type.parameters = copy;
  }
  return add_type(result, type);
}

static HDTypeId simple_type(HDTypeCheck *result, HDTypeKind kind) {
  HDType type;
  memset(&type, 0, sizeof(type));
  type.kind = kind;
  type.primary = HD_NO_TYPE;
  type.secondary = HD_NO_TYPE;
  return add_type(result, type);
}

static int text_is(const char *text, int length, const char *expected) {
  return (int)strlen(expected) == length &&
         strncmp(text, expected, (size_t)length) == 0;
}

static int is_integer(HDTypeId type) {
  return type >= TYPE_BOOL_ID && type <= TYPE_U64_ID;
}

static int integer_rank(HDTypeId type) {
  static const int ranks[] = {0, 0, 0, 1, 1, 2, 2, 3, 3};
  return is_integer(type) ? ranks[type - TYPE_BOOL_ID] : -1;
}

static int is_unsigned_integer(HDTypeId type) {
  return type == TYPE_U8_ID || type == TYPE_U16_ID ||
         type == TYPE_U32_ID || type == TYPE_U64_ID;
}

static HDTypeId integer_of_rank(int rank, int is_unsigned) {
  static const HDTypeId signed_types[] = {
      TYPE_I8_ID, TYPE_I16_ID, TYPE_I32_ID, TYPE_I64_ID};
  static const HDTypeId unsigned_types[] = {
      TYPE_U8_ID, TYPE_U16_ID, TYPE_U32_ID, TYPE_U64_ID};
  if (rank < 0) rank = 0;
  if (rank > 3) rank = 3;
  return is_unsigned ? unsigned_types[rank] : signed_types[rank];
}

static HDTypeId common_type(HDTypeCheck *result, HDTypeId left,
                            HDTypeId right) {
  if (left == right)
    return left;
  if (left == TYPE_UNKNOWN_ID || left == TYPE_AUTO_ID)
    return right;
  if (right == TYPE_UNKNOWN_ID || right == TYPE_AUTO_ID)
    return left;
  if (left == TYPE_F64_ID && (right == TYPE_F64_ID || is_integer(right)))
    return TYPE_F64_ID;
  if (right == TYPE_F64_ID && is_integer(left))
    return TYPE_F64_ID;
  if (is_integer(left) && is_integer(right)) {
    int left_rank = integer_rank(left);
    int right_rank = integer_rank(right);
    int rank = left_rank > right_rank ? left_rank : right_rank;
    int use_unsigned = left_rank == right_rank
                           ? is_unsigned_integer(left) ||
                                 is_unsigned_integer(right)
                           : left_rank > right_rank
                                 ? is_unsigned_integer(left)
                                 : is_unsigned_integer(right);
    return integer_of_rank(rank, use_unsigned);
  }
  const HDType *a = HDTypeGet(result, left);
  const HDType *b = HDTypeGet(result, right);
  if (a && b &&
      (a->kind == HD_TYPE_DYNAMIC_ARRAY || a->kind == HD_TYPE_STATIC_ARRAY) &&
      (b->kind == HD_TYPE_DYNAMIC_ARRAY || b->kind == HD_TYPE_STATIC_ARRAY)) {
    HDType array;
    memset(&array, 0, sizeof(array));
    array.kind = HD_TYPE_DYNAMIC_ARRAY;
    array.primary = common_type(result, a->primary, b->primary);
    array.secondary = HD_NO_TYPE;
    return intern_type(result, array);
  }
  return TYPE_UNKNOWN_ID;
}

static HDTypeId type_from_syntax(Checker *checker, TypeSyntax *syntax);
static HDTypeId infer_expr(Checker *checker, ASTNode *node);
static void check_stmt(Checker *checker, ASTNode *node);

static HDTypeId named_type(HDTypeCheck *result, const char *name, int length) {
  if (text_is(name, length, "U0") || text_is(name, length, "void"))
    return TYPE_VOID_ID;
  if (text_is(name, length, "bool")) return TYPE_BOOL_ID;
  if (text_is(name, length, "I8")) return TYPE_I8_ID;
  if (text_is(name, length, "U8")) return TYPE_U8_ID;
  if (text_is(name, length, "I16")) return TYPE_I16_ID;
  if (text_is(name, length, "U16")) return TYPE_U16_ID;
  if (text_is(name, length, "I32") || text_is(name, length, "int"))
    return TYPE_I32_ID;
  if (text_is(name, length, "U32") || text_is(name, length, "uint"))
    return TYPE_U32_ID;
  if (text_is(name, length, "I64") || text_is(name, length, "long"))
    return TYPE_I64_ID;
  if (text_is(name, length, "U64") || text_is(name, length, "ulong"))
    return TYPE_U64_ID;
  if (text_is(name, length, "F64") || text_is(name, length, "double"))
    return TYPE_F64_ID;
  if (text_is(name, length, "string")) return TYPE_STRING_ID;
  if (text_is(name, length, "auto")) return TYPE_AUTO_ID;

  HDType type;
  memset(&type, 0, sizeof(type));
  type.kind = HD_TYPE_NAMED;
  type.primary = type.secondary = HD_NO_TYPE;
  type.name = name;
  type.name_length = length;
  return intern_type(result, type);
}

static HDTypeId callable_type(Checker *checker, TypeSyntax *syntax,
                              HDTypeKind kind) {
  HDType type;
  memset(&type, 0, sizeof(type));
  type.kind = kind;
  type.primary = type_from_syntax(checker, syntax->as.callable.return_type);
  type.secondary = HD_NO_TYPE;
  type.parameter_count = syntax->as.callable.parameter_count;
  if (type.parameter_count > 0) {
    type.parameters = malloc(sizeof(HDTypeId) * (size_t)type.parameter_count);
    if (!type.parameters) {
      checker->result->had_error = 1;
      return TYPE_ERROR_ID;
    }
    for (int i = 0; i < type.parameter_count; i++)
      type.parameters[i] = type_from_syntax(
          checker, syntax->as.callable.parameters[i].type);
  }
  HDTypeId id = intern_type(checker->result, type);
  free(type.parameters);
  return id;
}

static HDTypeId type_from_syntax(Checker *checker, TypeSyntax *syntax) {
  if (!syntax)
    return TYPE_UNKNOWN_ID;
  HDType type;
  memset(&type, 0, sizeof(type));
  type.primary = type.secondary = HD_NO_TYPE;
  switch (syntax->kind) {
  case TYPE_SYNTAX_NAMED:
    return named_type(checker->result, syntax->as.named.name,
                      syntax->as.named.name_length);
  case TYPE_SYNTAX_POINTER:
    type.kind = HD_TYPE_POINTER;
    type.primary = type_from_syntax(checker, syntax->as.pointer.pointee);
    return intern_type(checker->result, type);
  case TYPE_SYNTAX_STATIC_ARRAY:
    type.kind = HD_TYPE_STATIC_ARRAY;
    type.primary = type_from_syntax(checker, syntax->as.static_array.element_type);
    if (syntax->as.static_array.length_expression &&
        syntax->as.static_array.length_expression->type == AST_NUMBER) {
      type.has_array_length = 1;
      type.array_length =
          syntax->as.static_array.length_expression->as.integer_literal.value;
    }
    return intern_type(checker->result, type);
  case TYPE_SYNTAX_DYNAMIC_ARRAY:
    type.kind = HD_TYPE_DYNAMIC_ARRAY;
    type.primary = type_from_syntax(checker, syntax->as.dynamic_array.element_type);
    return intern_type(checker->result, type);
  case TYPE_SYNTAX_ASSOC_ARRAY:
    type.kind = HD_TYPE_ASSOC_ARRAY;
    type.primary = type_from_syntax(checker, syntax->as.associative_array.value_type);
    type.secondary = type_from_syntax(checker, syntax->as.associative_array.key_type);
    return intern_type(checker->result, type);
  case TYPE_SYNTAX_FUNCTION:
    return callable_type(checker, syntax, HD_TYPE_FUNCTION);
  case TYPE_SYNTAX_DELEGATE:
    return callable_type(checker, syntax, HD_TYPE_DELEGATE);
  case TYPE_SYNTAX_QUALIFIED:
    type.kind = HD_TYPE_QUALIFIED;
    type.primary = type_from_syntax(checker, syntax->as.qualified.base_type);
    type.qualifier = syntax->as.qualified.qualifier;
    return intern_type(checker->result, type);
  case TYPE_SYNTAX_TYPEOF:
    return infer_expr(checker, syntax->as.typeof_expression.expression);
  }
  return TYPE_ERROR_ID;
}

static void set_node_type(HDTypeCheck *result, const ASTNode *node,
                          HDTypeId type) {
  if (!node)
    return;
  for (int i = 0; i < result->node_count; i++) {
    if (result->nodes[i].node == node) {
      result->nodes[i].type = type;
      return;
    }
  }
  if (result->node_count >= result->node_capacity &&
      !grow((void **)&result->nodes, &result->node_capacity,
            sizeof(HDNodeType))) {
    result->had_error = 1;
    return;
  }
  result->nodes[result->node_count].node = node;
  result->nodes[result->node_count].type = type;
  result->node_count++;
}

HDTypeId HDTypeOfNode(const HDTypeCheck *result, const ASTNode *node) {
  if (!result || !node)
    return HD_NO_TYPE;
  for (int i = result->node_count - 1; i >= 0; i--)
    if (result->nodes[i].node == node)
      return result->nodes[i].type;
  return HD_NO_TYPE;
}

HDTypeId HDTypeOfSymbol(const HDTypeCheck *result, int symbol_id) {
  if (!result || symbol_id < 0 || symbol_id >= result->symbol_count)
    return HD_NO_TYPE;
  return result->symbol_types[symbol_id];
}

const HDType *HDTypeGet(const HDTypeCheck *result, HDTypeId type) {
  if (!result || type < 0 || type >= result->type_count)
    return NULL;
  return &result->types[type];
}

static int binding_symbol(Checker *checker, ASTNode *node,
                          HDBindingRole role) {
  const HDBinding *binding =
      HDResolutionBinding(checker->resolution, node, role);
  return binding ? binding->symbol_id : HD_NO_SYMBOL;
}

static ASTNode *find_function(Checker *checker, const char *name, int length) {
  for (int i = 0; i < checker->resolution->function_count; i++) {
    ASTNode *fn = (ASTNode *)checker->resolution->functions[i].declaration;
    if (fn->as.function_decl.name_length == length &&
        strncmp(fn->as.function_decl.name, name, (size_t)length) == 0)
      return fn;
  }
  return NULL;
}

static HDTypeId infer_expr(Checker *checker, ASTNode *node) {
  if (!node)
    return TYPE_VOID_ID;
  HDTypeCheck *result = checker->result;
  HDTypeId type = TYPE_UNKNOWN_ID;
  switch (node->type) {
  case AST_NUMBER:
    type = node->as.integer_literal.is_boolean ? TYPE_BOOL_ID : TYPE_I64_ID;
    break;
  case AST_FLOAT:
    type = TYPE_F64_ID;
    break;
  case AST_STRING:
    type = TYPE_STRING_ID;
    break;
  case AST_VAR_REF: {
    int symbol = binding_symbol(checker, node, HD_BINDING_READ);
    type = HDTypeOfSymbol(result, symbol);
    if (type == HD_NO_TYPE) type = TYPE_UNKNOWN_ID;
    break;
  }
  case AST_ASSIGN: {
    HDTypeId value = infer_expr(checker, node->as.assignment.value);
    int symbol = binding_symbol(checker, node, HD_BINDING_WRITE);
    type = HDTypeOfSymbol(result, symbol);
    if (symbol >= 0 && (type == TYPE_UNKNOWN_ID || type == TYPE_AUTO_ID))
      result->symbol_types[symbol] = type = value;
    break;
  }
  case AST_BINARY_OP: {
    HDTypeId left = infer_expr(checker, node->as.binary_op.left);
    HDTypeId right = infer_expr(checker, node->as.binary_op.right);
    TokenType op = node->as.binary_op.operator_type;
    if (op == TOKEN_EQEQ || op == TOKEN_NEQ || op == TOKEN_LT ||
        op == TOKEN_GT || op == TOKEN_LTEQ || op == TOKEN_GTEQ ||
        op == TOKEN_ANDAND || op == TOKEN_OROR) {
      type = TYPE_BOOL_ID;
    } else if (op == TOKEN_TILDE &&
               (left == TYPE_STRING_ID || right == TYPE_STRING_ID)) {
      type = TYPE_STRING_ID;
    } else {
      type = common_type(result, left, right);
    }
    break;
  }
  case AST_UNARY_OP:
    type = node->as.unary_op.operator_type == TOKEN_BANG
               ? TYPE_BOOL_ID
               : infer_expr(checker, node->as.unary_op.operand);
    if (node->as.unary_op.operator_type == TOKEN_BANG)
      infer_expr(checker, node->as.unary_op.operand);
    break;
  case AST_TERNARY_OP: {
    infer_expr(checker, node->as.ternary_op.condition);
    HDTypeId yes = infer_expr(checker, node->as.ternary_op.true_expr);
    HDTypeId no = infer_expr(checker, node->as.ternary_op.false_expr);
    type = common_type(result, yes, no);
    break;
  }
  case AST_ARRAY_LITERAL: {
    HDTypeId element = TYPE_UNKNOWN_ID;
    for (int i = 0; i < node->as.array_literal.element_count; i++)
      element = common_type(result, element,
                            infer_expr(checker, node->as.array_literal.elements[i]));
    HDType array;
    memset(&array, 0, sizeof(array));
    array.kind = HD_TYPE_DYNAMIC_ARRAY;
    array.primary = element;
    array.secondary = HD_NO_TYPE;
    type = intern_type(result, array);
    break;
  }
  case AST_INDEX: {
    HDTypeId target = infer_expr(checker, node->as.index_expr.target);
    infer_expr(checker, node->as.index_expr.index);
    const HDType *container = HDTypeGet(result, target);
    if (target == TYPE_STRING_ID)
      type = TYPE_U8_ID;
    else if (container &&
             (container->kind == HD_TYPE_STATIC_ARRAY ||
              container->kind == HD_TYPE_DYNAMIC_ARRAY ||
              container->kind == HD_TYPE_ASSOC_ARRAY))
      type = container->primary;
    break;
  }
  case AST_ARRAY_LEN_EXPR:
    infer_expr(checker, node->as.array_length_expr.target);
    type = TYPE_I64_ID;
    break;
  case AST_CALL: {
    for (int i = 0; i < node->as.call.argument_count; i++)
      infer_expr(checker, node->as.call.arguments[i]);
    ASTNode *fn = find_function(checker, node->as.call.callee_name,
                                node->as.call.callee_name_length);
    if (fn)
      type = type_from_syntax(checker, fn->as.function_decl.return_type);
    else if (text_is(node->as.call.callee_name,
                     node->as.call.callee_name_length, "Str"))
      type = TYPE_STRING_ID;
    else if (text_is(node->as.call.callee_name,
                     node->as.call.callee_name_length, "Print") ||
             text_is(node->as.call.callee_name,
                     node->as.call.callee_name_length, "PrintLn"))
      type = TYPE_I64_ID;
    break;
  }
  default:
    break;
  }
  set_node_type(result, node, type);
  return type;
}

static void check_stmt(Checker *checker, ASTNode *node) {
  if (!node)
    return;
  HDTypeCheck *result = checker->result;
  switch (node->type) {
  case AST_VAR_DECL: {
    HDTypeId declared = type_from_syntax(checker, node->as.variable_decl.declared_type);
    HDTypeId initial = node->as.variable_decl.initializer
                           ? infer_expr(checker, node->as.variable_decl.initializer)
                           : TYPE_UNKNOWN_ID;
    HDTypeId type = declared == TYPE_AUTO_ID ? initial : declared;
    int symbol = binding_symbol(checker, node, HD_BINDING_DECLARATION);
    if (symbol >= 0) result->symbol_types[symbol] = type;
    set_node_type(result, node, type);
    break;
  }
  case AST_ASSIGN:
  case AST_INDEX:
  case AST_ARRAY_LEN_EXPR:
  case AST_CALL:
  case AST_ARRAY_LITERAL:
  case AST_BINARY_OP:
  case AST_UNARY_OP:
  case AST_TERNARY_OP:
  case AST_VAR_REF:
  case AST_NUMBER:
  case AST_FLOAT:
  case AST_STRING:
    infer_expr(checker, node);
    break;
  case AST_INDEX_ASSIGN:
    infer_expr(checker, node->as.index_assignment.target);
    infer_expr(checker, node->as.index_assignment.index);
    infer_expr(checker, node->as.index_assignment.value);
    set_node_type(result, node, TYPE_VOID_ID);
    break;
  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      check_stmt(checker, node->as.block.statements[i]);
    set_node_type(result, node, TYPE_VOID_ID);
    break;
  case AST_IF:
    infer_expr(checker, node->as.if_statement.condition);
    check_stmt(checker, node->as.if_statement.then_branch);
    check_stmt(checker, node->as.if_statement.else_branch);
    set_node_type(result, node, TYPE_VOID_ID);
    break;
  case AST_WHILE:
    infer_expr(checker, node->as.while_statement.condition);
    check_stmt(checker, node->as.while_statement.body);
    set_node_type(result, node, TYPE_VOID_ID);
    break;
  case AST_FOR:
    check_stmt(checker, node->as.for_statement.initializer);
    infer_expr(checker, node->as.for_statement.condition);
    check_stmt(checker, node->as.for_statement.body);
    check_stmt(checker, node->as.for_statement.increment);
    set_node_type(result, node, TYPE_VOID_ID);
    break;
  case AST_FOREACH: {
    HDTypeId array = infer_expr(checker, node->as.foreach_statement.array_expression);
    const HDType *container = HDTypeGet(result, array);
    HDTypeId element = container &&
                               (container->kind == HD_TYPE_STATIC_ARRAY ||
                                container->kind == HD_TYPE_DYNAMIC_ARRAY)
                           ? container->primary
                           : TYPE_UNKNOWN_ID;
    HDTypeId value = type_from_syntax(
        checker, node->as.foreach_statement.variable_type);
    if (value == TYPE_AUTO_ID) value = element;
    int value_symbol = binding_symbol(checker, node, HD_BINDING_FOREACH_VALUE);
    if (value_symbol >= 0) result->symbol_types[value_symbol] = value;
    if (node->as.foreach_statement.index_name) {
      HDTypeId index = type_from_syntax(
          checker, node->as.foreach_statement.index_type);
      if (index == TYPE_AUTO_ID) index = TYPE_I64_ID;
      int index_symbol = binding_symbol(checker, node, HD_BINDING_FOREACH_INDEX);
      if (index_symbol >= 0) result->symbol_types[index_symbol] = index;
    }
    check_stmt(checker, node->as.foreach_statement.body);
    set_node_type(result, node, TYPE_VOID_ID);
    break;
  }
  case AST_FUNC_DECL:
    check_stmt(checker, node->as.function_decl.body);
    {
      HDType function;
      memset(&function, 0, sizeof(function));
      function.kind = HD_TYPE_FUNCTION;
      function.primary =
          type_from_syntax(checker, node->as.function_decl.return_type);
      function.secondary = HD_NO_TYPE;
      function.parameter_count = node->as.function_decl.parameter_count;
      if (function.parameter_count > 0) {
        function.parameters = malloc(sizeof(HDTypeId) *
                                     (size_t)function.parameter_count);
        if (!function.parameters) {
          result->had_error = 1;
          set_node_type(result, node, TYPE_ERROR_ID);
          break;
        }
        for (int i = 0; i < function.parameter_count; i++) {
          function.parameters[i] = type_from_syntax(
              checker, node->as.function_decl.parameters[i].type);
        }
      }
      set_node_type(result, node, intern_type(result, function));
      free(function.parameters);
    }
    break;
  case AST_RETURN:
    infer_expr(checker, node->as.return_statement.expression);
    set_node_type(result, node, TYPE_VOID_ID);
    break;
  case AST_LABEL:
  case AST_GOTO:
  case AST_BREAK:
  case AST_CONTINUE:
    set_node_type(result, node, TYPE_VOID_ID);
    break;
  }
}

static void seed_symbol_types(Checker *checker) {
  HDTypeCheck *result = checker->result;
  const HDResolution *resolution = checker->resolution;
  result->symbol_count = resolution->symbol_count;
  result->symbol_types = malloc(sizeof(HDTypeId) * (size_t)result->symbol_count);
  if (result->symbol_count && !result->symbol_types) {
    result->had_error = 1;
    return;
  }
  for (int i = 0; i < result->symbol_count; i++) {
    const HDSymbol *symbol = &resolution->symbols[i];
    HDTypeId type = TYPE_UNKNOWN_ID;
    if (symbol->flags & HD_SYMBOL_PARAMETER) {
      type = type_from_syntax(checker,
                              ((ParameterSyntax *)symbol->declaration)->type);
    } else if (symbol->flags & HD_SYMBOL_FOREACH_VALUE) {
      type = type_from_syntax(
          checker, ((ASTNode *)symbol->declaration)
                       ->as.foreach_statement.variable_type);
    } else if (symbol->flags & HD_SYMBOL_FOREACH_INDEX) {
      type = type_from_syntax(
          checker, ((ASTNode *)symbol->declaration)
                       ->as.foreach_statement.index_type);
    } else if (!(symbol->flags & HD_SYMBOL_IMPLICIT) &&
               symbol->storage != HD_SYMBOL_EXTERNAL && symbol->declaration) {
      type = type_from_syntax(
          checker, ((ASTNode *)symbol->declaration)->as.variable_decl.declared_type);
    }
    result->symbol_types[i] = type;
  }
}

int HDTypeCheckProgram(ASTNode *program, const HDResolution *resolution,
                       HDTypeCheck *result) {
  HDTypeCheckInit(result);
  static const HDTypeKind builtins[] = {
      HD_TYPE_ERROR, HD_TYPE_UNKNOWN, HD_TYPE_AUTO, HD_TYPE_VOID, HD_TYPE_BOOL,
      HD_TYPE_I8, HD_TYPE_U8, HD_TYPE_I16, HD_TYPE_U16, HD_TYPE_I32,
      HD_TYPE_U32, HD_TYPE_I64, HD_TYPE_U64, HD_TYPE_F64, HD_TYPE_STRING};
  for (int i = 0; i < TYPE_BUILTIN_COUNT; i++)
    simple_type(result, builtins[i]);
  Checker checker = {result, resolution, program};
  seed_symbol_types(&checker);
  check_stmt(&checker, program);
  if (result->had_error)
    printf("Type error: out of memory while building semantic types.\n");
  return !result->had_error;
}

static void print_type(const HDTypeCheck *result, HDTypeId id) {
  const HDType *type = HDTypeGet(result, id);
  if (!type) {
    fputs("<none>", stdout);
    return;
  }
  static const char *simple[] = {"<error>", "?", "auto", "void", "bool",
      "I8", "U8", "I16", "U16", "I32", "U32", "I64", "U64", "F64",
      "string"};
  if (type->kind <= HD_TYPE_STRING) {
    fputs(simple[type->kind], stdout);
    return;
  }
  switch (type->kind) {
  case HD_TYPE_NAMED:
    printf("%.*s", type->name_length, type->name);
    break;
  case HD_TYPE_POINTER:
    print_type(result, type->primary); fputc('*', stdout);
    break;
  case HD_TYPE_STATIC_ARRAY:
    print_type(result, type->primary);
    if (type->has_array_length) printf("[%lld]", type->array_length);
    else fputs("[?]", stdout);
    break;
  case HD_TYPE_DYNAMIC_ARRAY:
    print_type(result, type->primary); fputs("[]", stdout);
    break;
  case HD_TYPE_ASSOC_ARRAY:
    print_type(result, type->primary); fputc('[', stdout);
    print_type(result, type->secondary); fputc(']', stdout);
    break;
  case HD_TYPE_FUNCTION:
  case HD_TYPE_DELEGATE:
    print_type(result, type->primary);
    fputs(type->kind == HD_TYPE_FUNCTION ? " function(" : " delegate(", stdout);
    for (int i = 0; i < type->parameter_count; i++) {
      if (i) fputs(", ", stdout);
      print_type(result, type->parameters[i]);
    }
    fputc(')', stdout);
    break;
  case HD_TYPE_QUALIFIED: {
    static const char *names[] = {"const", "immutable", "shared", "inout"};
    printf("%s(", names[type->qualifier]);
    print_type(result, type->primary); fputc(')', stdout);
    break;
  }
  default:
    fputs("?", stdout);
    break;
  }
}

static const char *node_name(ASTNodeType type) {
  static const char *names[] = {
      "integer", "float", "string", "declaration", "reference", "assignment",
      "index-assignment", "binary", "unary", "ternary", "call", "array",
      "index", "length", "block", "if", "while", "for", "foreach", "goto",
      "label", "function", "class", "struct", "enum", "enum-value", "break",
      "continue", "return"};
  int count = (int)(sizeof(names) / sizeof(names[0]));
  int index = (int)type;
  return index >= 0 && index < count ? names[index] : "node";
}

static void print_nodes(const HDTypeCheck *result, const ASTNode *node) {
  if (!node) return;
  HDTypeId type = HDTypeOfNode(result, node);
  printf("  %zu:%zu-%zu:%zu %-16s : ", node->span.start_line,
         node->span.start_column, node->span.end_line, node->span.end_column,
         node_name(node->type));
  print_type(result, type);
  fputc('\n', stdout);
  switch (node->type) {
  case AST_BINARY_OP:
    print_nodes(result, node->as.binary_op.left);
    print_nodes(result, node->as.binary_op.right); break;
  case AST_UNARY_OP: print_nodes(result, node->as.unary_op.operand); break;
  case AST_TERNARY_OP:
    print_nodes(result, node->as.ternary_op.condition);
    print_nodes(result, node->as.ternary_op.true_expr);
    print_nodes(result, node->as.ternary_op.false_expr); break;
  case AST_CALL:
    for (int i = 0; i < node->as.call.argument_count; i++)
      print_nodes(result, node->as.call.arguments[i]);
    break;
  case AST_ARRAY_LITERAL:
    for (int i = 0; i < node->as.array_literal.element_count; i++)
      print_nodes(result, node->as.array_literal.elements[i]);
    break;
  case AST_INDEX:
    print_nodes(result, node->as.index_expr.target);
    print_nodes(result, node->as.index_expr.index); break;
  case AST_ARRAY_LEN_EXPR:
    print_nodes(result, node->as.array_length_expr.target); break;
  case AST_VAR_DECL:
    print_nodes(result, node->as.variable_decl.initializer); break;
  case AST_ASSIGN:
    print_nodes(result, node->as.assignment.value); break;
  case AST_INDEX_ASSIGN:
    print_nodes(result, node->as.index_assignment.target);
    print_nodes(result, node->as.index_assignment.index);
    print_nodes(result, node->as.index_assignment.value); break;
  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      print_nodes(result, node->as.block.statements[i]);
    break;
  case AST_IF:
    print_nodes(result, node->as.if_statement.condition);
    print_nodes(result, node->as.if_statement.then_branch);
    print_nodes(result, node->as.if_statement.else_branch); break;
  case AST_WHILE:
    print_nodes(result, node->as.while_statement.condition);
    print_nodes(result, node->as.while_statement.body); break;
  case AST_FOR:
    print_nodes(result, node->as.for_statement.initializer);
    print_nodes(result, node->as.for_statement.condition);
    print_nodes(result, node->as.for_statement.increment);
    print_nodes(result, node->as.for_statement.body); break;
  case AST_FOREACH:
    print_nodes(result, node->as.foreach_statement.array_expression);
    print_nodes(result, node->as.foreach_statement.body); break;
  case AST_FUNC_DECL:
    print_nodes(result, node->as.function_decl.body); break;
  case AST_RETURN:
    print_nodes(result, node->as.return_statement.expression); break;
  default: break;
  }
}

void HDTypeCheckPrint(const HDTypeCheck *result, const HDResolution *resolution,
                      const ASTNode *program) {
  fputs("Symbols:\n", stdout);
  for (int i = 0; i < resolution->symbol_count; i++) {
    const HDSymbol *symbol = &resolution->symbols[i];
    printf("  #%d %-16.*s : ", i, symbol->name_length, symbol->name);
    print_type(result, HDTypeOfSymbol(result, i));
    fputc('\n', stdout);
  }
  fputs("Nodes:\n", stdout);
  print_nodes(result, program);
}
