/* HolyD value semantics , shared by the bytecode VM and by transpiled C.
 * See runtime.h for why the operators live here rather than inline in the
 * VM's dispatch loop. */

#include "runtime.h"
#include "ffi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- Constructors ------------------------------------------- */

HDValue int_value(long long v) {
  HDValue value;
  value.type = VAL_INT;
  value.i64 = v;
  value.f64 = 0.0;
  value.str = NULL;
  value.str_len = 0;
  value.elements = NULL;
  value.array_len = 0;
  return value;
}

HDValue string_value(const char *s, int len) {
  HDValue value = int_value(0);
  value.type = VAL_STRING;
  value.str = s;
  value.str_len = len;
  return value;
}

HDValue float_value(double v) {
  HDValue value = int_value(0);
  value.type = VAL_FLOAT;
  value.f64 = v;
  return value;
}

int HDCastKindFromTypeSyntax(const TypeSyntax *type, HDCastKind *kind) {
  static const struct {
    const char *name;
    HDCastKind kind;
  } names[] = {
      {"F64", HD_CAST_FLOAT}, {"double", HD_CAST_FLOAT},
      {"Bool", HD_CAST_BOOL}, {"bool", HD_CAST_BOOL},
      {"I8", HD_CAST_INT}, {"U8", HD_CAST_INT},
      {"I16", HD_CAST_INT}, {"U16", HD_CAST_INT},
      {"I32", HD_CAST_INT}, {"U32", HD_CAST_INT},
      {"I64", HD_CAST_INT}, {"U64", HD_CAST_INT},
      {"int", HD_CAST_INT}, {"uint", HD_CAST_INT},
      {"long", HD_CAST_INT}, {"ulong", HD_CAST_INT},
  };
  if (!type || !kind || type->kind != TYPE_SYNTAX_NAMED)
    return 0;
  const char *name = type->as.named.name;
  int length = type->as.named.name_length;
  for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
    int expected = (int)strlen(names[i].name);
    if (length == expected && strncmp(name, names[i].name, (size_t)length) == 0) {
      *kind = names[i].kind;
      return 1;
    }
  }
  return 0;
}

/* ---------------- Inspection --------------------------------------------- */

int HDIsNumeric(HDValue value) {
  return value.type == VAL_INT || value.type == VAL_FLOAT;
}

double HDAsDouble(HDValue value) {
  return value.type == VAL_FLOAT ? value.f64 : (double)value.i64;
}

int HDTruthy(HDValue value) {
  if (value.type == VAL_INT)
    return value.i64 != 0;
  if (value.type == VAL_FLOAT)
    return value.f64 != 0.0;
  if (value.type == VAL_STRING)
    return value.str_len != 0;
  if (value.type == VAL_ARRAY)
    return value.array_len != 0;
  return 0;
}

void HDAbort(const char *message) {
  printf("%s", message);
  exit(1);
}

/* ---------------- Printing ------------------------------------------------ */

/* printf carries no %f conversion, so build the digits here: whole part,
 * then six rounded decimals with trailing zeros trimmed. Magnitudes past
 * 64-bit say so instead of overflowing the cast. */
void HDPrintDouble(double value) {
  if (value != value) {
    printf("nan");
    return;
  }
  if (value < 0.0) {
    printf("-");
    value = -value;
  }
  if (value >= 9.0e18) {
    printf("<out of range>");
    return;
  }

  long long whole = (long long)value;
  long long frac = (long long)((value - (double)whole) * 1000000.0 + 0.5);
  if (frac >= 1000000) {
    whole++;
    frac -= 1000000;
  }

  char digits[24];
  snprintf(digits, sizeof(digits), "%06lld", frac);

  int last = 5;
  while (last > 0 && digits[last] == '0')
    last--;
  digits[last + 1] = '\0';

  printf("%lld.%s", whole, digits);
}

/* Escapes are resolved here rather than in the lexer, so a string value
 * still points at the raw source bytes and \n is two characters until it
 * reaches this function. Transpiled code carries the same raw bytes into
 * .rodata, so both paths unescape at exactly this point. */
static void print_string_escaped(const char *s, int len) {
  for (int j = 0; j < len; j++) {
    if (s[j] == '\\' && j + 1 < len) {
      char next = s[j + 1];
      if (next == 'n') {
        printf("\n");
        j++;
      } else if (next == '\\') {
        printf("\\");
        j++;
      } else if (next == '"') {
        printf("\"");
        j++;
      } else {
        printf("%c", s[j]);
      }
    } else {
      printf("%c", s[j]);
    }
  }
}

