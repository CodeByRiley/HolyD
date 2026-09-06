#include "resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define HD_NO_FUNCTION (-1)

typedef struct {
  HDResolution *result;
  int function_index;
} Resolver;

static int same_name(const char *left, int left_length, const char *right,
                     int right_length) {
  return left_length == right_length &&
         strncmp(left, right, (size_t)left_length) == 0;
}

static void resolution_error(HDResolution *resolution, const char *message) {
  if (!resolution->had_error)
    printf("Resolve error: %s\n", message);
  resolution->had_error = 1;
}

void HDResolutionInit(HDResolution *resolution) {
  memset(resolution, 0, sizeof(*resolution));
}

void HDResolutionFree(HDResolution *resolution) {
  free(resolution->symbols);
  free(resolution->bindings);
  for (int i = 0; i < resolution->function_count; i++)
    free(resolution->functions[i].parameter_slots);
  free(resolution->functions);
  free(resolution->labels);
  free(resolution->gotos);
  free(resolution->binding_index);
  HDResolutionInit(resolution);
}

static int grow_array(void **items, int *capacity, size_t item_size) {
  int new_capacity = *capacity ? *capacity * 2 : 16;
  void *grown = realloc(*items, item_size * (size_t)new_capacity);
  if (!grown)
    return 0;
  *items = grown;
  *capacity = new_capacity;
  return 1;
}

static int find_symbol(const HDResolution *resolution, const char *name,
                       int name_length, HDSymbolStorage storage,
                       int function_index) {
  for (int i = 0; i < resolution->symbol_count; i++) {
    const HDSymbol *symbol = &resolution->symbols[i];
    if (symbol->storage == storage &&
        symbol->function_index == function_index &&
        same_name(symbol->name, symbol->name_length, name, name_length)) {
      return symbol->id;
    }
  }
  return HD_NO_SYMBOL;
}

static int add_symbol(Resolver *resolver, const char *name, int name_length,
                      HDSymbolStorage storage, unsigned flags,
                      const void *declaration) {
  HDResolution *resolution = resolver->result;
  int function_index = storage == HD_SYMBOL_LOCAL
                           ? resolver->function_index
                           : HD_NO_FUNCTION;
  int existing = find_symbol(resolution, name, name_length, storage,
                             function_index);
  if (existing != HD_NO_SYMBOL) {
    HDSymbol *symbol = &resolution->symbols[existing];
    symbol->flags |= flags;
    if (!symbol->declaration)
      symbol->declaration = declaration;
    return existing;
  }

  if (resolution->symbol_count >= resolution->symbol_capacity &&
      !grow_array((void **)&resolution->symbols,
                  &resolution->symbol_capacity, sizeof(HDSymbol))) {
    resolution_error(resolution, "out of memory while recording symbols.");
    return HD_NO_SYMBOL;
  }

  HDSymbol *symbol = &resolution->symbols[resolution->symbol_count];
  symbol->id = resolution->symbol_count++;
  symbol->storage = storage;
  symbol->function_index = function_index;
  symbol->flags = flags;
  symbol->name = name;
  symbol->name_length = name_length;
  symbol->declaration = declaration;
  symbol->fallback_id = HD_NO_SYMBOL;

  if (storage == HD_SYMBOL_GLOBAL) {
    symbol->slot = resolution->global_slot_count++;
  } else if (storage == HD_SYMBOL_LOCAL) {
    HDResolvedFunction *function =
        &resolution->functions[resolver->function_index];
    symbol->slot = function->frame_slot_count++;
  } else {
    symbol->slot = -1;
  }
  return symbol->id;
}

static int ensure_external(Resolver *resolver, const char *name,
                           int name_length) {
  return add_symbol(resolver, name, name_length, HD_SYMBOL_EXTERNAL, 0, NULL);
}

static int add_binding(Resolver *resolver, const ASTNode *node,
                       HDBindingRole role, int symbol_id) {
  HDResolution *resolution = resolver->result;
  if (symbol_id == HD_NO_SYMBOL)
    return 0;
  if (resolution->binding_count >= resolution->binding_capacity &&
      !grow_array((void **)&resolution->bindings,
                  &resolution->binding_capacity, sizeof(HDBinding))) {
    resolution_error(resolution, "out of memory while recording bindings.");
    return 0;
  }
  HDBinding *binding = &resolution->bindings[resolution->binding_count++];
  binding->node = node;
  binding->role = role;
  binding->symbol_id = symbol_id;
  return 1;
}

