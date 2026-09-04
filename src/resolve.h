#ifndef HOLYD_RESOLVE_H
#define HOLYD_RESOLVE_H

#include "ast/ast.h"

/* Name resolution is deliberately separate from the AST. The parser records
 * source structure; this side table records what each name means. Keeping the
 * two apart also lets later compiler passes be rebuilt without teaching every
 * AST constructor about semantic state. */

#define HD_NO_SYMBOL (-1)

typedef enum {
  HD_SYMBOL_GLOBAL,
  HD_SYMBOL_LOCAL,
  HD_SYMBOL_EXTERNAL
} HDSymbolStorage;

typedef enum {
  HD_SYMBOL_PARAMETER = 1 << 0,
  HD_SYMBOL_FOREACH_VALUE = 1 << 1,
  HD_SYMBOL_FOREACH_INDEX = 1 << 2,
  HD_SYMBOL_IMPLICIT = 1 << 3
} HDSymbolFlags;

typedef struct {
  int id;                 /* Stable across the whole resolved program. */
  int slot;               /* Index in the global or function frame. */
  int function_index;     /* -1 for globals and external runtime names. */
  HDSymbolStorage storage;
  unsigned flags;
  const char *name;
  int name_length;
  const void *declaration;

  /* A declaration is not active until execution reaches it. Until then a
   * name means whatever the enclosing scope means by it, so every symbol
   * carries the next link in that search: a local falls back to a global of
   * the same name and then to the runtime-provided name, a global falls back
   * to the runtime-provided name, and both end at HD_NO_SYMBOL. Parameters
   * are active from entry and have no fallback. Following this chain is what
   * lets slots be introduced without hoisting declarations. */
  int fallback_id;
} HDSymbol;

typedef enum {
  HD_BINDING_DECLARATION,
  HD_BINDING_READ,
  HD_BINDING_WRITE,
  HD_BINDING_FOREACH_VALUE,
  HD_BINDING_FOREACH_INDEX
} HDBindingRole;

typedef struct {
  const ASTNode *node;
  HDBindingRole role;
  int symbol_id;
} HDBinding;

typedef struct {
  const ASTNode *declaration;
  int frame_slot_count;

  /* Where each parameter lands in the frame. Usually 0..parameter_count-1,
   * since parameters are registered before anything else in the scope, but
   * two parameters of the same name resolve to one symbol and so share a
   * slot. Recording the mapping keeps a caller from assuming otherwise. */
  int *parameter_slots;
  int parameter_count;
} HDResolvedFunction;

/* A label names a point in its function. The resolver owns which point,
 * because both backends need the same answer and because the one rule that
 * is easy to get wrong , where a goto may land , should have one
 * implementation rather than two.
 *
 * `enclosing_foreach` is the innermost foreach body the label sits in, or
 * NULL. A goto may only target such a label from inside that same foreach:
 * the lowering of foreach declares a loop counter on entry, and jumping past
 * that declaration would leave the loop reading a counter that was never
 * set. Jumping *out* of a foreach is fine, and so is jumping about within
 * one. Nothing else in the language declares anything a jump could skip:
 * every variable is a frame slot, declared once for the whole frame and
 * carrying its own bound bit. */
typedef struct {
  const ASTNode *node;
  const ASTNode *enclosing_foreach;
  int function_index; /* -1 at the top level. */
  const char *name;
  int name_length;
} HDLabel;

typedef struct {
  const ASTNode *node; /* the AST_GOTO */
  int label_index;
} HDGotoTarget;

/* Open-addressed (node, role) -> binding index, so a backend can ask about
 * every name it emits without rescanning the table each time. */
typedef struct {
  const ASTNode *node;
  int role;
  int binding;
} HDBindingSlot;

typedef struct {
  HDSymbol *symbols;
  int symbol_count;
  int symbol_capacity;

  HDBinding *bindings;
  int binding_count;
  int binding_capacity;

  HDResolvedFunction *functions;
  int function_count;
  int function_capacity;

  HDBindingSlot *binding_index;
  int binding_index_mask;

  HDLabel *labels;
  int label_count;
  int label_capacity;

  HDGotoTarget *gotos;
  int goto_count;
  int goto_capacity;

  int global_slot_count;
  int had_error;
} HDResolution;

void HDResolutionInit(HDResolution *resolution);
void HDResolutionFree(HDResolution *resolution);
int HDResolveProgram(ASTNode *program, HDResolution *resolution);

const HDSymbol *HDResolutionSymbol(const HDResolution *resolution,
                                   int symbol_id);
const HDBinding *HDResolutionBinding(const HDResolution *resolution,
                                     const ASTNode *node,
                                     HDBindingRole role);

/* The function scope `node` declares, or -1 when it is not a function the
 * resolver gave a frame to. */
int HDResolutionFunctionIndex(const HDResolution *resolution,
                              const ASTNode *declaration);

/* What occupies `slot` in a frame: pass -1 for the globals. NULL when the
 * slot is unused, which a frame can have when two parameters share a name. */
const HDSymbol *HDResolutionSlotSymbol(const HDResolution *resolution,
                                       int function_index, int slot);

/* Whether any reference reads the symbol. A slot that is only ever written
 * is still a slot, but generated C wants to say so out loud rather than
 * draw an unused-variable warning. */
int HDResolutionSymbolIsRead(const HDResolution *resolution, int symbol_id);

/* Labels, by the AST node that declares one and by the goto that names one.
 * Both return an index into the label table, or -1. A resolved program has
 * an index for every goto in it, so a backend can emit the jump without
 * repeating the lookup or the diagnostics. */
int HDResolutionLabelIndex(const HDResolution *resolution,
                           const ASTNode *label_node);
int HDResolutionGotoLabel(const HDResolution *resolution,
                          const ASTNode *goto_node);
const HDLabel *HDResolutionLabel(const HDResolution *resolution,
                                 int label_index);

/* Deterministic diagnostics used by --dump-symbols and resolver tests. */
void HDResolutionPrint(const HDResolution *resolution);

#endif