void HDPrintValue(HDValue value) {
  if (value.type == VAL_STRING) {
    print_string_escaped(value.str, value.str_len);
  } else if (value.type == VAL_ARRAY) {
    printf("[");
    for (int i = 0; i < value.array_len; i++) {
      if (i > 0)
        printf(", ");
      HDPrintValue(value.elements[i]);
    }
    printf("]");
  } else if (value.type == VAL_FLOAT) {
    HDPrintDouble(value.f64);
  } else {
    printf("%lld", value.i64);
  }
}

HDValue HDPrintN(int argc, HDValue *args, int add_newline) {
  for (int i = 0; i < argc; i++)
    HDPrintValue(args[i]);
  if (add_newline)
    printf("\n");
  return int_value(0);
}

/* ---------------- Operators ----------------------------------------------- */

int HDConcat(HDValue left, HDValue right, HDValue *out) {
  if (left.type == VAL_STRING && right.type == VAL_STRING) {
    int len = left.str_len + right.str_len;
    char *joined = (char *)malloc((size_t)len + 1);
    if (!joined) {
      printf("Runtime error: out of memory in ~.\n");
      return 0;
    }
    memcpy(joined, left.str, (size_t)left.str_len);
    memcpy(joined + left.str_len, right.str, (size_t)right.str_len);
    joined[len] = '\0';
    *out = string_value(joined, len);
    return 1;
  }

  if (left.type == VAL_ARRAY || right.type == VAL_ARRAY) {
    int left_len = left.type == VAL_ARRAY ? left.array_len : 1;
    int right_len = right.type == VAL_ARRAY ? right.array_len : 1;
    HDValue array = int_value(0);
    array.type = VAL_ARRAY;
    array.array_len = left_len + right_len;
    array.elements =
        (HDValue *)malloc(sizeof(HDValue) * (size_t)array.array_len);
    if (!array.elements) {
      printf("Runtime error: out of memory in ~.\n");
      return 0;
    }

    int at = 0;
    if (left.type == VAL_ARRAY) {
      for (int i = 0; i < left.array_len; i++)
        array.elements[at++] = left.elements[i];
    } else {
      array.elements[at++] = left;
    }
    if (right.type == VAL_ARRAY) {
      for (int i = 0; i < right.array_len; i++)
        array.elements[at++] = right.elements[i];
    } else {
      array.elements[at++] = right;
    }
    *out = array;
    return 1;
  }

  printf("Runtime error: ~ expects strings or arrays.\n");
  return 0;
}

HDValue HDNot(HDValue value) { return int_value(!HDTruthy(value)); }

/* Bitwise and shift operators are integer-only, as they are in D. Floats
 * reach here through the same HDBinary door as everything else, so the
 * check is explicit rather than implied by a cast. */
static int integer_only(HDBinOp op, HDValue left, HDValue right,
                        const char *name, long long *a, long long *b) {
  (void)op;
  if (left.type != VAL_INT || right.type != VAL_INT) {
    printf("Runtime error: %s expects integers.\n", name);
    return 0;
  }
  *a = left.i64;
  *b = right.i64;
  return 1;
}

/* A shift of 64 or more, or of a negative amount, is undefined in C rather
 * than merely surprising, so it is refused instead of being passed through
 * to the hardware. */
static int shift_amount(long long by, const char *name, int *out) {
  if (by < 0 || by > 63) {
    printf("Runtime error: %s by %lld is out of range (0 to 63).\n", name, by);
    return 0;
  }
  *out = (int)by;
  return 1;
}

/* Exponentiation by repeated multiplication. Deliberately no pow(): that
 * would put libm in the link line of every transpiled program, and HolyD's
 * standalone build otherwise needs nothing but libc and three system
 * libraries. The cost is that the exponent has to be a non-negative
 * integer; a fractional one is refused rather than silently truncated. */
static int power(HDValue base, HDValue exponent, HDValue *out) {
  if (exponent.type != VAL_INT) {
    printf("Runtime error: ^^ expects an integer exponent.\n");
    return 0;
  }
  if (exponent.i64 < 0) {
    printf("Runtime error: ^^ expects a non-negative exponent.\n");
    return 0;
  }

  if (base.type == VAL_FLOAT) {
    double acc = 1.0;
    for (long long i = 0; i < exponent.i64; i++)
      acc = acc * base.f64;
    *out = float_value(acc);
    return 1;
  }

  long long acc = 1;
  for (long long i = 0; i < exponent.i64; i++)
    acc = acc * base.i64;
  *out = int_value(acc);
  return 1;
}