static int add_function(HDResolution *resolution, const ASTNode *node) {
  if (resolution->function_count >= resolution->function_capacity &&
      !grow_array((void **)&resolution->functions,
                  &resolution->function_capacity,
                  sizeof(HDResolvedFunction))) {
    resolution_error(resolution,
                     "out of memory while recording function scopes.");
    return HD_NO_FUNCTION;
  }
  int index = resolution->function_count++;
  resolution->functions[index].declaration = node;
  resolution->functions[index].frame_slot_count = 0;
  resolution->functions[index].parameter_slots = NULL;
  resolution->functions[index].parameter_count = 0;
  return index;
}

/* Knuth multiplicative over the pointer, mixed with the role so the two
 * bindings a foreach node owns land in different buckets. */
static unsigned binding_hash(const ASTNode *node, int role) {
  uintptr_t bits = (uintptr_t)node;
  unsigned h = (unsigned)(bits ^ (bits >> 16 >> 16));
  return (h * 2654435761u) ^ ((unsigned)role * 0x9e3779b9u);
}

static void build_binding_index(HDResolution *resolution);

static void collect_explicit(Resolver *resolver, ASTNode *node);
static void collect_implicit(Resolver *resolver, ASTNode *node);
static void resolve_node(Resolver *resolver, ASTNode *node);

static void visit_type(TypeSyntax *type,
                       void (*visit)(Resolver *, ASTNode *),
                       Resolver *resolver) {
  if (!type)
    return;
  switch (type->kind) {
  case TYPE_SYNTAX_POINTER:
    visit_type(type->as.pointer.pointee, visit, resolver);
    break;
  case TYPE_SYNTAX_STATIC_ARRAY:
    visit_type(type->as.static_array.element_type, visit, resolver);
    visit(resolver, type->as.static_array.length_expression);
    break;
  case TYPE_SYNTAX_DYNAMIC_ARRAY:
    visit_type(type->as.dynamic_array.element_type, visit, resolver);
    break;
  case TYPE_SYNTAX_ASSOC_ARRAY:
    visit_type(type->as.associative_array.value_type, visit, resolver);
    visit_type(type->as.associative_array.key_type, visit, resolver);
    break;
  case TYPE_SYNTAX_FUNCTION:
  case TYPE_SYNTAX_DELEGATE:
    visit_type(type->as.callable.return_type, visit, resolver);
    for (int i = 0; i < type->as.callable.parameter_count; i++) {
      ParameterSyntax *parameter = &type->as.callable.parameters[i];
      visit_type(parameter->type, visit, resolver);
      visit(resolver, parameter->default_value);
    }
    break;
  case TYPE_SYNTAX_QUALIFIED:
    visit_type(type->as.qualified.base_type, visit, resolver);
    break;
  case TYPE_SYNTAX_TYPEOF:
    visit(resolver, type->as.typeof_expression.expression);
    break;
  case TYPE_SYNTAX_NAMED:
    break;
  }
}

