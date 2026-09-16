/* Holy D Compiler
 * A recreation of Terry A. Davis' (Rest in Peace) Holy C compiler.
 * For the D programming language.
 */

#include "compiler.h"
#include "emit_c.h"
#include "emit_pe.h"
#include "eval.h"
#include "ffi.h"
#include "parser/parser.h"
#include "emit_asm.h"
#include "resolve.h"
#include "typecheck.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#endif

/* Listing a directory is the one thing this file needs that is not standard
 * C. TOS answers it with a syscall that packs several names per call;
 * everywhere else has dirent. See get_test_files. */
#ifdef _WIN32
#include <dirent.h>
#else
#include <lib/syscall.h>
#endif

#define BUF_SIZE 1024

/* Where --test looks when it is not told. The TOS image keeps the scripts
 * under holyd/tests; the standalone repository has them at tests/. */
#ifndef HOLYD_DEFAULT_TEST_DIR
#define HOLYD_DEFAULT_TEST_DIR "holyd/tests"
#endif

/* FLAGS

        This block is the design intent for the full hcc-style CLI. What is
  actually wired up is whatever print_usage() lists; every other flag below
  is rejected with the reason it cannot work yet. Keep the two in sync as
  flags land.

        Immediate Your File: -run
        Immediately run your code. This is smoke and mirrors, under the hood it
  will create the assembly file, use gcc to assemble it and then run the file.
  So is more of a convenience.

  ----------------------------------
        Create Control Flow Graph: -cfg
        Creates control flow graph of your code as a .dot file that can be used
  by graphviz to create a graphical representation of the flow of your program.

  ----------------------------------

        -cfg-png
        Immediately creates a .png of your program using graphviz. You must have
  graphviz installed for this to work correctly.

  ----------------------------------

        -cfg-svg
        Immediately creates a .svg of your program using graphviz. You must have
  graphviz installed for this to work correctly.

  ----------------------------------

        Print tokens: -tokens
        Prints all of the tokens from your program to stdout

  ----------------------------------


        Create Assembly File: -S
        Create assembly code and write to a file.

  ----------------------------------

        If you are wanting to then compile the assembly use the following:

        gcc -lsqlite3 -ltos -I/usr/local/include -L/usr/local/lib ./<file>.s

        Presently the libraries are created as a dynamic library which are then
  linked at compile time.

        Create Object File: -obj
        Creates an object file.

  ----------------------------------


        Create Library: -lib
        Creates a dynamic library and shared object file from your code,
  treating it as position independent. You cannot have a Main or main function
  nor can you have any top level executing code in your file.

  ----------------------------------

        Link C-Libraries: -clibs
        Links in c libraries, you have to declare the function prototypes
  manually in your code. Note that there are no F32's yet so libraries requiring
  float will need you to figure out a workaround.

  ----------------------------------

        Rename Binary: -o
        Change the output name of your executable from the default a.out

  ----------------------------------

        Define Preprocessor Variable: -D<var>
        Define a #define, this does not yet accept a value.

  ----------------------------------

        Print Help: --help
        Display all the above options with a small description
 */

int check_ext(const char *filename) {
  const char *ext = strrchr(filename, '.');
  return ext && strcmp(ext, ".hd") == 0;
}

/* Class declarations are presently lowered by the C backend.  Keep the
 * bytecode, interpreter, assembly and tiny direct-PE backends from treating
 * a class as an invisible no-op while those backends gain object support. */
static int program_has_classes(const ASTNode *program) {
  if (!program || program->type != AST_BLOCK)
    return 0;
  for (int i = 0; i < program->as.block.statement_count; i++) {
    const ASTNode *statement = program->as.block.statements[i];
    if (statement && statement->type == AST_CLASS_DECL)
      return 1;
  }
  return 0;
}

