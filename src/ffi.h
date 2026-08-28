#ifndef FFI_H
#define FFI_H

#include "compiler.h" // Gets HDValue from here

// Define the function pointer type
typedef HDValue (*NativeFn)(int arg_count, HDValue* args);

// Expose a function that compiler.c can call to find a native function
NativeFn ffi_lookup_native(const char* name, int len);

/* Define the names a native needs but the language cannot spell: event
 * codes, keycodes, prompt kinds, colours. HolyD has no enums and no
 * preprocessor, so they land in the global environment as plain I64s
 * before the program runs. Call once, on a freshly initialised env. */
void ffi_define_globals(Environment* env);

#endif