static void visit_children(ASTNode *node,
                           void (*visit)(Resolver *, ASTNode *),
                           Resolver *resolver) {
  if (!node)
    return;
  switch (node->type) {
  case AST_BINARY_OP:
    visit(resolver, node->as.binary_op.left);
    visit(resolver, node->as.binary_op.right);
    break;
  case AST_UNARY_OP:
    visit(resolver, node->as.unary_op.operand);
    break;
  case AST_TERNARY_OP:
    visit(resolver, node->as.ternary_op.condition);
    visit(resolver, node->as.ternary_op.true_expr);
    visit(resolver, node->as.ternary_op.false_expr);
    break;
  case AST_CALL:
    for (int i = 0; i < node->as.call.argument_count; i++)
      visit(resolver, node->as.call.arguments[i]);
    break;
  case AST_ARRAY_LITERAL:
    for (int i = 0; i < node->as.array_literal.element_count; i++)
      visit(resolver, node->as.array_literal.elements[i]);
    break;
  case AST_INDEX:
    visit(resolver, node->as.index_expr.target);
    visit(resolver, node->as.index_expr.index);
    break;
  case AST_ARRAY_LEN_EXPR:
    visit(resolver, node->as.array_length_expr.target);
    break;
  case AST_VAR_DECL:
    visit_type(node->as.variable_decl.declared_type, visit, resolver);
    visit(resolver, node->as.variable_decl.initializer);
    break;
  case AST_ASSIGN:
    visit(resolver, node->as.assignment.value);
    break;
  case AST_INDEX_ASSIGN:
    visit(resolver, node->as.index_assignment.target);
    visit(resolver, node->as.index_assignment.index);
    visit(resolver, node->as.index_assignment.value);
    break;
  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      visit(resolver, node->as.block.statements[i]);
    break;
  case AST_IF:
    visit(resolver, node->as.if_statement.condition);
    visit(resolver, node->as.if_statement.then_branch);
    visit(resolver, node->as.if_statement.else_branch);
    break;
  case AST_WHILE:
    visit(resolver, node->as.while_statement.condition);
    visit(resolver, node->as.while_statement.body);
    break;
  case AST_FOR:
    visit(resolver, node->as.for_statement.initializer);
    visit(resolver, node->as.for_statement.condition);
    visit(resolver, node->as.for_statement.increment);
    visit(resolver, node->as.for_statement.body);
    break;
  case AST_FOREACH:
    visit_type(node->as.foreach_statement.variable_type, visit, resolver);
    visit_type(node->as.foreach_statement.index_type, visit, resolver);
    visit(resolver, node->as.foreach_statement.array_expression);
    visit(resolver, node->as.foreach_statement.body);
    break;
  case AST_RETURN:
    visit(resolver, node->as.return_statement.expression);
    break;
  case AST_FUNC_DECL:
    /* Function bodies own a different name scope and are visited separately. */
    break;
  case AST_LABEL:
  case AST_GOTO:
    /* Names a point in the program, not a value. Nothing to resolve here;
     * resolve_labels below does that walk, because it needs to track which
     * foreach bodies a statement sits inside and this one does not. */
    break;
  default:
    break;
  }
}

static int scope_symbol(Resolver *resolver, const char *name, int name_length) {
  HDResolution *resolution = resolver->result;
  if (resolver->function_index != HD_NO_FUNCTION) {
    int local = find_symbol(resolution, name, name_length, HD_SYMBOL_LOCAL,
                            resolver->function_index);
    if (local != HD_NO_SYMBOL)
      return local;
  }
  return find_symbol(resolution, name, name_length, HD_SYMBOL_GLOBAL,
                     HD_NO_FUNCTION);
}

static void collect_explicit(Resolver *resolver, ASTNode *node) {
  if (!node || resolver->result->had_error)
    return;
  if (node->type == AST_VAR_DECL) {
    HDSymbolStorage storage = resolver->function_index == HD_NO_FUNCTION
                                  ? HD_SYMBOL_GLOBAL
                                  : HD_SYMBOL_LOCAL;
    add_symbol(resolver, node->as.variable_decl.name,
               node->as.variable_decl.name_length, storage, 0, node);
  } else if (node->type == AST_FOREACH) {
    HDSymbolStorage storage = resolver->function_index == HD_NO_FUNCTION
                                  ? HD_SYMBOL_GLOBAL
                                  : HD_SYMBOL_LOCAL;
    add_symbol(resolver, node->as.foreach_statement.variable_name,
               node->as.foreach_statement.variable_name_length, storage,
               HD_SYMBOL_FOREACH_VALUE, node);
    if (node->as.foreach_statement.index_name) {
      add_symbol(resolver, node->as.foreach_statement.index_name,
                 node->as.foreach_statement.index_name_length, storage,
                 HD_SYMBOL_FOREACH_INDEX, node);
    }
  }
  visit_children(node, collect_explicit, resolver);
}

static void collect_implicit(Resolver *resolver, ASTNode *node) {
  if (!node || resolver->result->had_error)
    return;
  if (node->type == AST_ASSIGN &&
      scope_symbol(resolver, node->as.assignment.name,
                   node->as.assignment.name_length) == HD_NO_SYMBOL) {
    HDSymbolStorage storage = resolver->function_index == HD_NO_FUNCTION
                                  ? HD_SYMBOL_GLOBAL
                                  : HD_SYMBOL_LOCAL;
    add_symbol(resolver, node->as.assignment.name,
               node->as.assignment.name_length, storage, HD_SYMBOL_IMPLICIT,
               node);
  }
  visit_children(node, collect_implicit, resolver);
}