char *read_file(const char *filename) {
  FILE *file =
      fopen(filename, "rb");
  if (file == NULL) {
    return NULL;
  }

  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (size < 0) {
    fclose(file);
    return NULL;
  }

  char *content = malloc(size + 1);
  if (content == NULL) {
    fclose(file);
    return NULL;
  }

  size_t bytes_read = fread(content, 1, size, file);
  content[bytes_read] = '\0'; // Null-terminate at actual read position

  fclose(file);
  return content;
}

/* A growable NUL-separated list of names, terminated by an empty one. */
struct name_list {
  char *buf;
  size_t len;
  size_t capacity;
};

/* Append `name` if it is a .hd file. Returns 0 only on allocation failure ,
 * a name that is not ours is a success with nothing added. */
static int name_list_add(struct name_list *list, const char *name) {
  size_t len = strlen(name);
  if (len < 3 || strcmp(name + len - 3, ".hd") != 0)
    return 1;

  if (list->len + len + 2 > list->capacity) {
    size_t capacity = list->capacity ? list->capacity : 4096;
    while (list->len + len + 2 > capacity)
      capacity *= 2;

    char *grown = realloc(list->buf, capacity);
    if (!grown)
      return 0;
    list->buf = grown;
    list->capacity = capacity;
  }

  memcpy(list->buf + list->len, name, len + 1);
  list->len += len + 1;
  return 1;
}

// Returns a packed buffer of filenames (each null-terminated, double-null at
// end). Caller must free() this buffer when done.
char *get_test_files(const char *dir) {
  struct name_list list;
  list.capacity = 4096;
  list.len = 0;
  list.buf = malloc(list.capacity);
  if (!list.buf)
    return NULL;

#ifdef _WIN32
  /* dirent hands back one name per call. */
  DIR *handle = opendir(dir);
  if (handle != NULL) {
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
      if (!name_list_add(&list, entry->d_name)) {
        closedir(handle);
        free(list.buf);
        return NULL;
      }
    }
    closedir(handle);
  }
#else
  /* readdir_path packs as many names as fit into one buffer per call, so
   * there is an inner walk over that buffer as well as the outer loop. */
  unsigned index = 0;
  char buf[1024];
  long bytes_read;

  while ((bytes_read = readdir_path(dir, &index, buf, sizeof(buf) - 1)) > 0) {
    buf[bytes_read] = '\0'; // Null-terminate what we just read

    for (long i = 0; i < bytes_read;) {
      if (buf[i] == '\0') {
        i++;
        continue;
      }

      char *filename = &buf[i];
      size_t len = strlen(filename);

      if (!name_list_add(&list, filename)) {
        free(list.buf);
        return NULL;
      }

      i += (long)len; // Skip to the end of the current filename
    }
  }
#endif

  // Double null-terminate the end of the buffer so the consumer knows to stop
  if (list.len + 1 > list.capacity) {
    char *grown = realloc(list.buf, list.len + 1);
    if (!grown) {
      free(list.buf);
      return NULL;
    }
    list.buf = grown;
  }
  list.buf[list.len] = '\0';

  return list.buf;
}

/* Run every .hd in `dir`. The directory is a parameter because the tree the
 * scripts ship in differs per host , holyd/tests on the TOS image, tests/ in
 * the standalone repository , and hardcoding one made the other unusable. */
