#include "emit_pe.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PE32+ constants. Keeping these local avoids depending on Windows headers:
 * the emitted image should be constructible by Holyd on a build machine that
 * has no SDK installed. */
enum {
  PE_OFFSET = 0x80,
  PE_OPTIONAL_SIZE = 0xf0,
  PE_SECTION_HEADERS = PE_OFFSET + 4 + 20 + PE_OPTIONAL_SIZE,
  PE_HEADERS_SIZE = 0x200,
  PE_FILE_ALIGNMENT = 0x200,
  PE_SECTION_ALIGNMENT = 0x1000,
  PE_TEXT_RVA = 0x1000,
  PE_RDATA_RVA = 0x2000,
  PE_TEXT_FILE_OFFSET = 0x200,
  PE_RDATA_FILE_OFFSET = 0x400,
  PE_FILE_SIZE = 0x600,

  PE_IMPORT_DESCRIPTOR = 0x00,
  PE_IMPORT_NULL_DESCRIPTOR = 0x14,
  PE_IMPORT_LOOKUP_TABLE = 0x28,
  PE_IMPORT_ADDRESS_TABLE = 0x38,
  PE_IMPORT_DLL_NAME = 0x48,
  PE_IMPORT_HINT_NAME = 0x56,
};

static void put_u16(unsigned char *image, size_t offset, uint16_t value) {
  image[offset] = (unsigned char)value;
  image[offset + 1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *image, size_t offset, uint32_t value) {
  for (int i = 0; i < 4; i++)
    image[offset + (size_t)i] = (unsigned char)(value >> (i * 8));
}

static void put_u64(unsigned char *image, size_t offset, uint64_t value) {
  for (int i = 0; i < 8; i++)
    image[offset + (size_t)i] = (unsigned char)(value >> (i * 8));
}

static int name_is(const char *name, int length, const char *text) {
  size_t wanted = strlen(text);
  return length == (int)wanted && strncmp(name, text, wanted) == 0;
}

static int evaluate_constant(const ASTNode *node, long long *value) {
  if (!node) {
    printf("PE error: expected a return expression.\n");
    return 0;
  }

  switch (node->type) {
  case AST_NUMBER:
    *value = node->as.integer_literal.value;
    return 1;

  case AST_UNARY_OP:
    if (node->as.unary_op.operator_type != TOKEN_MINUS) {
      printf("PE error: only unary '-' is supported in a direct return expression.\n");
      return 0;
    }
    if (!evaluate_constant(node->as.unary_op.operand, value))
      return 0;
    *value = -*value;
    return 1;

  case AST_BINARY_OP: {
    long long left;
    long long right;
    if (!evaluate_constant(node->as.binary_op.left, &left) ||
        !evaluate_constant(node->as.binary_op.right, &right))
      return 0;
    switch (node->as.binary_op.operator_type) {
    case TOKEN_PLUS:
      *value = (long long)((uint64_t)left + (uint64_t)right);
      return 1;
    case TOKEN_MINUS:
      *value = (long long)((uint64_t)left - (uint64_t)right);
      return 1;
    case TOKEN_STAR:
      *value = (long long)((uint64_t)left * (uint64_t)right);
      return 1;
    case TOKEN_SLASH:
    case TOKEN_PERCENT:
      if (right == 0 || (left == LLONG_MIN && right == -1)) {
        printf("PE error: constant division is undefined for this expression.\n");
        return 0;
      }
      *value = node->as.binary_op.operator_type == TOKEN_SLASH
                   ? left / right
                   : left % right;
      return 1;
    default:
      printf("PE error: this binary operator is not supported by the direct backend yet.\n");
      return 0;
    }
  }

  default:
    printf("PE error: direct return expressions currently need integer constants.\n");
    return 0;
  }
}

/* This first entry-point compiler is intentionally strict. Rejecting an AST
 * node is safer than ignoring it and producing an executable with different
 * semantics from the VM. */
static int find_exit_status(const ASTNode *program, uint32_t *exit_status) {
  const ASTNode *entry = NULL;
  if (!program || program->type != AST_BLOCK) {
    printf("PE error: program root is not a block.\n");
    return 0;
  }

  for (int i = 0; i < program->as.block.statement_count; i++) {
    const ASTNode *statement = program->as.block.statements[i];
    if (!statement)
      continue;
    if (statement->type == AST_BLOCK &&
        statement->as.block.statement_count == 0)
      continue;
    if (statement->type != AST_FUNC_DECL) {
      printf("PE error: top-level execution is not supported by the direct backend yet.\n");
      return 0;
    }
    if (name_is(statement->as.function_decl.name,
                statement->as.function_decl.name_length, "main") ||
        name_is(statement->as.function_decl.name,
                statement->as.function_decl.name_length, "Main")) {
      if (entry != NULL) {
        printf("PE error: direct backend found more than one entry function.\n");
        return 0;
      }
      entry = statement;
    }
  }

  if (entry == NULL) {
    *exit_status = 0;
    return 1;
  }
  if (entry->as.function_decl.parameter_count != 0) {
    printf("PE error: direct entry function must have no parameters.\n");
    return 0;
  }

  const ASTNode *body = entry->as.function_decl.body;
  if (!body || body->type != AST_BLOCK) {
    printf("PE error: direct entry function needs a block body.\n");
    return 0;
  }
  if (body->as.block.statement_count == 0) {
    *exit_status = 0;
    return 1;
  }
  if (body->as.block.statement_count != 1 ||
      body->as.block.statements[0]->type != AST_RETURN) {
    printf("PE error: direct entry function currently supports one return statement.\n");
    return 0;
  }

  long long result = 0;
  const ASTNode *return_node = body->as.block.statements[0];
  if (return_node->as.return_statement.expression != NULL &&
      !evaluate_constant(return_node->as.return_statement.expression, &result))
    return 0;
  *exit_status = (uint32_t)result;
  return 1;
}

static void write_section_header(unsigned char *image, size_t offset,
                                 const char *name, uint32_t virtual_size,
                                 uint32_t virtual_address, uint32_t raw_size,
                                 uint32_t raw_offset, uint32_t characteristics) {
  memset(image + offset, 0, 40);
  memcpy(image + offset, name, strlen(name));
  put_u32(image, offset + 8, virtual_size);
  put_u32(image, offset + 12, virtual_address);
  put_u32(image, offset + 16, raw_size);
  put_u32(image, offset + 20, raw_offset);
  put_u32(image, offset + 36, characteristics);
}

int HDEmitPE(ASTNode *ast, FILE *out, const char *source_name) {
  uint32_t exit_status;
  if (!find_exit_status(ast, &exit_status))
    return 0;

  unsigned char *image = calloc(1, PE_FILE_SIZE);
  if (!image) {
    printf("PE error: out of memory while building executable image.\n");
    return 0;
  }

  /* DOS stub and PE/COFF header. The DOS payload is intentionally empty; the
   * loader needs MZ and e_lfanew, not a legacy executable. */
  image[0] = 'M';
  image[1] = 'Z';
  put_u32(image, 0x3c, PE_OFFSET);
  memcpy(image + PE_OFFSET, "PE\0\0", 4);
  size_t coff = PE_OFFSET + 4;
  put_u16(image, coff + 0, 0x8664); /* AMD64 */
  put_u16(image, coff + 2, 2);
  put_u16(image, coff + 16, PE_OPTIONAL_SIZE);
  put_u16(image, coff + 18, 0x0022); /* executable, large-address-aware */

  size_t optional = coff + 20;
  put_u16(image, optional + 0, 0x20b); /* PE32+ */
  put_u32(image, optional + 4, PE_FILE_ALIGNMENT); /* SizeOfCode */
  put_u32(image, optional + 8, PE_FILE_ALIGNMENT); /* initialized data */
  put_u32(image, optional + 16, PE_TEXT_RVA);      /* entry RVA */
  put_u32(image, optional + 20, PE_TEXT_RVA);      /* BaseOfCode */
  put_u64(image, optional + 24, UINT64_C(0x140000000));
  put_u32(image, optional + 32, PE_SECTION_ALIGNMENT);
  put_u32(image, optional + 36, PE_FILE_ALIGNMENT);
  put_u16(image, optional + 40, 6); /* minimum Windows version */
  put_u16(image, optional + 48, 6);
  put_u32(image, optional + 56, 0x3000); /* SizeOfImage */
  put_u32(image, optional + 60, PE_HEADERS_SIZE);
  put_u16(image, optional + 68, 3);     /* console subsystem */
  put_u16(image, optional + 70, 0x0100); /* NX compatible */
  put_u64(image, optional + 72, UINT64_C(0x100000));
  put_u64(image, optional + 80, UINT64_C(0x1000));
  put_u64(image, optional + 88, UINT64_C(0x100000));
  put_u64(image, optional + 96, UINT64_C(0x1000));
  put_u32(image, optional + 108, 16); /* data-directory count */
  put_u32(image, optional + 120, PE_RDATA_RVA + PE_IMPORT_DESCRIPTOR);
  put_u32(image, optional + 124, 40); /* import directory */

  write_section_header(image, PE_SECTION_HEADERS, ".text", 16,
                       PE_TEXT_RVA, PE_FILE_ALIGNMENT, PE_TEXT_FILE_OFFSET,
                       0x60000020); /* code, execute, read */
  write_section_header(image, PE_SECTION_HEADERS + 40, ".rdata", 0x64,
                       PE_RDATA_RVA, PE_FILE_ALIGNMENT, PE_RDATA_FILE_OFFSET,
                       0x40000040); /* initialized data, read */

  /* Entry point:
   *   sub rsp, 40h   Win64 shadow space and call alignment
   *   mov ecx, status
   *   call [rip + ExitProcess IAT]
   * ExitProcess never returns. The RIP-relative call means ASLR needs no
   * base relocation for this first image. */
  unsigned char *code = image + PE_TEXT_FILE_OFFSET;
  code[0] = 0x48; code[1] = 0x83; code[2] = 0xec; code[3] = 0x28;
  code[4] = 0xb9;
  put_u32(code, 5, exit_status);
  code[9] = 0xff; code[10] = 0x15;
  int32_t call_displacement = (int32_t)(
      (PE_RDATA_RVA + PE_IMPORT_ADDRESS_TABLE) - (PE_TEXT_RVA + 15));
  put_u32(code, 11, (uint32_t)call_displacement);
  code[15] = 0xcc;

  unsigned char *rdata = image + PE_RDATA_FILE_OFFSET;
  put_u32(rdata, PE_IMPORT_DESCRIPTOR + 0,
          PE_RDATA_RVA + PE_IMPORT_LOOKUP_TABLE);
  put_u32(rdata, PE_IMPORT_DESCRIPTOR + 12,
          PE_RDATA_RVA + PE_IMPORT_DLL_NAME);
  put_u32(rdata, PE_IMPORT_DESCRIPTOR + 16,
          PE_RDATA_RVA + PE_IMPORT_ADDRESS_TABLE);
  put_u64(rdata, PE_IMPORT_LOOKUP_TABLE,
          PE_RDATA_RVA + PE_IMPORT_HINT_NAME);
  put_u64(rdata, PE_IMPORT_ADDRESS_TABLE,
          PE_RDATA_RVA + PE_IMPORT_HINT_NAME);
  memcpy(rdata + PE_IMPORT_DLL_NAME, "KERNEL32.dll", 13);
  /* Hint is zero; Windows resolves the name regardless of the hint. */
  memcpy(rdata + PE_IMPORT_HINT_NAME + 2, "ExitProcess", 12);

  int ok = fwrite(image, 1, PE_FILE_SIZE, out) == PE_FILE_SIZE;
  free(image);
  if (!ok) {
    printf("PE error: failed writing executable%s%s.\n",
           source_name ? " for " : "", source_name ? source_name : "");
  }
  return ok;
}
