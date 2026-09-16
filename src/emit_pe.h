#ifndef HOLYD_EMIT_PE_H
#define HOLYD_EMIT_PE_H

#include "ast/ast.h"
#include <stdio.h>

/* Write a Windows x86-64 PE executable without invoking an assembler, linker,
 * C compiler, or C runtime. This intentionally starts as a narrow vertical
 * slice: a parameterless main/Main may return a constant integer expression.
 * Later milestones widen the language and replace the tiny ExitProcess-only
 * runtime with HolyD's native runtime. */
int HDEmitPE(ASTNode *ast, FILE *out, const char *source_name);

#endif
