#ifndef HOLYD_EMIT_C_H
#define HOLYD_EMIT_C_H

#include "ast/ast.h"
#include <stdio.h>

/* Writes `ast` to `out` as a C translation unit.
 *
 * The C it writes is a transliteration of what the VM does, not an
 * optimisation of it: control flow becomes real C control flow and calls
 * become direct C calls, but values stay boxed HDValues and variables stay
 * in the runtime Environment. That is deliberate for a first backend ,
 * every semantic the VM has comes along for free, so the two paths can be
 * diffed against each other (tools/difftest.sh).
 *
 * Making it fast is a later, separate change: resolve names to frame slots
 * and give the language a real type system, then the same emitter can put
 * out `long long` locals instead of HDValue.
 *
 * `source_name` only labels the generated file. Returns 0 on failure,
 * having printed why.
 */
int HDEmitC(ASTNode *ast, FILE *out, const char *source_name);

#endif