/* One link per symbol, walked at run time until a slot is active. A local
 * defers to a same-named global and then to the runtime name; a global
 * defers to the runtime name, which is where the FFI constants live and
 * where an undefined-variable error comes from. Parameters are active from
 * entry, so they end the chain immediately.
 *
 * This runs after every symbol exists, and appends the external links it
 * needs as it goes, so it indexes rather than holding an HDSymbol pointer
 * across a call that can reallocate the table. */
static void link_fallbacks(Resolver *resolver) {
  HDResolution *resolution = resolver->result;
  for (int i = 0; i < resolution->symbol_count; i++) {
    HDSymbolStorage storage = resolution->symbols[i].storage;
    unsigned flags = resolution->symbols[i].flags;
    const char *name = resolution->symbols[i].name;
    int name_length = resolution->symbols[i].name_length;

    if (storage == HD_SYMBOL_EXTERNAL || (flags & HD_SYMBOL_PARAMETER))
      continue;

    int fallback = HD_NO_SYMBOL;
    if (storage == HD_SYMBOL_LOCAL) {
      fallback = find_symbol(resolution, name, name_length, HD_SYMBOL_GLOBAL,
                             HD_NO_FUNCTION);
    }
    if (fallback == HD_NO_SYMBOL)
      fallback = ensure_external(resolver, name, name_length);
    resolution->symbols[i].fallback_id = fallback;
  }
}

static int resolve_name(Resolver *resolver, const char *name,
                        int name_length) {
  int symbol_id = scope_symbol(resolver, name, name_length);
  if (symbol_id != HD_NO_SYMBOL)
    return symbol_id;
  return ensure_external(resolver, name, name_length);
}

static void resolve_node(Resolver *resolver, ASTNode *node) {
  if (!node || resolver->result->had_error)
    return;

  switch (node->type) {
  case AST_VAR_DECL: {
    int symbol = scope_symbol(resolver, node->as.variable_decl.name,
                              node->as.variable_decl.name_length);
    add_binding(resolver, node, HD_BINDING_DECLARATION, symbol);
    break;
  }
  case AST_VAR_REF: {
    int symbol = resolve_name(resolver, node->as.variable_ref.name,
                              node->as.variable_ref.name_length);
    add_binding(resolver, node, HD_BINDING_READ, symbol);
    break;
  }
  case AST_ASSIGN: {
    int symbol = resolve_name(resolver, node->as.assignment.name,
                              node->as.assignment.name_length);
    add_binding(resolver, node, HD_BINDING_WRITE, symbol);
    break;
  }
  case AST_FOREACH: {
    int value = scope_symbol(resolver, node->as.foreach_statement.variable_name,
                             node->as.foreach_statement.variable_name_length);
    add_binding(resolver, node, HD_BINDING_FOREACH_VALUE, value);
    if (node->as.foreach_statement.index_name) {
      int index = scope_symbol(resolver, node->as.foreach_statement.index_name,
                               node->as.foreach_statement.index_name_length);
      add_binding(resolver, node, HD_BINDING_FOREACH_INDEX, index);
    }
    break;
  }
  default:
    break;
  }

  visit_children(node, resolve_node, resolver);
}

/* Parameters come first in the frame, so a caller can fill them before the
 * body runs. They are also the one kind of local that is active on entry,
 * which is why they take no fallback. */
static void add_parameters(Resolver *resolver, ASTNode *function) {
  int count = function->as.function_decl.parameter_count;
  HDResolvedFunction *scope = &resolver->result->functions[resolver->function_index];

  if (count > 0) {
    scope->parameter_slots = (int *)malloc(sizeof(int) * (size_t)count);
    if (!scope->parameter_slots) {
      resolution_error(resolver->result,
                       "out of memory while recording parameter slots.");
      return;
    }
  }
  scope->parameter_count = count;

  for (int i = 0; i < count; i++) {
    ParameterSyntax *parameter = &function->as.function_decl.parameters[i];
    scope->parameter_slots[i] = -1;
    if (!parameter->name)
      continue;
    int symbol = add_symbol(resolver, parameter->name, parameter->name_length,
                            HD_SYMBOL_LOCAL, HD_SYMBOL_PARAMETER, parameter);
    if (symbol != HD_NO_SYMBOL)
      scope->parameter_slots[i] = resolver->result->symbols[symbol].slot;
  }
}

