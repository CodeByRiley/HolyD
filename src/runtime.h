#ifndef HOLYD_RUNTIME_H
#define HOLYD_RUNTIME_H

/* The semantics of a HolyD value, in one place.
 *
 * Both execution paths live on top of this file: the bytecode VM in
 * compiler.c, and the C that emit_c.c writes. Keeping the operators here
 * rather than inline in the VM is what lets `tools/difftest.sh` mean
 * something , the two paths cannot drift apart on arithmetic or printing
 * because there is only one copy of each.
 *
 * Two spellings of most operations:
 *
 *   HDAdd..., returning int    the VM's form. 0 means the error was already
 *                             printed and the caller should unwind.
 *   ...X, returning HDValue    the transpiler's form. Aborts on the same
 *                             error instead of unwinding, which is what the
 *                             VM's unwind amounts to anyway , run_chunk
 *                             propagates 0 to main, which exits 1 without
 *                             printing anything further.
 *
 * So both paths produce identical stdout and identical exit codes.
 */

#include "eval.h" /* HDValue, Environment */

typedef enum {
  HD_ADD,
  HD_SUB,
  HD_MUL,
  HD_DIV,
  HD_MOD,
  HD_POW,
  HD_EQ,
  HD_NE,
  HD_LT,
  HD_GT,
  HD_LE,
  HD_GE,
  HD_BAND,
  HD_BOR,
  HD_BXOR,
  HD_SHL,
  HD_SHR,
  HD_USHR,
  HD_CONCAT
} HDBinOp;

/* The first cast slice deliberately follows the runtime's actual value
 * model: integers, doubles, and truth values. Source-width integer names
 * share the signed 64-bit storage used everywhere else today. */
typedef enum {
  HD_CAST_INT,
  HD_CAST_FLOAT,
  HD_CAST_BOOL
} HDCastKind;

/* Converts a parsed scalar type spelling into the compact runtime dispatch
 * kind. Keeping this here avoids four backends independently deciding what
 * `cast(long)` or `cast(F64)` means. */
int HDCastKindFromTypeSyntax(const TypeSyntax *type, HDCastKind *kind);

/* ---- Constructors. Declared in compiler.h too, for existing callers. ---- */
HDValue int_value(long long v);
HDValue string_value(const char *s, int len);
HDValue float_value(double v);

/* ---- Inspection ---- */
int HDIsNumeric(HDValue value);
double HDAsDouble(HDValue value);
int HDTruthy(HDValue value);

/* ---- Printing. HDPrintDouble is also declared in eval.h. ---- */
void HDPrintDouble(double value);
void HDPrintValue(HDValue value);
/* argc values, then a newline when add_newline. Always yields int 0, the
 * value a print call leaves on the VM's stack. */
HDValue HDPrintN(int argc, HDValue *args, int add_newline);

/* ---- Operators ---- */
int HDBinary(HDBinOp op, HDValue left, HDValue right, HDValue *out);
/* Logical negation. Defined over every value, not just numbers, because it
 * asks HDTruthy rather than comparing against zero , so !"" is 1 and ![]
 * is 1, where `== 0` would have been a type error. */
HDValue HDNot(HDValue value);
int HDConcat(HDValue left, HDValue right, HDValue *out);
int HDArrayNew(int count, HDValue *elements, HDValue *out);
int HDIndex(HDValue array, HDValue index, HDValue *out);
int HDIndexSet(HDValue array, HDValue index, HDValue value);
int HDLength(HDValue value, HDValue *out);
int HDCast(HDCastKind kind, HDValue value, HDValue *out);

/* ---- Aborting forms, for generated code ---- */
HDValue HDBinaryX(HDBinOp op, HDValue left, HDValue right);
HDValue HDArrayNewX(int count, HDValue *elements);
HDValue HDIndexX(HDValue array, HDValue index);
void HDIndexSetX(HDValue array, HDValue index, HDValue value);
HDValue HDLengthX(HDValue value);
HDValue HDCastX(HDCastKind kind, HDValue value);
/* Reads a variable, aborting with the VM's message when it is undefined. */
HDValue HDLoadX(Environment *env, const char *name, int len);
/* Calls a native by name, aborting when no native has that name. Generated
 * code only emits this once the name has resolved at transpile time, so the
 * abort is a should-not-happen guard. */
HDValue HDNativeX(const char *name, int len, int argc, HDValue *args);
/* The VM's "unknown function" error, for a callee that resolved to nothing.
 * Emitted at the call site so a script that never reaches it still runs. */
HDValue HDUnknownFunctionX(const char *name, int len);
/* Likewise for a call whose argument count does not match the declaration.
 * The transpiler can see this statically, but the VM only complains when
 * the call runs, so it is reported from the call site to match. */
HDValue HDArityX(const char *name, int len, int expects, int got);

/* Prints `message` and exits 1, matching what the VM's unwind produces. */
void HDAbort(const char *message);

#endif
