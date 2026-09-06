#ifndef HOLYD_TYPECHECK_H
#define HOLYD_TYPECHECK_H

#include "resolve.h"

/* Semantic types use integer handles rather than pointers. The type arena may
 * grow during inference, so handles remain valid when realloc moves it. */
typedef int HDTypeId;

#define HD_NO_TYPE (-1)

typedef enum {
  HD_TYPE_ERROR,
  HD_TYPE_UNKNOWN,
  HD_TYPE_AUTO,
  HD_TYPE_VOID,
  HD_TYPE_BOOL,
  HD_TYPE_I8,
  HD_TYPE_U8,
  HD_TYPE_I16,
  HD_TYPE_U16,
  HD_TYPE_I32,
  HD_TYPE_U32,
  HD_TYPE_I64,
  HD_TYPE_U64,
  HD_TYPE_F64,
  HD_TYPE_STRING,
  HD_TYPE_NAMED,
  HD_TYPE_POINTER,
  HD_TYPE_STATIC_ARRAY,
  HD_TYPE_DYNAMIC_ARRAY,
  HD_TYPE_ASSOC_ARRAY,
  HD_TYPE_FUNCTION,
  HD_TYPE_DELEGATE,
  HD_TYPE_QUALIFIED
} HDTypeKind;

typedef struct {
  HDTypeKind kind;
  HDTypeId primary;
  HDTypeId secondary;
  const char *name;
  int name_length;
  long long array_length;
  int has_array_length;
  HDTypeId *parameters;
  int parameter_count;
  TypeQualifier qualifier;
} HDType;

typedef struct {
  const ASTNode *node;
  HDTypeId type;
} HDNodeType;

typedef struct {
  HDType *types;
  int type_count;
  int type_capacity;
  HDNodeType *nodes;
  int node_count;
  int node_capacity;
  HDTypeId *symbol_types;
  int symbol_count;
  int had_error;
} HDTypeCheck;

void HDTypeCheckInit(HDTypeCheck *result);
void HDTypeCheckFree(HDTypeCheck *result);
int HDTypeCheckProgram(ASTNode *program, const HDResolution *resolution,
                       HDTypeCheck *result);

HDTypeId HDTypeOfNode(const HDTypeCheck *result, const ASTNode *node);
HDTypeId HDTypeOfSymbol(const HDTypeCheck *result, int symbol_id);
const HDType *HDTypeGet(const HDTypeCheck *result, HDTypeId type);
void HDTypeCheckPrint(const HDTypeCheck *result, const HDResolution *resolution,
                      const ASTNode *program);

#endif
