#include "eval.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void EnvInit(Environment* env) {
    env->head = NULL;
    env->parent = NULL;
}

void EnvInitChild(Environment* env, Environment* parent) {
    env->head = NULL;
    env->parent = parent;
}

static EnvEntry* EnvFindLocal(Environment* env, const char* name, size_t name_len) {
    EnvEntry* entry = env->head;
    while (entry != NULL) {
        if (entry->name && strlen(entry->name) == name_len && strncmp(entry->name, name, name_len) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

void EnvDefine(Environment* env, const char* name, size_t name_len, HDValue value) {
    EnvEntry* entry = EnvFindLocal(env, name, name_len);
    if (entry != NULL) {
        entry->value = value;
        return;
    }

    EnvEntry* new_entry = (EnvEntry*)malloc(sizeof(EnvEntry));
    new_entry->name = (char*)malloc(name_len + 1);
    memcpy(new_entry->name, name, name_len);
    new_entry->name[name_len] = '\0';
    new_entry->value = value;
    new_entry->next = env->head;
    env->head = new_entry;
}

void EnvSet(Environment* env, const char* name, size_t name_len, HDValue value) {
    Environment* current = env;
    while (current != NULL) {
        EnvEntry* entry = EnvFindLocal(current, name, name_len);
        if (entry != NULL) {
            entry->value = value;
            return;
        }
        current = current->parent;
    }

    EnvDefine(env, name, name_len, value);
}

HDValue* EnvGet(Environment* env, const char* name, size_t name_len) {
    Environment* current = env;
    while (current != NULL) {
        EnvEntry* entry = EnvFindLocal(current, name, name_len);
        if (entry != NULL) {
            return &entry->value;
        }
        current = current->parent;
    }
    return NULL; // Variable not found
}

static HDValue EvalExpression(ASTNode* node, Environment* env) {
    HDValue val;
    val.type = VAL_INT;
    val.i64 = 0;

    if (node == NULL) return val;

    switch (node->type) {
        case AST_NUMBER:
            val.type = VAL_INT;
            val.i64 = node->as.integer_literal.value;
            break;

        case AST_FLOAT:
            val.type = VAL_FLOAT;
            val.f64 = node->as.float_literal.value;
            break;

        case AST_STRING:
            val.type = VAL_STRING;
            val.str = node->as.string_literal.value;
            val.str_len = node->as.string_literal.length;
            break;

        case AST_VAR_REF: {
            HDValue* var = EnvGet(env, node->as.variable_ref.name,
                                  node->as.variable_ref.name_length);
            if (var != NULL) {
                return *var;
            }
            printf("Error: Undefined variable '%.*s'\n",
                   node->as.variable_ref.name_length,
                   node->as.variable_ref.name);
            break;
        }

        case AST_BINARY_OP: {
            HDValue left = EvalExpression(node->as.binary_op.left, env);
            HDValue right = EvalExpression(node->as.binary_op.right, env);

            // D-style string concatenation with ~
            if (node->as.binary_op.operator_type == TOKEN_TILDE) {
                // Simple concatenation for demo purposes
                int new_len = left.str_len + right.str_len;
                char* new_str = (char*)malloc(new_len + 1);
                memcpy(new_str, left.str, left.str_len);
                memcpy(new_str + left.str_len, right.str, right.str_len);
                new_str[new_len] = '\0';
                val.type = VAL_STRING;
                val.str = new_str;
                val.str_len = new_len;
                return val;
            }

            // A float on either side promotes both, matching the VM.
            if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
                double a = left.type == VAL_FLOAT ? left.f64 : (double)left.i64;
                double b = right.type == VAL_FLOAT ? right.f64 : (double)right.i64;
                val.type = VAL_FLOAT;
                switch (node->as.binary_op.operator_type) {
                    case TOKEN_PLUS:  val.f64 = a + b; break;
                    case TOKEN_MINUS: val.f64 = a - b; break;
                    case TOKEN_STAR:  val.f64 = a * b; break;
                    case TOKEN_SLASH: val.f64 = a / b; break;
                    case TOKEN_EQEQ:  val.type = VAL_INT; val.i64 = (a == b); break;
                    case TOKEN_NEQ:   val.type = VAL_INT; val.i64 = (a != b); break;
                    case TOKEN_LT:    val.type = VAL_INT; val.i64 = (a < b); break;
                    case TOKEN_GT:    val.type = VAL_INT; val.i64 = (a > b); break;
                    case TOKEN_LTEQ:  val.type = VAL_INT; val.i64 = (a <= b); break;
                    case TOKEN_GTEQ:  val.type = VAL_INT; val.i64 = (a >= b); break;
                    default: val.type = VAL_INT; val.i64 = 0; break;
                }
                return val;
            }

            // Integer math
            val.type = VAL_INT;
            switch (node->as.binary_op.operator_type) {
                case TOKEN_PLUS:  val.i64 = left.i64 + right.i64; break;
                case TOKEN_MINUS: val.i64 = left.i64 - right.i64; break;
                case TOKEN_STAR:  val.i64 = left.i64 * right.i64; break;
                case TOKEN_SLASH: val.i64 = left.i64 / right.i64; break;
                case TOKEN_EQEQ:  val.i64 = (left.i64 == right.i64); break;
                case TOKEN_NEQ:   val.i64 = (left.i64 != right.i64); break;
                case TOKEN_LT:    val.i64 = (left.i64 < right.i64); break;
                case TOKEN_GT:    val.i64 = (left.i64 > right.i64); break;
                case TOKEN_LTEQ:  val.i64 = (left.i64 <= right.i64); break;
                case TOKEN_GTEQ:  val.i64 = (left.i64 >= right.i64); break;
                default: break;
            }
            break;
        }

        case AST_TERNARY_OP: {
            HDValue condition =
                EvalExpression(node->as.ternary_op.condition, env);
            if (HDTruthy(condition)) {
                return EvalExpression(node->as.ternary_op.true_expr, env);
            }
            return EvalExpression(node->as.ternary_op.false_expr, env);
        }

        case AST_ARRAY_LITERAL: {
            HDValue array;
            array.type = VAL_ARRAY;
            array.array_len = node->as.array_literal.element_count;
            array.elements = (HDValue*)malloc(
                sizeof(HDValue) * node->as.array_literal.element_count);
            for (int i = 0; i < node->as.array_literal.element_count; i++) {
                array.elements[i] = EvalExpression(
                    node->as.array_literal.elements[i], env);
            }
            return array;
        }

        default:
            // For statements accidentally treated as expressions
            break;
    }
    return val;
}

HDValue EvalNode(ASTNode* node, Environment* env) {
    HDValue val;
    val.type = VAL_INT;
    val.i64 = 0;

    if (node == NULL) return val;

    switch (node->type) {
        case AST_VAR_DECL: {
            if (node->as.variable_decl.initializer != NULL) {
                val = EvalExpression(node->as.variable_decl.initializer, env);
            } else {
                val.type = VAL_INT;
                val.i64 = 0;
            }
            EnvSet(env, node->as.variable_decl.name,
                   node->as.variable_decl.name_length, val);
            break;
        }

        case AST_ASSIGN: {
            // Evaluate right side
            val = EvalExpression(node->as.assignment.value, env);
            // Assign to existing variable
            EnvSet(env, node->as.assignment.name,
                   node->as.assignment.name_length, val);
            break;
        }

        case AST_BLOCK: {
            for (int i = 0; i < node->as.block.statement_count; i++) {
                EvalNode(node->as.block.statements[i], env);
            }
            break;
        }

        case AST_IF: {
            HDValue cond = EvalExpression(node->as.if_statement.condition, env);
            if (cond.i64 != 0) {
                EvalNode(node->as.if_statement.then_branch, env);
            } else if (node->as.if_statement.else_branch != NULL) {
                EvalNode(node->as.if_statement.else_branch, env);
            }
            break;
        }

        case AST_FOREACH: {
            // Evaluate the array we are looping over
            HDValue array = EvalExpression(
                node->as.foreach_statement.array_expression, env);

            if (array.type != VAL_ARRAY) {
                printf("Error: foreach expects an array.\n");
                break;
            }

            for (int i = 0; i < array.array_len; i++) {
                EnvSet(env, node->as.foreach_statement.variable_name,
                       node->as.foreach_statement.variable_name_length,
                       array.elements[i]);

                // Execute the body of the loop
                EvalNode(node->as.foreach_statement.body, env);
            }
            break;
        }

        case AST_CALL: {
            // Built-in Print function
            if (node->as.call.callee_name_length == 5 &&
                strncmp(node->as.call.callee_name, "Print", 5) == 0) {
                for (int i = 0; i < node->as.call.argument_count; i++) {
                    HDValue arg = EvalExpression(
                        node->as.call.arguments[i], env);
                    if (arg.type == VAL_STRING) {
                        // Process escape sequences like \n
                        for (int j = 0; j < arg.str_len; j++) {
                            if (arg.str[j] == '\\') {
                                // Look at the next character
                                if (j + 1 < arg.str_len) {
                                    char next = arg.str[j + 1];
                                    if (next == 'n') {
                                        printf("\n"); // Print actual newline
                                        j++; // Skip the 'n'
                                    } else if (next == '\\') {
                                        printf("\\"); // Print backslash
                                        j++;
                                    } else if (next == '"') {
                                        printf("\""); // Print quote
                                        j++;
                                    } else {
                                        printf("%c", arg.str[j]); // Unknown, print as is
                                    }
                                }
                            } else {
                                printf("%c", arg.str[j]);
                            }
                        }
                    } else if (arg.type == VAL_FLOAT) {
                        HDPrintDouble(arg.f64);
                    } else {
                        printf("%lld", arg.i64);
                    }
                }
            } else {
                printf("Error: Unknown function '%.*s'\n",
                       node->as.call.callee_name_length,
                       node->as.call.callee_name);
            }
            break;
        }

        case AST_INDEX_ASSIGN: {
            if (node->as.index_assignment.target == NULL ||
                node->as.index_assignment.target->type != AST_VAR_REF) {
                printf("Error: indexed assignment needs a named array.\n");
                break;
            }

            ASTNode* target = node->as.index_assignment.target;
            HDValue* array = EnvGet(
                env, target->as.variable_ref.name,
                (size_t)target->as.variable_ref.name_length);
            if (array == NULL || array->type != VAL_ARRAY) {
                printf("Error: indexed assignment target is not an array.\n");
                break;
            }

            HDValue index = EvalExpression(node->as.index_assignment.index, env);
            if (index.type != VAL_INT || index.i64 < 0 ||
                index.i64 >= array->array_len) {
                printf("Error: array index out of range.\n");
                break;
            }

            array->elements[index.i64] = EvalExpression(
                node->as.index_assignment.value, env);
            break;
        }

        /* Marks a point the tree walker has no way to return to: it runs
         * the AST by recursion, so a jump would have to unwind out of every
         * enclosing node and then find its way back in. The two paths that
         * difftest holds to each other , the VM and the C backend , both
         * implement goto; this one predates them, like its missing FFI. */
        case AST_LABEL:
            break;

        case AST_GOTO:
            printf("Runtime error: goto is not supported by the tree walker; "
                   "run without --interpret.\n");
            exit(1);

        default:
            // Try evaluating as an expression (e.g. a standalone "5 + 5;")
            EvalExpression(node, env);
            break;
    }
    return val;
}