int test(const char *dir) {
  int success = 0, failure = 0;

  /* NUL-separated .hd paths. */
  char *files = get_test_files(dir);
  if (files == NULL || files[0] == '\0') {
    printf("holyd: no .hd test files found in %s/\n", dir);
    if (files)
      free(files);
    return 1;
  }

  char *current = files;
  while (*current != '\0') {
    // Construct the full path, e.g. "holyd/tests/filename.hd"
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, current);

    printf("holyd: running %s... ", full_path);
    fflush(stdout);

    // Use full_path instead of current
    char *source = read_file(full_path);
    if (source == NULL) {
      printf("holyd: failed to read %s\n", full_path);
      failure++;
      current += strlen(current) + 1; // Move to next string
      continue;
    }

    Parser parser;
    ParserInit(&parser, source);
    ASTNode *program = ParseProgram(&parser);

    if (program == NULL) {
      printf("holyd: failed to parse %s\n", full_path);
      failure++;
    } else {
      HDResolution resolution;
      HDTypeCheck types;
      HDProgram bytecode;
      HDTypeCheckInit(&types);
      HDProgramInit(&bytecode);

      if (!HDResolveProgram(program, &resolution)) {
        printf("holyd: failed to resolve %s\n", full_path);
        failure++;
      } else if (!HDTypeCheckProgram(program, &resolution, &types)) {
        printf("holyd: failed to type-check %s\n", full_path);
        failure++;
      } else if (!HDCompileProgram(program, &resolution, &bytecode)) {
        printf("holyd: failed to compile %s\n", full_path);
        failure++;
      } else if (!HDRunProgram(&bytecode)) {
        printf("holyd: failed to run %s\n", full_path);
        failure++;
      } else {
        printf("holyd: passed %s\n", full_path);
        success++;
      }
      HDTypeCheckFree(&types);
      HDResolutionFree(&resolution);
    }

    free(source);
    current += strlen(current) + 1; // Move to next string
  }

  free(files);

  printf("\n--- Results ---\n");
  printf("holyd: success: %d, Failure: %d\n", success, failure);
  return failure > 0 ? 1 : 0;
}

static void print_usage(void) {
  printf("Usage: holyd [options] <source.hd>\n");
  printf("       holyd --test [dir]\n");
  printf("\nImplemented:\n");
  printf("  %-16s %s\n", "--help", "Show this list and exit");
  printf("  %-16s %s\n", "--test [dir]",
         "Run every .hd file under dir (default "
         HOLYD_DEFAULT_TEST_DIR "/)");
  printf("  %-16s %s\n", "-tokens", "Print the token stream and exit");
  printf("  %-16s %s\n", "-ast", "Print the parsed AST and exit");
  printf("  %-16s %s\n", "--dump-symbols",
         "Print resolved names and frame slots, then exit");
  printf("  %-16s %s\n", "--dump-types",
         "Print inferred semantic types, then exit");
  printf("  %-16s %s\n", "--emit-asm",
         "Translate to x86-64 assembly instead of running it");
  printf("  %-16s %s\n", "--interpret", "Walk the AST instead of running bytecode");
  printf("  %-16s %s\n", "--dump-bytecode", "Disassemble the program before running it");
  printf("  %-16s %s\n", "--emit-c [-o]", "Write runnable C source and exit");
  printf("  %-16s %s\n", "--emit-exe [-o]",
         "Build a runnable Windows executable through the assembly backend");
  printf("  %-16s %s\n", "--emit-pe [-o]",
         "Write a direct PE executable (no assembler, linker, or C runtime)");
  printf("  %-16s %s\n", "-run",
         "Build and run a direct PE executable without a host toolchain");
  printf("\nNot implemented yet:\n");
  printf("  %-16s %s\n", "-S -obj", "need a native code backend");
  printf("  %-16s %s\n", "-lib -clibs -o", "need a native code backend");
  printf("  %-16s %s\n", "-D<var>", "needs a preprocessor");
  printf("  %-16s %s\n", "-cfg", "control flow graphs are not built yet");
  printf("  %-16s %s\n", "-cfg-png", "not built, and needs graphviz on the host");
  printf("  %-16s %s\n", "-cfg-svg", "not built, and needs graphviz on the host");
}

// Why a documented flag still cannot run. NULL means we do not know the flag
// at all, which is a different message.
static const char *unimplemented_reason(const char *arg) {
  static const char *needs_backend[] = {"-S", "-obj", "-lib",
                                        "-clibs"};
  for (unsigned i = 0; i < sizeof(needs_backend) / sizeof(needs_backend[0]); i++) {
    if (strcmp(arg, needs_backend[i]) == 0) {
      return "it needs a native code backend";
    }
  }

  if (strncmp(arg, "-D", 2) == 0) {
    return "it needs a preprocessor";
  }
  if (strcmp(arg, "-cfg") == 0) {
    return "control flow graphs are not built yet";
  }
  if (strcmp(arg, "-cfg-png") == 0 || strcmp(arg, "-cfg-svg") == 0) {
    return "control flow graphs are not built yet, and these also need graphviz";
  }
  return NULL;
}