/* The foreach bodies a statement sits inside, innermost first. Built on the
 * C stack as the walk descends, so it costs nothing to carry. */
typedef struct ForeachFrame {
  const ASTNode *node;
  const struct ForeachFrame *parent;
} ForeachFrame;

static int foreach_encloses(const ForeachFrame *frame, const ASTNode *node) {
  for (; frame; frame = frame->parent) {
    if (frame->node == node)
      return 1;
  }
  return 0;
}

static int add_label(Resolver *resolver, const ASTNode *node,
                     const ForeachFrame *frame) {
  HDResolution *resolution = resolver->result;
  const char *name = node->as.label.name;
  int name_length = node->as.label.len;

  for (int i = 0; i < resolution->label_count; i++) {
    const HDLabel *label = &resolution->labels[i];
    if (label->function_index == resolver->function_index &&
        same_name(label->name, label->name_length, name, name_length)) {
      printf("Resolve error: label '%.*s' is declared twice in one function.\n",
             name_length, name);
      resolution->had_error = 1;
      return 0;
    }
  }

  if (resolution->label_count >= resolution->label_capacity &&
      !grow_array((void **)&resolution->labels, &resolution->label_capacity,
                  sizeof(HDLabel))) {
    resolution_error(resolution, "out of memory while recording labels.");
    return 0;
  }

  HDLabel *label = &resolution->labels[resolution->label_count++];
  label->node = node;
  label->enclosing_foreach = frame ? frame->node : NULL;
  label->function_index = resolver->function_index;
  label->name = name;
  label->name_length = name_length;
  return 1;
}

static int find_label(const HDResolution *resolution, int function_index,
                      const char *name, int name_length) {
  for (int i = 0; i < resolution->label_count; i++) {
    const HDLabel *label = &resolution->labels[i];
    if (label->function_index == function_index &&
        same_name(label->name, label->name_length, name, name_length))
      return i;
  }
  return -1;
}

static int add_goto(Resolver *resolver, const ASTNode *node, int label_index) {
  HDResolution *resolution = resolver->result;
  if (resolution->goto_count >= resolution->goto_capacity &&
      !grow_array((void **)&resolution->gotos, &resolution->goto_capacity,
                  sizeof(HDGotoTarget))) {
    resolution_error(resolution, "out of memory while recording gotos.");
    return 0;
  }
  HDGotoTarget *target = &resolution->gotos[resolution->goto_count++];
  target->node = node;
  target->label_index = label_index;
  return 1;
}

/* Labels and gotos are statements, so this walks statement structure only ,
 * no expression can contain either. `collect` distinguishes the two passes:
 * every label in the scope has to exist before any goto is bound, or a
 * forward jump would look undeclared. */
static void walk_labels(Resolver *resolver, ASTNode *node,
                        const ForeachFrame *frame, int collect) {
  if (!node || resolver->result->had_error)
    return;

  switch (node->type) {
  case AST_LABEL:
    if (collect)
      add_label(resolver, node, frame);
    break;

  case AST_GOTO: {
    if (collect)
      break;
    const char *name = node->as.goto_statement.target;
    int name_length = node->as.goto_statement.target_len;
    int index = find_label(resolver->result, resolver->function_index, name,
                           name_length);
    if (index < 0) {
      printf("Resolve error: goto names no label '%.*s' in this function.\n",
             name_length, name);
      resolver->result->had_error = 1;
      break;
    }
    const HDLabel *label = &resolver->result->labels[index];
    if (label->enclosing_foreach &&
        !foreach_encloses(frame, label->enclosing_foreach)) {
      printf("Resolve error: goto '%.*s' jumps into a foreach body, which "
             "would skip the loop counter it declares.\n",
             name_length, name);
      resolver->result->had_error = 1;
      break;
    }
    add_goto(resolver, node, index);
    break;
  }

  case AST_BLOCK:
    for (int i = 0; i < node->as.block.statement_count; i++)
      walk_labels(resolver, node->as.block.statements[i], frame, collect);
    break;

  case AST_IF:
    walk_labels(resolver, node->as.if_statement.then_branch, frame, collect);
    walk_labels(resolver, node->as.if_statement.else_branch, frame, collect);
    break;

  case AST_WHILE:
    walk_labels(resolver, node->as.while_statement.body, frame, collect);
    break;

  case AST_FOR:
    walk_labels(resolver, node->as.for_statement.initializer, frame, collect);
    walk_labels(resolver, node->as.for_statement.body, frame, collect);
    walk_labels(resolver, node->as.for_statement.increment, frame, collect);
    break;

  case AST_FOREACH: {
    ForeachFrame inner;
    inner.node = node;
    inner.parent = frame;
    walk_labels(resolver, node->as.foreach_statement.body, &inner, collect);
    break;
  }

  default:
    break;
  }
}

