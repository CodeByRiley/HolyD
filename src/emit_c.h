#ifndef HOLYD_EMIT_C_H
#define HOLYD_EMIT_C_H

#include "ast/ast.h"
#include "resolve.h"
#include <stdio.h>

/* Writes `ast` to `out` as a C translation unit, using `resolution` to give
 * every name a C object.
 *
 * The C it writes is a transliteration of what the VM does, not an
 * optimisation of it: control flow becomes real C control flow, calls become
 * direct C calls, and each resolved name becomes a C local or a file-scope
 * static. Values stay boxed HDValues. That is deliberate for a first
 * backend , every semantic the VM has comes along for free, so the two paths
 * can be diffed against each other (tools/difftest.sh).
 *
 * The runtime Environment survives only as the place the FFI constants live
 * and as the last link in a name's fallback chain, which is what lets a
 * declaration take effect where it is written rather than at the top of its
 * frame.
 *
 * Making it fast is a later, separate change: give the language a real type
 * system, then the same emitter can put out `long long` locals instead of
 * HDValue.
 *
 * `source_name` only labels the generated file. Returns 0 on failure,
 * having printed why.
 */
int HDEmitC(ASTNode *ast, const HDResolution *resolution, FILE *out,
            const char *source_name);

#endif