/* The native emitter already handles instruction selection and the Win64
 * calling convention. This final host-toolchain step stays here because
 * assembling and linking are host concerns, not AST emission. _spawnvp uses
 * an argv array, so paths containing spaces cannot become compiler options. */
static int assemble_and_link(const char *assembly_path, const char *output_path) {
#ifdef _WIN32
  const char *compiler = getenv("HOLYD_CC");
  if (compiler == NULL || compiler[0] == '\0')
    compiler = "cc";

  const char *arguments[] = {
      compiler,
      "-std=gnu11", "-O2", "-I", "src", assembly_path,
      "src/runtime.c", "src/eval.c", "src/ffi.c", "src/ffi_win32.c",
      "src/platform/standalone/gfx.c", "src/platform/standalone/bmp.c",
      "-o", output_path, "-lgdi32", "-luser32", "-lws2_32", NULL};
  intptr_t status = _spawnvp(_P_WAIT, compiler, arguments);
  if (status == -1) {
    printf("holyd: could not start '%s' (set HOLYD_CC to choose a C toolchain).\n",
           compiler);
    return 0;
  }
  if (status != 0) {
    printf("holyd: assembler/linker failed while creating '%s'.\n", output_path);
    return 0;
  }
  return 1;
#else
  (void)assembly_path;
  (void)output_path;
  printf("holyd: --emit-exe is currently implemented for the Windows host build.\n");
  return 0;
#endif
}

/* The compiler refuses to reuse an existing temporary, so cleanup can only
 * remove a file created by this invocation. */
static char *temporary_assembly_name(const char *output_path) {
  size_t length = strlen(output_path);
  const char suffix[] = ".holyd.tmp.s";
  char *path = malloc(length + sizeof(suffix));
  if (!path)
    return NULL;
  memcpy(path, output_path, length);
  memcpy(path + length, suffix, sizeof(suffix));
  return path;
}

/* -run owns this output and deletes it after the child exits. Refusing an
 * existing path keeps cleanup confined to an artifact this invocation made. */
static char *temporary_run_name(const char *source_path) {
  size_t length = strlen(source_path);
  const char suffix[] = ".holyd-run.exe";
  char *path = malloc(length + sizeof(suffix));
  if (!path)
    return NULL;
  memcpy(path, source_path, length);
  memcpy(path + length, suffix, sizeof(suffix));
  return path;
}

static int emit_and_run_pe(ASTNode *program, const char *source_path,
                           const char *executable_path, int *exit_code) {
#ifdef _WIN32
  FILE *existing = fopen(executable_path, "rb");
  if (existing != NULL) {
    fclose(existing);
    printf("Error: run executable path '%s' already exists; refusing to replace it.\n",
           executable_path);
    return 0;
  }

  FILE *out = fopen(executable_path, "wb");
  if (out == NULL) {
    printf("Error: could not open '%s' for writing\n", executable_path);
    return 0;
  }
  int emitted = HDEmitPE(program, out, source_path);
  fclose(out);
  if (!emitted) {
    remove(executable_path);
    return 0;
  }

  const char *arguments[] = {executable_path, NULL};
  intptr_t status = _spawnv(_P_WAIT, executable_path, arguments);
  remove(executable_path);
  if (status == -1) {
    printf("holyd: could not start direct executable '%s'.\n", executable_path);
    return 0;
  }
  *exit_code = (int)status;
  return 1;
#else
  (void)program;
  (void)source_path;
  (void)executable_path;
  (void)exit_code;
  printf("holyd: -run is currently implemented for the Windows direct-PE backend.\n");
  return 0;
#endif
}

