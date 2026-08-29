#include "ast.h"
#include <stdio.h>
#include <stdlib.h>

static TypeSyntax *TypeSyntaxNew(TypeSyntaxKind kind) {
  TypeSyntax *type = malloc(sizeof(TypeSyntax));

  if (type == NULL) {
    return NULL;
  }

  type->kind = kind;
  return type;
}

static TypeSyntax *TypeSyntaxNewCallable(TypeSyntaxKind kind,
                                         TypeSyntax *return_type,
                                         ParameterSyntax *parameters,
                                         int parameter_count) {
  TypeSyntax *type = TypeSyntaxNew(kind);
  if (type == NULL)
    return NULL;

  type->as.callable.return_type = return_type;
  type->as.callable.parameters = parameters;
  type->as.callable.parameter_count = parameter_count;
  return type;
}

TypeSyntax *TypeSyntaxNewNamed(const char *name, int name_length) {
  TypeSyntax *type = TypeSyntaxNew(TYPE_SYNTAX_NAMED);
  if (type == NULL)
    return NULL;

  type->as.named.name = name;
  type->as.named.name_length = name_length;
  return type;
}

TypeSyntax *TypeSyntaxNewPointer(TypeSyntax *pointee) {
  TypeSyntax *type = TypeSyntaxNew(TYPE_SYNTAX_POINTER);
  if (type == NULL)
    return NULL;

  type->as.pointer.pointee = pointee;
  return type;
}

TypeSyntax *TypeSyntaxNewStaticArray(TypeSyntax *element_type,
                                     ASTNode *length_expression) {
  TypeSyntax *type = TypeSyntaxNew(TYPE_SYNTAX_STATIC_ARRAY);
  if (type == NULL)
    return NULL;

  type->as.static_array.element_type = element_type;
  type->as.static_array.length_expression = length_expression;
  return type;
}

TypeSyntax *TypeSyntaxNewDynamicArray(TypeSyntax *element_type) {
  TypeSyntax *type = TypeSyntaxNew(TYPE_SYNTAX_DYNAMIC_ARRAY);
  if (type == NULL)
    return NULL;

  type->as.dynamic_array.element_type = element_type;
  return type;
}

TypeSyntax *TypeSyntaxNewAssocArray(TypeSyntax *value_type,
                                    TypeSyntax *key_type) {
  TypeSyntax *type = TypeSyntaxNew(TYPE_SYNTAX_ASSOC_ARRAY);
  if (type == NULL)
    return NULL;

  type->as.associative_array.value_type = value_type;
  type->as.associative_array.key_type = key_type;
  return type;
}

TypeSyntax *TypeSyntaxNewFunction(TypeSyntax *return_type,
                                  ParameterSyntax *parameters,
                                  int parameter_count) {
  return TypeSyntaxNewCallable(TYPE_SYNTAX_FUNCTION, return_type, parameters,
                               parameter_count);
}

TypeSyntax *TypeSyntaxNewDelegate(TypeSyntax *return_type,
                                  ParameterSyntax *parameters,
                                  int parameter_count) {
  return TypeSyntaxNewCallable(TYPE_SYNTAX_DELEGATE, return_type, parameters,
                               parameter_count);
}

TypeSyntax *TypeSyntaxNewQualified(TypeQualifier qualifier,
                                   TypeSyntax *base_type) {
  TypeSyntax *type = TypeSyntaxNew(TYPE_SYNTAX_QUALIFIED);
  if (type == NULL)
    return NULL;

  type->as.qualified.qualifier = qualifier;
  type->as.qualified.base_type = base_type;
  return type;
}

TypeSyntax *TypeSyntaxNewTypeof(ASTNode *expression) {
  TypeSyntax *type = TypeSyntaxNew(TYPE_SYNTAX_TYPEOF);
  if (type == NULL)
    return NULL;

  type->as.typeof_expression.expression = expression;
  return type;
}

static void TypePrintIndent(int indent) {
  for (int i = 0; i < indent; i++) {
    fputs("  ", stdout);
  }
}

static const char *TypeQualifierName(TypeQualifier qualifier) {
  switch (qualifier) {
  case TYPE_QUALIFIER_CONST:
    return "const";
  case TYPE_QUALIFIER_IMMUTABLE:
    return "immutable";
  case TYPE_QUALIFIER_SHARED:
    return "shared";
  case TYPE_QUALIFIER_INOUT:
    return "inout";
  default:
    return "unknown";
  }
}