int HDBinary(HDBinOp op, HDValue left, HDValue right, HDValue *out) {
  if (op == HD_CONCAT)
    return HDConcat(left, right, out);

  if (!HDIsNumeric(left) || !HDIsNumeric(right)) {
    printf("Runtime error: numeric operator used on a non-numeric value.\n");
    return 0;
  }

  if (op == HD_POW)
    return power(left, right, out);

  /* These never promote to double, so they are handled before the float
   * path below rather than inside it. */
  {
    long long a = 0;
    long long b = 0;
    int by = 0;
    switch (op) {
    case HD_MOD:
      if (!integer_only(op, left, right, "%", &a, &b))
        return 0;
      if (b == 0) {
        printf("Runtime error: division by zero.\n");
        return 0;
      }
      *out = int_value(a - (a / b) * b);
      return 1;
    case HD_BAND:
      if (!integer_only(op, left, right, "&", &a, &b))
        return 0;
      *out = int_value(a & b);
      return 1;
    case HD_BOR:
      if (!integer_only(op, left, right, "|", &a, &b))
        return 0;
      *out = int_value(a | b);
      return 1;
    case HD_BXOR:
      if (!integer_only(op, left, right, "^", &a, &b))
        return 0;
      *out = int_value(a ^ b);
      return 1;
    case HD_SHL:
      if (!integer_only(op, left, right, "<<", &a, &b) ||
          !shift_amount(b, "<<", &by))
        return 0;
      /* Shifting into or past the sign bit is undefined for a signed left
       * operand, so the shift is done unsigned and reinterpreted. */
      *out = int_value((long long)((unsigned long long)a << by));
      return 1;
    case HD_SHR:
      if (!integer_only(op, left, right, ">>", &a, &b) ||
          !shift_amount(b, ">>", &by))
        return 0;
      *out = int_value(a >> by); /* arithmetic: sign is preserved */
      return 1;
    case HD_USHR:
      if (!integer_only(op, left, right, ">>>", &a, &b) ||
          !shift_amount(b, ">>>", &by))
        return 0;
      *out = int_value((long long)((unsigned long long)a >> by));
      return 1;
    default:
      break;
    }
  }

  /* A float on either side promotes both, as in C. Comparisons still yield
   * an int so the jump ops stay integer-only. */
  if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
    double a = HDAsDouble(left);
    double b = HDAsDouble(right);
    switch (op) {
    case HD_ADD:
      *out = float_value(a + b);
      return 1;
    case HD_SUB:
      *out = float_value(a - b);
      return 1;
    case HD_MUL:
      *out = float_value(a * b);
      return 1;
    case HD_DIV:
      if (b == 0.0) {
        printf("Runtime error: division by zero.\n");
        return 0;
      }
      *out = float_value(a / b);
      return 1;
    case HD_EQ:
      *out = int_value(a == b);
      return 1;
    case HD_NE:
      *out = int_value(a != b);
      return 1;
    case HD_LT:
      *out = int_value(a < b);
      return 1;
    case HD_GT:
      *out = int_value(a > b);
      return 1;
    case HD_LE:
      *out = int_value(a <= b);
      return 1;
    case HD_GE:
      *out = int_value(a >= b);
      return 1;
    default:
      *out = int_value(0);
      return 1;
    }
  }

  switch (op) {
  case HD_ADD:
    *out = int_value(left.i64 + right.i64);
    return 1;
  case HD_SUB:
    *out = int_value(left.i64 - right.i64);
    return 1;
  case HD_MUL:
    *out = int_value(left.i64 * right.i64);
    return 1;
  case HD_DIV:
    if (right.i64 == 0) {
      printf("Runtime error: division by zero.\n");
      return 0;
    }
    *out = int_value(left.i64 / right.i64);
    return 1;
  case HD_EQ:
    *out = int_value(left.i64 == right.i64);
    return 1;
  case HD_NE:
    *out = int_value(left.i64 != right.i64);
    return 1;
  case HD_LT:
    *out = int_value(left.i64 < right.i64);
    return 1;
  case HD_GT:
    *out = int_value(left.i64 > right.i64);
    return 1;
  case HD_LE:
    *out = int_value(left.i64 <= right.i64);
    return 1;
  case HD_GE:
    *out = int_value(left.i64 >= right.i64);
    return 1;
  default:
    *out = int_value(0);
    return 1;
  }
}