// Runs the lexer alone, so this still works on a file the parser rejects.
static void dump_tokens(const char *source) {
  Lexer lexer;
  LexerInit(&lexer, source);

  for (;;) {
    Token token = LexerNextToken(&lexer);

    // A newline token would otherwise break the one-token-per-line layout,
    // and EOF has no text to show at all.
    if (token.type == TOKEN_NEWLINE) {
      printf("%4zu:%-3zu %-16s '\\n'\n", token.span.start_line,
             token.span.start_column, TokenTypeToString(token.type));
    } else if (token.type == TOKEN_EOF) {
      printf("%4zu:%-3zu %-16s\n", token.span.start_line,
             token.span.start_column, TokenTypeToString(token.type));
      break;
    } else {
      printf("%4zu:%-3zu %-16s '%.*s'\n", token.span.start_line,
             token.span.start_column, TokenTypeToString(token.type),
             (int)token.length, token.start);
    }
  }
}

int main(int argc, char **argv) {
  int use_interpreter = 0;
  int dump_bytecode = 0;
  int tokens_only = 0;
  int ast_only = 0;
  int dump_symbols = 0;
  int dump_types = 0;
  int run_tests = 0;
  int emit_c = 0;
  int emit_asm = 0;
  int emit_exe = 0;
  int emit_pe = 0;
  int run_direct = 0;
  const char *output_path = NULL;
  const char *source_path = NULL;

  if (argc < 2) {
    print_usage();
    return 1;
  }

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];

    if (strcmp(arg, "--help") == 0) {
      print_usage();
      return 0;
    }
    if (strcmp(arg, "--test") == 0) {
      run_tests = 1;
      continue;
    }
    if (strcmp(arg, "--interpret") == 0) {
      use_interpreter = 1;
      continue;
    }
    if (strcmp(arg, "-run") == 0) {
      run_direct = 1;
      continue;
    }
    if (strcmp(arg, "--dump-bytecode") == 0) {
      dump_bytecode = 1;
      continue;
    }
    if (strcmp(arg, "-tokens") == 0) {
      tokens_only = 1;
      continue;
    }
    if (strcmp(arg, "-ast") == 0) {
      ast_only = 1;
      continue;
    }
    if (strcmp(arg, "--emit-asm") == 0) {
      emit_asm = 1;
      continue;
    }
    if (strcmp(arg, "--dump-symbols") == 0) {
      dump_symbols = 1;
      continue;
    }
    if (strcmp(arg, "--dump-types") == 0) {
      dump_types = 1;
      continue;
    }
    if (strcmp(arg, "--emit-c") == 0) {
      emit_c = 1;
      continue;
    }
    if (strcmp(arg, "--emit-exe") == 0) {
      emit_exe = 1;
      continue;
    }
    if (strcmp(arg, "--emit-pe") == 0) {
      emit_pe = 1;
      continue;
    }
    if (strcmp(arg, "-o") == 0) {
      if (i + 1 >= argc) {
        printf("holyd: -o needs a file name\n");
        return 1;
      }
      output_path = argv[++i];
      continue;
    }

    // Anything else starting with '-' is a flag we do not run. Saying so beats
    // the old behaviour, where an unknown flag was quietly taken as the source
    // path and then overwritten by the real one.
    if (arg[0] == '-') {
      const char *reason = unimplemented_reason(arg);
      if (reason != NULL) {
        printf("holyd: '%s' is not implemented: %s\n", arg, reason);
      } else {
        printf("holyd: unknown option '%s'\n", arg);
      }
      printf("Run 'holyd --help' for the full list.\n");
      return 1;
    }

    if (source_path != NULL) {
      printf("holyd: one source file at a time (already have '%s')\n",
             source_path);
      return 1;
    }
    source_path = arg;
  }

  if (run_tests) {
    /* A path given alongside --test names the directory to scan rather than
     * a script to run. */
    return test(source_path != NULL ? source_path : HOLYD_DEFAULT_TEST_DIR);
  }

  if (emit_c + emit_asm + emit_exe + emit_pe + run_direct > 1) {
    printf("holyd: choose only one output mode.\n");
    return 1;
  }
  if (run_direct && output_path != NULL) {
    printf("holyd: -run owns a temporary executable and does not accept -o.\n");
    return 1;
  }

  if (source_path == NULL) {
    print_usage();
    return 1;
  }

  // validate Extension
  if (!check_ext(source_path)) {
    printf("Error: File '%s' must have .hd extension\n", source_path);
    return 1;
  }

  char *source = read_file(source_path);
  if (source == NULL) {
    printf("Error: Could not open or read file '%s'\n", source_path);
    return 1;
  }

  if (tokens_only) {
    dump_tokens(source);
    free(source);
    return 0;
  }

  Parser parser;
  ParserInit(&parser, source);
  ASTNode *program = ParseProgram(&parser);

  if (program == NULL) {
    printf("Error: Failed to parse file.\n");
    free(source);
    return 1;
  }

  if (ast_only) {
    ASTPrint(program, 0);
    free(source);
    return 0;
  }

  HDResolution resolution;
  if (!HDResolveProgram(program, &resolution)) {
    HDResolutionFree(&resolution);
    free(source);
    return 1;
  }

  if (dump_symbols) {
    HDResolutionPrint(&resolution);
    HDResolutionFree(&resolution);
    free(source);
    return 0;
  }

  HDTypeCheck types;
  if (!HDTypeCheckProgram(program, &resolution, &types)) {
    HDTypeCheckFree(&types);
    HDResolutionFree(&resolution);
    free(source);
    return 1;
  }

  if (program_has_classes(program) && !emit_c) {
    printf("Error: classes currently execute through --emit-c only.\n");
    HDTypeCheckFree(&types);
    HDResolutionFree(&resolution);
    free(source);
    return 1;
  }

  if (dump_types) {
    HDTypeCheckPrint(&types, &resolution, program);
    HDTypeCheckFree(&types);
    HDResolutionFree(&resolution);
    free(source);
    return 0;
  }

  if (run_direct) {
    char *temporary_path = temporary_run_name(source_path);
    if (temporary_path == NULL) {
      printf("Error: out of memory choosing a temporary executable name.\n");
      HDTypeCheckFree(&types);
      HDResolutionFree(&resolution);
      free(source);
      return 1;
    }
    int program_exit = 0;
    int ok = emit_and_run_pe(program, source_path, temporary_path,
                             &program_exit);
    free(temporary_path);
    HDTypeCheckFree(&types);
    HDResolutionFree(&resolution);
    free(source);
    if (!ok)
      return 1;
    return program_exit;
  }

  if (emit_pe) {
    char *derived = NULL;
    const char *target = output_path;
    if (target == NULL) {
      size_t len = strlen(source_path);
      derived = malloc(len + 3); /* ".hd" -> ".exe" plus NUL */
      if (derived == NULL) {
        printf("Error: out of memory choosing an output name.\n");
        HDTypeCheckFree(&types);
        HDResolutionFree(&resolution);
        free(source);
        return 1;
      }
      memcpy(derived, source_path, len - 2);
      memcpy(derived + len - 2, "exe", 4);
      target = derived;
    }

    FILE *out = fopen(target, "wb");
    if (out == NULL) {
      printf("Error: could not open '%s' for writing\n", target);
      free(derived);
      HDTypeCheckFree(&types);
      HDResolutionFree(&resolution);
      free(source);
      return 1;
    }
    int ok = HDEmitPE(program, out, source_path);
    fclose(out);
    if (ok)
      printf("holyd: wrote %s\n", target);
    free(derived);
    HDTypeCheckFree(&types);
    HDResolutionFree(&resolution);
    free(source);
    return ok ? 0 : 1;
  }

  if (emit_c || emit_asm || emit_exe) {
    /* Default output name is the source with its extension swapped, so
     * `holyd --emit-c samples/gui.hd` lands at samples/gui.c, and
     * `--emit-asm` on the same file lands at samples/gui.s, and
     * `--emit-exe` lands at samples/gui.exe. */
    char *derived = NULL;
    const char *target = output_path;
    if (target == NULL) {
      size_t len = strlen(source_path);
      derived = malloc(len + 3); /* enough for ".hd" -> ".exe" plus NUL */
      if (derived == NULL) {
        printf("Error: out of memory choosing an output name.\n");
        HDTypeCheckFree(&types);
        HDResolutionFree(&resolution);
        free(source);
        return 1;
      }
      memcpy(derived, source_path, len - 2);
      if (emit_exe) {
        memcpy(derived + len - 2, "exe", 4);
      } else {
        derived[len - 2] = emit_asm ? 's' : 'c';
        derived[len - 1] = '\0';
      }
      target = derived;
    }

    if (emit_exe) {
      char *assembly_path = temporary_assembly_name(target);
      if (assembly_path == NULL) {
        printf("Error: out of memory choosing a temporary assembly name.\n");
        free(derived);
        HDTypeCheckFree(&types);
        HDResolutionFree(&resolution);
        free(source);
        return 1;
      }

      FILE *existing = fopen(assembly_path, "rb");
      if (existing != NULL) {
        fclose(existing);
        printf("Error: temporary assembly path '%s' already exists; refusing to replace it.\n",
               assembly_path);
        free(assembly_path);
        free(derived);
        HDTypeCheckFree(&types);
        HDResolutionFree(&resolution);
        free(source);
        return 1;
      }

      FILE *assembly = fopen(assembly_path, "wb");
      if (assembly == NULL) {
        printf("Error: could not open '%s' for writing\n", assembly_path);
        free(assembly_path);
        free(derived);
        HDTypeCheckFree(&types);
        HDResolutionFree(&resolution);
        free(source);
        return 1;
      }

      int emitted = HDEmitAsm(program, &resolution, assembly, source_path);
      fclose(assembly);
      int linked = emitted && assemble_and_link(assembly_path, target);
      remove(assembly_path);
      free(assembly_path);
      if (linked)
        printf("holyd: wrote %s\n", target);
      free(derived);
      HDTypeCheckFree(&types);
      HDResolutionFree(&resolution);
      free(source);
      return linked ? 0 : 1;
    }

    FILE *out = fopen(target, "wb");
    if (out == NULL) {
      printf("Error: could not open '%s' for writing\n", target);
      free(derived);
      HDTypeCheckFree(&types);
      HDResolutionFree(&resolution);
      free(source);
      return 1;
    }

    int ok = emit_asm ? HDEmitAsm(program, &resolution, out, source_path)
                      : HDEmitC(program, &resolution, &types, out, source_path);
    fclose(out);
    if (ok) {
      printf("holyd: wrote %s\n", target);
    }
    free(derived);
    HDTypeCheckFree(&types);
    HDResolutionFree(&resolution);
    free(source);
    return ok ? 0 : 1;
  }

  if (use_interpreter) {
    /* The AST walker has no FFI, so the Win* calls are unavailable here.
     * The constants are still defined so a script that names one gets an
     * "unknown function" on the call rather than an undefined variable
     * several lines earlier. */
    Environment env;
    EnvInit(&env);
    ffi_define_globals(&env);
    EvalNode(program, &env);
  } else {
    HDProgram bytecode;
    HDProgramInit(&bytecode);
    if (!HDCompileProgram(program, &resolution, &bytecode)) {
      HDTypeCheckFree(&types);
      HDResolutionFree(&resolution);
      free(source);
      return 1;
    }
    if (dump_bytecode) {
      HDDumpProgram(&bytecode);
    }
    if (!HDRunProgram(&bytecode)) {
      HDTypeCheckFree(&types);
      HDResolutionFree(&resolution);
      free(source);
      return 1;
    }
  }

  HDTypeCheckFree(&types);
  HDResolutionFree(&resolution);
  free(source);
  return 0;
}