int HDResolveProgram(ASTNode *program, HDResolution *resolution) {
  HDResolutionInit(resolution);
  if (!program || program->type != AST_BLOCK) {
    resolution_error(resolution, "program root is not a block.");
    return 0;
  }

  Resolver resolver;
  resolver.result = resolution;
  resolver.function_index = HD_NO_FUNCTION;

  /* Register scopes first, then declarations. This makes source order
   * irrelevant to identity while execution still decides when a slot becomes
   * active. */
  for (int i = 0; i < program->as.block.statement_count; i++) {
    ASTNode *statement = program->as.block.statements[i];
    if (statement && statement->type == AST_FUNC_DECL)
      add_function(resolution, statement);
  }

  for (int i = 0; i < program->as.block.statement_count; i++) {
    ASTNode *statement = program->as.block.statements[i];
    if (!statement || statement->type == AST_FUNC_DECL)
      continue;
    collect_explicit(&resolver, statement);
  }
  for (int i = 0; i < program->as.block.statement_count; i++) {
    ASTNode *statement = program->as.block.statements[i];
    if (!statement || statement->type == AST_FUNC_DECL)
      continue;
    collect_implicit(&resolver, statement);
  }

  for (int i = 0; i < resolution->function_count; i++) {
    resolver.function_index = i;
    ASTNode *function = (ASTNode *)resolution->functions[i].declaration;
    add_parameters(&resolver, function);
    collect_explicit(&resolver, function->as.function_decl.body);
    collect_implicit(&resolver, function->as.function_decl.body);
  }

  resolver.function_index = HD_NO_FUNCTION;
  link_fallbacks(&resolver);

  for (int i = 0; i < program->as.block.statement_count; i++) {
    ASTNode *statement = program->as.block.statements[i];
    if (!statement || statement->type == AST_FUNC_DECL)
      continue;
    resolve_node(&resolver, statement);
  }
  for (int i = 0; i < resolution->function_count; i++) {
    resolver.function_index = i;
    ASTNode *function = (ASTNode *)resolution->functions[i].declaration;
    visit_type(function->as.function_decl.return_type, resolve_node,
               &resolver);
    for (int p = 0; p < function->as.function_decl.parameter_count; p++) {
      ParameterSyntax *parameter = &function->as.function_decl.parameters[p];
      visit_type(parameter->type, resolve_node, &resolver);
      resolve_node(&resolver, parameter->default_value);
    }
    resolve_node(&resolver, function->as.function_decl.body);
  }

  /* Labels last, and in two passes per scope, so a goto may name a label
   * declared after it. A label is scoped to the function that declares it;
   * the top level is a scope of its own, which is what stops a goto there
   * from naming a label inside a function. */
  for (int pass = 0; pass < 2; pass++) {
    resolver.function_index = HD_NO_FUNCTION;
    for (int i = 0; i < program->as.block.statement_count; i++) {
      ASTNode *statement = program->as.block.statements[i];
      if (!statement || statement->type == AST_FUNC_DECL)
        continue;
      walk_labels(&resolver, statement, NULL, pass == 0);
    }
    for (int i = 0; i < resolution->function_count; i++) {
      resolver.function_index = i;
      ASTNode *function = (ASTNode *)resolution->functions[i].declaration;
      walk_labels(&resolver, function->as.function_decl.body, NULL, pass == 0);
    }
  }

  build_binding_index(resolution);
  return !resolution->had_error;
}