void ParameterSyntaxPrint(const ParameterSyntax *parameter, int indent) {
  TypePrintIndent(indent);
  fputs("PARAMETER", stdout);
  if (parameter == NULL) {
    fputs(" <null>\n", stdout);
    return;
  }

  if (parameter->name != NULL) {
    printf(" %.*s", parameter->name_length, parameter->name);
  } else {
    fputs(" <unnamed>", stdout);
  }
  if (parameter->storage & PARAMETER_STORAGE_SCOPE)
    fputs(" scope", stdout);
  if (parameter->storage & PARAMETER_STORAGE_REF)
    fputs(" ref", stdout);
  if (parameter->storage & PARAMETER_STORAGE_OUT)
    fputs(" out", stdout);
  if (parameter->storage & PARAMETER_STORAGE_LAZY)
    fputs(" lazy", stdout);
  if (parameter->is_variadic)
    fputs(" variadic", stdout);
  fputc('\n', stdout);

  TypePrintIndent(indent + 1);
  fputs("TYPE\n", stdout);
  TypeSyntaxPrint(parameter->type, indent + 2);

  if (parameter->default_value != NULL) {
    TypePrintIndent(indent + 1);
    fputs("DEFAULT_VALUE\n", stdout);
    ASTPrint(parameter->default_value, indent + 2);
  }
}

void TypeSyntaxPrint(const TypeSyntax *type, int indent) {
  TypePrintIndent(indent);
  if (type == NULL) {
    fputs("<null-type>\n", stdout);
    return;
  }

  switch (type->kind) {
  case TYPE_SYNTAX_NAMED:
    printf("NAMED_TYPE %.*s\n", type->as.named.name_length,
           type->as.named.name);
    break;

  case TYPE_SYNTAX_POINTER:
    fputs("POINTER_TYPE\n", stdout);
    TypePrintIndent(indent + 1);
    fputs("POINTEE\n", stdout);
    TypeSyntaxPrint(type->as.pointer.pointee, indent + 2);
    break;

  case TYPE_SYNTAX_STATIC_ARRAY:
    fputs("STATIC_ARRAY_TYPE\n", stdout);
    TypePrintIndent(indent + 1);
    fputs("ELEMENT_TYPE\n", stdout);
    TypeSyntaxPrint(type->as.static_array.element_type, indent + 2);
    TypePrintIndent(indent + 1);
    fputs("LENGTH_EXPRESSION\n", stdout);
    ASTPrint(type->as.static_array.length_expression, indent + 2);
    break;

  case TYPE_SYNTAX_DYNAMIC_ARRAY:
    fputs("DYNAMIC_ARRAY_TYPE\n", stdout);
    TypePrintIndent(indent + 1);
    fputs("ELEMENT_TYPE\n", stdout);
    TypeSyntaxPrint(type->as.dynamic_array.element_type, indent + 2);
    break;

  case TYPE_SYNTAX_ASSOC_ARRAY:
    fputs("ASSOCIATIVE_ARRAY_TYPE\n", stdout);
    TypePrintIndent(indent + 1);
    fputs("VALUE_TYPE\n", stdout);
    TypeSyntaxPrint(type->as.associative_array.value_type, indent + 2);
    TypePrintIndent(indent + 1);
    fputs("KEY_TYPE\n", stdout);
    TypeSyntaxPrint(type->as.associative_array.key_type, indent + 2);
    break;

  case TYPE_SYNTAX_FUNCTION:
  case TYPE_SYNTAX_DELEGATE:
    fputs(type->kind == TYPE_SYNTAX_FUNCTION ? "FUNCTION_TYPE\n"
                                             : "DELEGATE_TYPE\n",
          stdout);
    TypePrintIndent(indent + 1);
    fputs("RETURN_TYPE\n", stdout);
    TypeSyntaxPrint(type->as.callable.return_type, indent + 2);
    TypePrintIndent(indent + 1);
    printf("PARAMETERS %d\n", type->as.callable.parameter_count);
    for (int i = 0; i < type->as.callable.parameter_count; i++) {
      ParameterSyntaxPrint(&type->as.callable.parameters[i], indent + 2);
    }
    break;

  case TYPE_SYNTAX_QUALIFIED:
    printf("QUALIFIED_TYPE %s\n",
           TypeQualifierName(type->as.qualified.qualifier));
    TypePrintIndent(indent + 1);
    fputs("BASE_TYPE\n", stdout);
    TypeSyntaxPrint(type->as.qualified.base_type, indent + 2);
    break;

  case TYPE_SYNTAX_TYPEOF:
    fputs("TYPEOF_TYPE\n", stdout);
    TypePrintIndent(indent + 1);
    fputs("EXPRESSION\n", stdout);
    ASTPrint(type->as.typeof_expression.expression, indent + 2);
    break;
  }
}
