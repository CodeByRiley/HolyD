#ifndef HOLYD_EMIT_ASM_H
#define HOLYD_EMIT_ASM_H

#include "ast/ast.h"
#include "resolve.h"
#include <stdio.h>

/* Writes `ast` to `out` as x86-64 assembly in GNU assembler syntax, for the
 * Win64 calling convention.
 *
 * This is the same translation src/emit_c.c performs, one level lower: the
 * same runtime, the same ~20 entry points, the same fallback chain for a
 * name whose declaration has not run yet. What it does not go through is a C
 * compiler. That is the whole point of it , it is the step that proves the
 * instruction selection and the ABI without also requiring an object-file
 * writer, so `--emit-exe` later becomes an encoder and a PE writer bolted to
 * a code generator that already works.
 *
 * It is not faster than the C backend and is not meant to be. Every value is
 * still a boxed 56-byte HDValue and every operator is still a call into the
 * runtime, so an add costs an add's worth of argument marshalling either
 * way; gcc -O2 will beat this comfortably. Speed is what a type system buys,
 * not what dropping the C compiler buys , docs/roadmap.md section 10.
 *
 * `source_name` only labels the generated file. Returns 0 on failure, having
 * printed why.
 */
int HDEmitAsm(ASTNode *ast, const HDResolution *resolution, FILE *out,
              const char *source_name);

#endif