/* Pointer-keyed, so a backend can ask about every name it emits without
 * rescanning the binding table. Losing the allocation is not fatal: the
 * lookup below falls back to the scan it replaces. */
static void build_binding_index(HDResolution *resolution) {
  int capacity = 16;
  while (capacity < resolution->binding_count * 2)
    capacity *= 2;

  HDBindingSlot *slots =
      (HDBindingSlot *)calloc((size_t)capacity, sizeof(HDBindingSlot));
  if (!slots)
    return;
  for (int i = 0; i < capacity; i++)
    slots[i].binding = -1;

  resolution->binding_index = slots;
  resolution->binding_index_mask = capacity - 1;

  /* First insertion wins. The parser reuses one subexpression node when it
   * desugars `arr[i] += v`, so a node can be visited twice; both visits are
   * in the same scope and agree, and keeping the first matches the scan. */
  for (int i = 0; i < resolution->binding_count; i++) {
    const HDBinding *binding = &resolution->bindings[i];
    unsigned h = binding_hash(binding->node, (int)binding->role) &
                 (unsigned)resolution->binding_index_mask;
    for (;;) {
      if (slots[h].binding < 0) {
        slots[h].node = binding->node;
        slots[h].role = (int)binding->role;
        slots[h].binding = i;
        break;
      }
      if (slots[h].node == binding->node &&
          slots[h].role == (int)binding->role)
        break;
      h = (h + 1) & (unsigned)resolution->binding_index_mask;
    }
  }
}

const HDSymbol *HDResolutionSymbol(const HDResolution *resolution,
                                   int symbol_id) {
  if (!resolution || symbol_id < 0 || symbol_id >= resolution->symbol_count)
    return NULL;
  return &resolution->symbols[symbol_id];
}

const HDBinding *HDResolutionBinding(const HDResolution *resolution,
                                     const ASTNode *node,
                                     HDBindingRole role) {
  if (!resolution)
    return NULL;

  if (resolution->binding_index) {
    unsigned mask = (unsigned)resolution->binding_index_mask;
    unsigned h = binding_hash(node, (int)role) & mask;
    for (;;) {
      const HDBindingSlot *slot = &resolution->binding_index[h];
      if (slot->binding < 0)
        return NULL;
      if (slot->node == node && slot->role == (int)role)
        return &resolution->bindings[slot->binding];
      h = (h + 1) & mask;
    }
  }

  for (int i = 0; i < resolution->binding_count; i++) {
    const HDBinding *binding = &resolution->bindings[i];
    if (binding->node == node && binding->role == role)
      return binding;
  }
  return NULL;
}

int HDResolutionFunctionIndex(const HDResolution *resolution,
                              const ASTNode *declaration) {
  if (!resolution)
    return HD_NO_FUNCTION;
  for (int i = 0; i < resolution->function_count; i++) {
    if (resolution->functions[i].declaration == declaration)
      return i;
  }
  return HD_NO_FUNCTION;
}

const HDSymbol *HDResolutionSlotSymbol(const HDResolution *resolution,
                                       int function_index, int slot) {
  if (!resolution || slot < 0)
    return NULL;
  HDSymbolStorage storage =
      function_index == HD_NO_FUNCTION ? HD_SYMBOL_GLOBAL : HD_SYMBOL_LOCAL;
  for (int i = 0; i < resolution->symbol_count; i++) {
    const HDSymbol *symbol = &resolution->symbols[i];
    if (symbol->storage == storage && symbol->function_index == function_index &&
        symbol->slot == slot) {
      return symbol;
    }
  }
  return NULL;
}

int HDResolutionLabelIndex(const HDResolution *resolution,
                           const ASTNode *label_node) {
  if (!resolution)
    return -1;
  for (int i = 0; i < resolution->label_count; i++) {
    if (resolution->labels[i].node == label_node)
      return i;
  }
  return -1;
}

int HDResolutionGotoLabel(const HDResolution *resolution,
                          const ASTNode *goto_node) {
  if (!resolution)
    return -1;
  for (int i = 0; i < resolution->goto_count; i++) {
    if (resolution->gotos[i].node == goto_node)
      return resolution->gotos[i].label_index;
  }
  return -1;
}

