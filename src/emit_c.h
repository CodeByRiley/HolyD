#ifndef HOLYD_EMIT_C_H
#define HOLYD_EMIT_C_H

#include "ast/ast.h"
#include "resolve.h"
#include "typecheck.h"
#include <stdio.h>

/* Writes `ast` to `out` as a C translation unit, using `resolution` to give
 * every name a C object.
 *
 * Control flow becomes real C control flow and calls become direct C calls.
 * Statically stable numeric slots and expressions use native long long or
 * double; values whose runtime representation can still vary remain boxed
 * HDValues. Boundary boxing keeps the optimized path comparable with the VM
 * through tools/difftest.sh.
 *
 * The runtime Environment survives only as the place the FFI constants live
 * and as the last link in a name's fallback chain, which is what lets a
 * declaration take effect where it is written rather than at the top of its
 * frame.
 *
 * `source_name` only labels the generated file. Returns 0 on failure,
 * having printed why.
 */
int HDEmitC(ASTNode *ast, const HDResolution *resolution,
            const HDTypeCheck *types, FILE *out, const char *source_name);

#endif
