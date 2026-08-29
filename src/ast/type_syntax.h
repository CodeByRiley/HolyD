#ifndef HOLYD_AST_TYPE_SYNTAX_H
#define HOLYD_AST_TYPE_SYNTAX_H

typedef struct ASTNode ASTNode;
typedef struct TypeSyntax TypeSyntax;

typedef enum {
  TYPE_SYNTAX_NAMED,
  TYPE_SYNTAX_POINTER,
  TYPE_SYNTAX_STATIC_ARRAY,
  TYPE_SYNTAX_DYNAMIC_ARRAY,
  TYPE_SYNTAX_ASSOC_ARRAY,
  TYPE_SYNTAX_FUNCTION,
  TYPE_SYNTAX_DELEGATE,
  TYPE_SYNTAX_QUALIFIED,
  TYPE_SYNTAX_TYPEOF
} TypeSyntaxKind;

typedef enum {
  TYPE_QUALIFIER_CONST,
  TYPE_QUALIFIER_IMMUTABLE,
  TYPE_QUALIFIER_SHARED,
  TYPE_QUALIFIER_INOUT
} TypeQualifier;

typedef enum {
  PARAMETER_STORAGE_NONE  = 0,
  PARAMETER_STORAGE_REF   = 1 << 0,
  PARAMETER_STORAGE_OUT   = 1 << 1,
  PARAMETER_STORAGE_LAZY  = 1 << 2,
  PARAMETER_STORAGE_SCOPE = 1 << 3
} ParameterStorage;

typedef struct {
  TypeSyntax *type;

  /* Optional for an unnamed function/delegate type. */
  const char *name;
  int name_length;

  ASTNode *default_value;
  ParameterStorage storage;
  int is_variadic;
} ParameterSyntax;

struct TypeSyntax {
  TypeSyntaxKind kind;

  union {
    struct {
      const char 			*name;
      int 						name_length;
    } 								named;

    struct {
      TypeSyntax 			*pointee;
    } 								pointer;

    struct {
      TypeSyntax 			*element_type;
      ASTNode 	 			*length_expression;
    } 								static_array;

    struct {
      TypeSyntax 			*element_type;
    } 								dynamic_array;

    struct {
      TypeSyntax 			*value_type;
      TypeSyntax 			*key_type;
    } 								associative_array;

    struct {
      TypeSyntax   	  *return_type;
      ParameterSyntax *parameters;
      int 						parameter_count;
    } 								callable;

    struct {
      TypeQualifier   qualifier;
      TypeSyntax 	  	*base_type;
    } 								qualified;

    struct {
      ASTNode 				*expression;
    } 								typeof_expression;
  } as;
};

TypeSyntax *TypeSyntaxNewNamed(const char *name, int name_length);

TypeSyntax *TypeSyntaxNewPointer(TypeSyntax *pointee);

TypeSyntax *TypeSyntaxNewStaticArray(TypeSyntax *element_type,
                                     ASTNode *length_expression);

TypeSyntax *TypeSyntaxNewDynamicArray(TypeSyntax *element_type);

TypeSyntax *TypeSyntaxNewAssocArray(TypeSyntax *value_type,
                                    TypeSyntax *key_type);

TypeSyntax *TypeSyntaxNewFunction(TypeSyntax *return_type,
                                  ParameterSyntax *parameters,
                                  int parameter_count);

TypeSyntax *TypeSyntaxNewDelegate(TypeSyntax *return_type,
                                  ParameterSyntax *parameters,
                                  int parameter_count);

TypeSyntax *TypeSyntaxNewQualified(TypeQualifier qualifier,
                                   TypeSyntax *base_type);

TypeSyntax *TypeSyntaxNewTypeof(ASTNode *expression);

/* Debug-tree printers used by the front-end's -ast mode. */
void TypeSyntaxPrint(const TypeSyntax *type, int indent);
void ParameterSyntaxPrint(const ParameterSyntax *parameter, int indent);

#endif // HOLYD_AST_TYPE_SYNTAX_H