const HDLabel *HDResolutionLabel(const HDResolution *resolution,
                                 int label_index) {
  if (!resolution || label_index < 0 || label_index >= resolution->label_count)
    return NULL;
  return &resolution->labels[label_index];
}

int HDResolutionSymbolIsRead(const HDResolution *resolution, int symbol_id) {
  if (!resolution)
    return 0;
  for (int i = 0; i < resolution->binding_count; i++) {
    const HDBinding *binding = &resolution->bindings[i];
    if (binding->symbol_id == symbol_id && binding->role == HD_BINDING_READ)
      return 1;
  }
  return 0;
}

static const char *storage_name(HDSymbolStorage storage) {
  switch (storage) {
  case HD_SYMBOL_GLOBAL:
    return "global";
  case HD_SYMBOL_LOCAL:
    return "local";
  case HD_SYMBOL_EXTERNAL:
    return "external";
  }
  return "unknown";
}

static const char *role_name(HDBindingRole role) {
  switch (role) {
  case HD_BINDING_DECLARATION:
    return "declare";
  case HD_BINDING_READ:
    return "read";
  case HD_BINDING_WRITE:
    return "write";
  case HD_BINDING_FOREACH_VALUE:
    return "foreach-value";
  case HD_BINDING_FOREACH_INDEX:
    return "foreach-index";
  }
  return "unknown";
}

void HDResolutionPrint(const HDResolution *resolution) {
  printf("Resolution: %d global slots, %d function scopes\n",
         resolution->global_slot_count, resolution->function_count);
  for (int i = 0; i < resolution->function_count; i++) {
    const ASTNode *function = resolution->functions[i].declaration;
    printf("  function %d '%.*s': %d frame slots\n", i,
           function->as.function_decl.name_length,
           function->as.function_decl.name,
           resolution->functions[i].frame_slot_count);
  }

  printf("Symbols:\n");
  for (int i = 0; i < resolution->symbol_count; i++) {
    const HDSymbol *symbol = &resolution->symbols[i];
    printf("  #%d %-8s", symbol->id, storage_name(symbol->storage));
    if (symbol->storage == HD_SYMBOL_LOCAL)
      printf(" fn=%d slot=%d", symbol->function_index, symbol->slot);
    else if (symbol->storage == HD_SYMBOL_GLOBAL)
      printf(" slot=%d", symbol->slot);
    else
      printf("       ");
    printf(" '%.*s'", symbol->name_length, symbol->name);
    if (symbol->flags & HD_SYMBOL_PARAMETER)
      printf(" parameter");
    if (symbol->flags & HD_SYMBOL_FOREACH_VALUE)
      printf(" foreach-value");
    if (symbol->flags & HD_SYMBOL_FOREACH_INDEX)
      printf(" foreach-index");
    if (symbol->flags & HD_SYMBOL_IMPLICIT)
      printf(" implicit");
    if (symbol->fallback_id != HD_NO_SYMBOL)
      printf(" fallback #%d", symbol->fallback_id);
    printf("\n");
  }

  if (resolution->label_count > 0) {
    printf("Labels:\n");
    for (int i = 0; i < resolution->label_count; i++) {
      const HDLabel *label = &resolution->labels[i];
      printf("  #%d '%.*s'", i, label->name_length, label->name);
      if (label->function_index == HD_NO_FUNCTION)
        printf(" top-level");
      else
        printf(" fn=%d", label->function_index);
      if (label->enclosing_foreach)
        printf(" inside a foreach");
      printf("\n");
    }
    for (int i = 0; i < resolution->goto_count; i++) {
      printf("  goto -> #%d\n", resolution->gotos[i].label_index);
    }
  }

  printf("Bindings:\n");
  for (int i = 0; i < resolution->binding_count; i++) {
    const HDBinding *binding = &resolution->bindings[i];
    const HDSymbol *symbol =
        HDResolutionSymbol(resolution, binding->symbol_id);
    printf("  %-13s '%.*s' -> #%d", role_name(binding->role),
           symbol->name_length, symbol->name, binding->symbol_id);
    printf("\n");
  }
}