int HDArrayNew(int count, HDValue *elements, HDValue *out) {
  HDValue array = int_value(0);
  array.type = VAL_ARRAY;
  array.array_len = count;
  array.elements = NULL;

  if (count > 0) {
    array.elements = (HDValue *)malloc(sizeof(HDValue) * (size_t)count);
    if (!array.elements) {
      printf("Runtime error: out of memory while creating array.\n");
      return 0;
    }
    for (int i = 0; i < count; i++)
      array.elements[i] = elements[i];
  }

  *out = array;
  return 1;
}

int HDIndex(HDValue array, HDValue index, HDValue *out) {
  if (array.type != VAL_ARRAY || index.type != VAL_INT || index.i64 < 0 ||
      index.i64 >= array.array_len) {
    printf("Runtime error: invalid array access.\n");
    return 0;
  }
  *out = array.elements[index.i64];
  return 1;
}

/* Elements are reached through the shared HDValue.elements pointer, so
 * writing through a copy of the array value updates the one the environment
 * holds. */
int HDIndexSet(HDValue array, HDValue index, HDValue value) {
  if (array.type != VAL_ARRAY || index.type != VAL_INT || index.i64 < 0 ||
      index.i64 >= array.array_len) {
    printf("Runtime error: invalid array assignment.\n");
    return 0;
  }
  array.elements[index.i64] = value;
  return 1;
}

int HDLength(HDValue value, HDValue *out) {
  if (value.type == VAL_ARRAY) {
    *out = int_value(value.array_len);
    return 1;
  }
  if (value.type == VAL_STRING) {
    *out = int_value(value.str_len);
    return 1;
  }
  printf("Runtime error: length requested from non-array/string value.\n");
  return 0;
}

int HDCast(HDCastKind kind, HDValue value, HDValue *out) {
  switch (kind) {
  case HD_CAST_INT:
    if (!HDIsNumeric(value)) {
      printf("Runtime error: cast to integer expects a numeric value.\n");
      return 0;
    }
    *out = int_value((long long)HDAsDouble(value));
    return 1;
  case HD_CAST_FLOAT:
    if (!HDIsNumeric(value)) {
      printf("Runtime error: cast to F64 expects a numeric value.\n");
      return 0;
    }
    *out = float_value(HDAsDouble(value));
    return 1;
  case HD_CAST_BOOL:
    *out = int_value(HDTruthy(value));
    return 1;
  }
  printf("Runtime error: unknown cast target.\n");
  return 0;
}

/* ---------------- Aborting forms, for generated code ---------------------- */

HDValue HDBinaryX(HDBinOp op, HDValue left, HDValue right) {
  HDValue out;
  if (!HDBinary(op, left, right, &out))
    exit(1);
  return out;
}

HDValue HDArrayNewX(int count, HDValue *elements) {
  HDValue out;
  if (!HDArrayNew(count, elements, &out))
    exit(1);
  return out;
}

HDValue HDIndexX(HDValue array, HDValue index) {
  HDValue out;
  if (!HDIndex(array, index, &out))
    exit(1);
  return out;
}

void HDIndexSetX(HDValue array, HDValue index, HDValue value) {
  if (!HDIndexSet(array, index, value))
    exit(1);
}

HDValue HDLengthX(HDValue value) {
  HDValue out;
  if (!HDLength(value, &out))
    exit(1);
  return out;
}

HDValue HDCastX(HDCastKind kind, HDValue value) {
  HDValue out;
  if (!HDCast(kind, value, &out))
    exit(1);
  return out;
}

HDValue HDLoadX(Environment *env, const char *name, int len) {
  HDValue *found = EnvGet(env, name, (size_t)len);
  if (!found) {
    printf("Runtime error: undefined variable '%.*s'.\n", len, name);
    exit(1);
  }
  return *found;
}

HDValue HDNativeX(const char *name, int len, int argc, HDValue *args) {
  NativeFn fn = ffi_lookup_native(name, len);
  if (!fn) {
    printf("Runtime error: unknown function '%.*s'.\n", len, name);
    exit(1);
  }
  return fn(argc, args);
}

HDValue HDUnknownFunctionX(const char *name, int len) {
  printf("Runtime error: unknown function '%.*s'.\n", len, name);
  exit(1);
}

HDValue HDArityX(const char *name, int len, int expects, int got) {
  printf("Runtime error: function '%.*s' expects %d args, got %d.\n", len,
         name, expects, got);
  exit(1);
}
