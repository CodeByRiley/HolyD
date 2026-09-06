#include "parser.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static ASTNode* ParseExpression(Parser* parser);
static ASTNode* ParseStatement(Parser* parser);
static ASTNode* ParseBlock(Parser* parser);
static ASTNode* ParseAssignmentExpression(Parser* parser);
static TypeSyntax* ParseTypeSyntax(Parser* parser);
static int ParseParameterList(Parser* parser, int require_names,
                              ParameterSyntax** parameters_out,
                              int* parameter_count_out);

static ASTNode* span_token(ASTNode* node, Token token) {
    return ASTSetSpan(node, token.span);
}

static ASTNode* span_between(ASTNode* node, HDSourceSpan first,
                             HDSourceSpan last) {
    return ASTSetSpan(node, HDSourceSpanCover(first, last));
}

static int append_node(ASTNode*** items, int* count, int* capacity, ASTNode* node) {
    if (*count >= *capacity) {
        int new_capacity = *capacity ? *capacity * 2 : 8;
        ASTNode** grown = (ASTNode**)malloc(sizeof(ASTNode*) * new_capacity);
        if (grown == NULL) {
            printf("Parse error: out of memory while growing node list.\n");
            return 0;
        }

        for (int i = 0; i < *count; i++) {
            grown[i] = (*items)[i];
        }
        free(*items);
        *items = grown;
        *capacity = new_capacity;
    }

    (*items)[(*count)++] = node;
    return 1;
}

static int append_parameter(ParameterSyntax** items, int* count, int* capacity,
                            ParameterSyntax parameter) {
    if (*count >= *capacity) {
        int new_capacity = *capacity ? *capacity * 2 : 8;
        ParameterSyntax* grown = (ParameterSyntax*)malloc(
            sizeof(ParameterSyntax) * new_capacity);
        if (grown == NULL) {
            printf("Parse error: out of memory while growing parameter list.\n");
            return 0;
        }

        for (int i = 0; i < *count; i++) {
            grown[i] = (*items)[i];
        }
        free(*items);
        *items = grown;
        *capacity = new_capacity;
    }

    (*items)[(*count)++] = parameter;
    return 1;
}

static void advance(Parser* parser) {
    parser->previous = parser->current;
    parser->current = LexerNextToken(&parser->lexer);
}

static int check(Parser* parser, TokenType type) {
    return parser->current.type == type;
}

static int match(Parser* parser, TokenType type) {
    if (check(parser, type)) {
        advance(parser);
        return 1;
    }
    return 0;
}

static void skip_terminators(Parser* parser) {
    while (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
        advance(parser);
    }
}

static int is_type_token(TokenType type) {
    switch (type) {
        case TOKEN_U0:
        case TOKEN_I8:
        case TOKEN_U8:
        case TOKEN_I16:
        case TOKEN_U16:
        case TOKEN_I32:
        case TOKEN_U32:
        case TOKEN_I64:
        case TOKEN_U64:
        case TOKEN_F64:
        case TOKEN_VOID:
        case TOKEN_INT:
        case TOKEN_UINT:
        case TOKEN_LONG:
        case TOKEN_ULONG:
        case TOKEN_DOUBLE:
        case TOKEN_BOOL:
        case TOKEN_STRING_TYPE:
        case TOKEN_AUTO:
            return 1;
        default:
            return 0;
    }
}

static int is_type_qualifier_token(TokenType type) {
    return type == TOKEN_CONST || type == TOKEN_IMMUTABLE ||
           type == TOKEN_SHARED || type == TOKEN_INOUT;
}

static int is_type_start_token(TokenType type) {
    return is_type_token(type) || type == TOKEN_IDENTIFIER ||
           is_type_qualifier_token(type) || type == TOKEN_TYPEOF;
}

static TypeQualifier qualifier_from_token(TokenType type) {
    switch (type) {
        case TOKEN_IMMUTABLE: return TYPE_QUALIFIER_IMMUTABLE;
        case TOKEN_SHARED: return TYPE_QUALIFIER_SHARED;
        case TOKEN_INOUT: return TYPE_QUALIFIER_INOUT;
        case TOKEN_CONST:
        default: return TYPE_QUALIFIER_CONST;
    }
}

static TypeSyntax* require_type_node(Parser* parser, TypeSyntax* type) {
    if (type == NULL) {
        parser->had_error = 1;
        printf("Parse error: out of memory while building type syntax.\n");
    }
    return type;
}

static TypeSyntax* ParseTypePrimary(Parser* parser) {
    if (is_type_qualifier_token(parser->current.type)) {
        TypeQualifier qualifier = qualifier_from_token(parser->current.type);
        advance(parser);

        TypeSyntax* base_type = NULL;
        if (match(parser, TOKEN_LPAREN)) {
            base_type = ParseTypeSyntax(parser);
            if (!match(parser, TOKEN_RPAREN)) {
                parser->had_error = 1;
                printf("Parse error: Expected ')' after qualified type on line %zu\n",
                       parser->lexer.line);
                return NULL;
            }
        } else {
            /* D also accepts `const I64`. Only parsing the following primary
             * makes `const I64*` equivalent to `const(I64)*`; parentheses can
             * still express `const(I64*)`. */
            base_type = ParseTypePrimary(parser);
        }

        if (base_type == NULL) return NULL;
        return require_type_node(
            parser, TypeSyntaxNewQualified(qualifier, base_type));
    }

    if (match(parser, TOKEN_TYPEOF)) {
        if (!match(parser, TOKEN_LPAREN)) {
            parser->had_error = 1;
            printf("Parse error: Expected '(' after typeof on line %zu\n",
                   parser->lexer.line);
            return NULL;
        }

        ASTNode* expression = ParseExpression(parser);
        if (expression == NULL || !match(parser, TOKEN_RPAREN)) {
            parser->had_error = 1;
            printf("Parse error: Expected ')' after typeof expression on line %zu\n",
                   parser->lexer.line);
            return NULL;
        }
        return require_type_node(parser, TypeSyntaxNewTypeof(expression));
    }

    Token name = parser->current;
    if (!is_type_token(name.type) && name.type != TOKEN_IDENTIFIER) {
        parser->had_error = 1;
        printf("Parse error: Expected type on line %zu\n", parser->lexer.line);
        return NULL;
    }

    advance(parser);
    return require_type_node(
        parser, TypeSyntaxNewNamed(name.start, name.length));
}

/* `Value[Key]` and `Value[length]` overlap when the brackets contain one
 * identifier. Until name resolution can distinguish a type from a constant,
 * that one ambiguous form remains a static array. Built-in and constructed
 * key types are unambiguous and become associative arrays here. */
static int TryParseAssociativeKey(Parser* parser, TypeSyntax** key_out) {
    if (!is_type_start_token(parser->current.type)) return 0;

    Parser saved = *parser;
    TokenType first = parser->current.type;
    TypeSyntax* candidate = ParseTypeSyntax(parser);
    int simple_identifier = candidate != NULL &&
                            candidate->kind == TYPE_SYNTAX_NAMED &&
                            first == TOKEN_IDENTIFIER;

    if (candidate != NULL && !simple_identifier &&
        check(parser, TOKEN_RBRACKET)) {
        *key_out = candidate;
        return 1;
    }

    *parser = saved;
    return 0;
}

static TypeSyntax* ParseTypeSyntax(Parser* parser) {
    TypeSyntax* type = ParseTypePrimary(parser);
    if (type == NULL) return NULL;

    for (;;) {
        if (match(parser, TOKEN_STAR)) {
            type = require_type_node(parser, TypeSyntaxNewPointer(type));
        } else if (match(parser, TOKEN_LBRACKET)) {
            if (match(parser, TOKEN_RBRACKET)) {
                type = require_type_node(
                    parser, TypeSyntaxNewDynamicArray(type));
            } else {
                TypeSyntax* key_type = NULL;
                if (TryParseAssociativeKey(parser, &key_type)) {
                    advance(parser); /* closing ']' */
                    type = require_type_node(
                        parser, TypeSyntaxNewAssocArray(type, key_type));
                } else {
                    ASTNode* length = ParseExpression(parser);
                    if (length == NULL || !match(parser, TOKEN_RBRACKET)) {
                        parser->had_error = 1;
                        printf("Parse error: Expected ']' after array length on line %zu\n",
                               parser->lexer.line);
                        return NULL;
                    }
                    type = require_type_node(
                        parser, TypeSyntaxNewStaticArray(type, length));
                }
            }
        } else if (match(parser, TOKEN_FUNCTION) ||
                   match(parser, TOKEN_DELEGATE)) {
            TokenType callable_kind = parser->previous.type;
            if (!match(parser, TOKEN_LPAREN)) {
                parser->had_error = 1;
                printf("Parse error: Expected '(' after %s on line %zu\n",
                       callable_kind == TOKEN_FUNCTION ? "function" : "delegate",
                       parser->lexer.line);
                return NULL;
            }

            ParameterSyntax* parameters = NULL;
            int parameter_count = 0;
            if (!ParseParameterList(parser, 0, &parameters,
                                    &parameter_count)) {
                return NULL;
            }

            if (callable_kind == TOKEN_FUNCTION) {
                type = require_type_node(
                    parser, TypeSyntaxNewFunction(type, parameters,
                                                  parameter_count));
            } else {
                type = require_type_node(
                    parser, TypeSyntaxNewDelegate(type, parameters,
                                                  parameter_count));
            }
        } else {
            break;
        }

        if (type == NULL) return NULL;
    }

    return type;
}

static int is_parameter_storage_token(TokenType type) {
    return type == TOKEN_REF || type == TOKEN_OUT || type == TOKEN_LAZY ||
           type == TOKEN_SCOPE;
}

static ParameterStorage parameter_storage_from_token(TokenType type) {
    switch (type) {
        case TOKEN_REF: return PARAMETER_STORAGE_REF;
        case TOKEN_OUT: return PARAMETER_STORAGE_OUT;
        case TOKEN_LAZY: return PARAMETER_STORAGE_LAZY;
        case TOKEN_SCOPE: return PARAMETER_STORAGE_SCOPE;
        default: return PARAMETER_STORAGE_NONE;
    }
}

/* Function/delegate types may omit parameter names. Named function bodies
 * keep requiring them because the current VM binds arguments by name. */
static int ParseParameterList(Parser* parser, int require_names,
                              ParameterSyntax** parameters_out,
                              int* parameter_count_out) {
    ParameterSyntax* parameters = NULL;
    int parameter_count = 0;
    int parameter_capacity = 0;

    while (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_EOF)) {
        ParameterStorage storage = PARAMETER_STORAGE_NONE;
        while (is_parameter_storage_token(parser->current.type)) {
            ParameterStorage next =
                parameter_storage_from_token(parser->current.type);
            if ((storage & next) != 0) {
                parser->had_error = 1;
                printf("Parse error: duplicate parameter storage class on line %zu\n",
                       parser->lexer.line);
                return 0;
            }

            if (next != PARAMETER_STORAGE_SCOPE &&
                (storage & (PARAMETER_STORAGE_REF | PARAMETER_STORAGE_OUT |
                            PARAMETER_STORAGE_LAZY)) != 0) {
                parser->had_error = 1;
                printf("Parse error: ref, out, and lazy are mutually exclusive on line %zu\n",
                       parser->lexer.line);
                return 0;
            }
            storage = (ParameterStorage)(storage | next);
            advance(parser);
        }

        if (check(parser, TOKEN_DOTDOTDOT)) {
            parser->had_error = 1;
            printf("Parse error: raw variadics need an explicit foreign ABI on line %zu\n",
                   parser->lexer.line);
            return 0;
        }

        if (!is_type_start_token(parser->current.type)) {
            parser->had_error = 1;
            printf("Parse error: Expected parameter type on line %zu\n",
                   parser->lexer.line);
            return 0;
        }

        ParameterSyntax parameter;
        parameter.type = ParseTypeSyntax(parser);
        parameter.name = NULL;
        parameter.name_length = 0;
        parameter.default_value = NULL;
        parameter.storage = storage;
        parameter.is_variadic = 0;
        if (parameter.type == NULL) return 0;

        if (match(parser, TOKEN_IDENTIFIER)) {
            parameter.name = parser->previous.start;
            parameter.name_length = parser->previous.length;
        } else if (require_names) {
            parser->had_error = 1;
            printf("Parse error: Expected parameter name on line %zu\n",
                   parser->lexer.line);
            return 0;
        }

        if (match(parser, TOKEN_ASSIGN)) {
            parameter.default_value = ParseExpression(parser);
            if (parameter.default_value == NULL) return 0;
        }

        if (match(parser, TOKEN_DOTDOTDOT)) {
            parameter.is_variadic = 1;
        }

        if (!append_parameter(&parameters, &parameter_count,
                              &parameter_capacity, parameter)) {
            parser->had_error = 1;
            return 0;
        }

        if (!match(parser, TOKEN_COMMA)) break;
        if (parameter.is_variadic) {
            parser->had_error = 1;
            printf("Parse error: variadic parameter must be last on line %zu\n",
                   parser->lexer.line);
            return 0;
        }
    }

    if (!match(parser, TOKEN_RPAREN)) {
        parser->had_error = 1;
        printf("Parse error: Expected ')' after parameter list on line %zu\n",
               parser->lexer.line);
        return 0;
    }

    *parameters_out = parameters;
    *parameter_count_out = parameter_count;
    return 1;
}

/* A statement beginning with a user-defined type is ambiguous with an
 * expression beginning with an identifier. Parse just the type syntax and
 * keep it only when another identifier follows as the declared name. A later
 * symbol-resolution pass can make the few genuinely ambiguous cases (such as
 * `Foo * value`) semantic rather than syntactic. */
static TypeSyntax* ParseOptionalDeclarationType(Parser* parser) {
    if (!is_type_start_token(parser->current.type)) return NULL;
    if (parser->current.type != TOKEN_IDENTIFIER) {
        return ParseTypeSyntax(parser);
    }

    Parser saved = *parser;
    TypeSyntax* type = ParseTypeSyntax(parser);
    if (type != NULL && check(parser, TOKEN_IDENTIFIER)) {
        return type;
    }

    *parser = saved;
    return NULL;
}

static ASTNode* ParseNumber(Parser* parser) {
    // Convert string to integer manually (no strtol in freestanding)
    long long val = 0;
    int base = 10;
    int i = 0;
    if (parser->previous.length > 2 &&
        parser->previous.start[0] == '0' &&
        (parser->previous.start[1] == 'x' || parser->previous.start[1] == 'X')) {
        base = 16;
        i = 2;
    }

    for (; i < (int)parser->previous.length; i++) {
        char c = parser->previous.start[i];
        int digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        val = val * base + digit;
    }
    return span_token(ASTNewNumber(val), parser->previous);
}

static ASTNode* ParseFloat(Parser* parser) {
    /* No strtod in freestanding. The lexer already guaranteed the shape
     * `digits . digits`, so a two-pass scan over the token is enough. */
    const char* text = parser->previous.start;
    int length = parser->previous.length;

    double val = 0.0;
    int i = 0;
    for (; i < length && text[i] != '.'; i++) {
        val = val * 10.0 + (double)(text[i] - '0');
    }

    /* Accumulate the fraction as an integer and divide once. Scaling by 0.1
     * per digit instead would fold a rounding error in at every step, so
     * even 3.75 could miss the exactly-representable value. */
    long long frac = 0;
    double divisor = 1.0;
    for (i++; i < length && divisor < 1.0e18; i++) {
        frac = frac * 10 + (text[i] - '0');
        divisor *= 10.0;
    }
    val += (double)frac / divisor;

    return span_token(ASTNewFloat(val), parser->previous);
}

static ASTNode* ParseString(Parser* parser) {
    // Strip the quotes
    const char* str = parser->previous.start + 1;
    int len = parser->previous.length - 2;
    return span_token(ASTNewString(str, len), parser->previous);
}

static ASTNode* ParseCall(Parser* parser) {
    Token name_token = parser->previous;
    const char* name = parser->previous.start;
    int len = parser->previous.length;

    advance(parser); // Consume '('

    ASTNode** args = NULL;
    int arg_count = 0;
    int arg_capacity = 0;

    if (!check(parser, TOKEN_RPAREN)) {
        do {
            if (!append_node(&args, &arg_count, &arg_capacity,
                             ParseExpression(parser))) {
                break;
            }
        } while (match(parser, TOKEN_COMMA));
    }

    int closed = match(parser, TOKEN_RPAREN); // Consume ')'
    ASTNode* call = ASTNewCall(name, len, args, arg_count);
    HDSourceSpan end = closed ? parser->previous.span
                              : arg_count > 0 ? args[arg_count - 1]->span
                                              : name_token.span;
    return span_between(call, name_token.span, end);
}

static ASTNode* ParsePrimary(Parser* parser) {
    /* These lex now, but no production consumes them. Name the missing
     * feature rather than reporting a bare unexpected token. */
    if (check(parser, TOKEN_DOTDOT) || check(parser, TOKEN_DOTDOTDOT)) {
        int is_range = check(parser, TOKEN_DOTDOT);
        parser->had_error = 1;
        printf("Parse error: '%s' is not implemented on line %zu "
               "(slices, case ranges and variadics are unbuilt)\n",
               is_range ? ".." : "...", parser->lexer.line);
        advance(parser);
        return NULL;
    }

    if (match(parser, TOKEN_NUMBER)) return ParseNumber(parser);
    if (match(parser, TOKEN_FLOAT)) return ParseFloat(parser);
    if (match(parser, TOKEN_STRING)) return ParseString(parser);
    if (match(parser, TOKEN_TRUE))
        return span_token(ASTNewBoolean(1), parser->previous);
    if (match(parser, TOKEN_FALSE))
        return span_token(ASTNewBoolean(0), parser->previous);

    if (match(parser, TOKEN_LPAREN)) {
        Token opening = parser->previous;
        ASTNode* expr = ParseExpression(parser);
        if (match(parser, TOKEN_RPAREN)) {
            if (expr != NULL)
                expr->span = HDSourceSpanCover(opening.span,
                                               parser->previous.span);
        } else {
            parser->had_error = 1;
            printf("Parse error: Expected ')' on line %zu\n", parser->lexer.line);
        }
        return expr;
    }

    if (match(parser, TOKEN_IDENTIFIER)) {
        Token name = parser->previous;
        if (check(parser, TOKEN_LPAREN)) {
            return ParseCall(parser);
        }
        return span_token(ASTNewVarRef(name.start, name.length), name);
    }

    // Parse Array Literal: [1, 2, 3]
    if (match(parser, TOKEN_LBRACKET)) {
        Token opening = parser->previous;
        ASTNode** elements = NULL;
        int count = 0;
        int capacity = 0;

        if (!check(parser, TOKEN_RBRACKET)) {
            do {
                if (!append_node(&elements, &count, &capacity,
                                 ParseExpression(parser))) {
                    break;
                }
            } while (match(parser, TOKEN_COMMA));
        }
        int closed = match(parser, TOKEN_RBRACKET); // Consume ']'

        ASTNode* array = ASTNewArrayLiteral(elements, count);
        HDSourceSpan end = closed ? parser->previous.span
                                  : count > 0 ? elements[count - 1]->span
                                              : opening.span;
        return span_between(array, opening.span, end);
    }

    parser->had_error = 1;
    return NULL; // No production matched the current token.
}

static int token_is_identifier_text(Token token, const char* text) {
    int len = 0;
    while (text[len]) len++;
    return token.length == (size_t)len &&
           strncmp(token.start, text, (size_t)len) == 0;
}

static ASTNode* ParsePostfix(Parser* parser) {
    ASTNode* node = ParsePrimary(parser);

    while (node != NULL) {
        if (match(parser, TOKEN_LBRACKET)) {
            HDSourceSpan start = node->span;
            ASTNode* index = ParseExpression(parser);
            if (check(parser, TOKEN_DOTDOT)) {
                parser->had_error = 1;
                printf("Parse error: array slices are not implemented on line %zu\n",
                       parser->lexer.line);
                break;
            }
            if (!match(parser, TOKEN_RBRACKET)) {
                parser->had_error = 1;
                printf("Parse error: Expected ']' on line %zu\n", parser->lexer.line);
            }
            node = ASTNewIndex(node, index);
            if (parser->previous.type == TOKEN_RBRACKET)
                node->span = HDSourceSpanCover(start, parser->previous.span);
        } else if (match(parser, TOKEN_DOT)) {
            if (!match(parser, TOKEN_IDENTIFIER)) {
                parser->had_error = 1;
                printf("Parse error: Expected property name after '.' on line %zu\n", parser->lexer.line);
                break;
            }
            if (token_is_identifier_text(parser->previous, "length")) {
                HDSourceSpan start = node->span;
                HDSourceSpan end = parser->previous.span;
                node = ASTNewArrayLenExpr(node);
                node->span = HDSourceSpanCover(start, end);
            } else {
                parser->had_error = 1;
                printf("Parse error: Unknown property '%.*s' on line %zu\n",
                       (int)parser->previous.length, parser->previous.start,
                       parser->lexer.line);
            }
        } else {
            break;
        }
    }

    return node;
}

/* Binary precedence, loosest binding first. Each rung parses the tighter
 * one on both sides, so `i < n + 1` groups as `i < (n + 1)` rather than
 * folding to `(i < n) + 1`.
 *
 *   ParseExpression   ||
 *   ParseLogicalOr    &&
 *   ParseLogicalAnd   |
 *   ParseBitOr        ^
 *   ParseBitXor       &
 *   ParseBitAnd       ==  !=
 *   ParseEquality     <  >  <=  >=
 *   ParseComparison   <<  >>  >>>
 *   ParseShift        +  -  ~
 *   ParseAdditive     *  /  %
 *   ParseTerm         unary !  -
 *   ParseUnary        ^^
 *   ParsePower        postfix
 *
 * This follows D except that equality and relational sit on separate rungs
 * here, as in C; D puts them on one non-associative level, so `a < b < c`
 * is an error there and chains here. Worth revisiting with the rest of the
 * D alignment rather than on its own.
 */
static ASTNode* ParseUnary(Parser* parser);

/* ^^ binds tighter than unary minus and is right-associative, both as in D:
 * -2 ^^ 2 is -(2 ^^ 2), and 2 ^^ 3 ^^ 2 is 2 ^^ (3 ^^ 2). Recursing into
 * ParseUnary on the right gives both. */
static ASTNode* ParsePower(Parser* parser) {
    ASTNode* node = ParsePostfix(parser);
    if (check(parser, TOKEN_POW)) {
        advance(parser);
        return ASTNewBinaryOp(TOKEN_POW, node, ParseUnary(parser));
    }
    return node;
}

static ASTNode* ParseUnary(Parser* parser) {
    if (check(parser, TOKEN_BANG) || check(parser, TOKEN_MINUS)) {
        Token operator_token = parser->current;
        TokenType op = parser->current.type;
        advance(parser);
        ASTNode* operand = ParseUnary(parser);
        ASTNode* unary = ASTNewUnaryOp(op, operand);
        if (operand != NULL)
            unary->span = HDSourceSpanCover(operator_token.span, operand->span);
        return unary;
    }
    return ParsePower(parser);
}

static ASTNode* ParseTerm(Parser* parser) {
    ASTNode* node = ParseUnary(parser);
    while (check(parser, TOKEN_STAR) || check(parser, TOKEN_SLASH) ||
           check(parser, TOKEN_PERCENT)) {
        TokenType op = parser->current.type;
        advance(parser);
        node = ASTNewBinaryOp(op, node, ParseUnary(parser));
    }
    return node;
}

static ASTNode* ParseAdditive(Parser* parser) {
    ASTNode* node = ParseTerm(parser);
    while (check(parser, TOKEN_PLUS) || check(parser, TOKEN_MINUS) ||
           check(parser, TOKEN_TILDE)) {
        TokenType op = parser->current.type;
        advance(parser);
        node = ASTNewBinaryOp(op, node, ParseTerm(parser));
    }
    return node;
}

static ASTNode* ParseShift(Parser* parser) {
    ASTNode* node = ParseAdditive(parser);
    while (check(parser, TOKEN_SHL) || check(parser, TOKEN_SHR) ||
           check(parser, TOKEN_USHR)) {
        TokenType op = parser->current.type;
        advance(parser);
        node = ASTNewBinaryOp(op, node, ParseAdditive(parser));
    }
    return node;
}

static ASTNode* ParseComparison(Parser* parser) {
    ASTNode* node = ParseShift(parser);
    while (check(parser, TOKEN_LT) || check(parser, TOKEN_GT) ||
           check(parser, TOKEN_LTEQ) || check(parser, TOKEN_GTEQ)) {
        TokenType op = parser->current.type;
        advance(parser);
        node = ASTNewBinaryOp(op, node, ParseShift(parser));
    }
    return node;
}

static ASTNode* ParseEquality(Parser* parser) {
    ASTNode* node = ParseComparison(parser);
    while (check(parser, TOKEN_EQEQ) || check(parser, TOKEN_NEQ)) {
        TokenType op = parser->current.type;
        advance(parser);
        node = ASTNewBinaryOp(op, node, ParseComparison(parser));
    }
    return node;
}

/* '&' is TOKEN_AMPERSAND: the lexer emits one token for it whether it was
 * meant as bitwise and or as address-of, and only the former parses. */
static ASTNode* ParseBitAnd(Parser* parser) {
    ASTNode* node = ParseEquality(parser);
    while (check(parser, TOKEN_AMPERSAND)) {
        advance(parser);
        node = ASTNewBinaryOp(TOKEN_AMPERSAND, node, ParseEquality(parser));
    }
    return node;
}

static ASTNode* ParseBitXor(Parser* parser) {
    ASTNode* node = ParseBitAnd(parser);
    while (check(parser, TOKEN_XOR)) {
        advance(parser);
        node = ASTNewBinaryOp(TOKEN_XOR, node, ParseBitAnd(parser));
    }
    return node;
}

static ASTNode* ParseBitOr(Parser* parser) {
    ASTNode* node = ParseBitXor(parser);
    while (check(parser, TOKEN_OR)) {
        advance(parser);
        node = ASTNewBinaryOp(TOKEN_OR, node, ParseBitXor(parser));
    }
    return node;
}

static ASTNode* ParseLogicalAnd(Parser* parser) {
    ASTNode* node = ParseBitOr(parser);
    while (check(parser, TOKEN_ANDAND)) {
        advance(parser);
        node = ASTNewBinaryOp(TOKEN_ANDAND, node, ParseBitOr(parser));
    }
    return node;
}

static ASTNode* ParseConditional(Parser* parser) {
    ASTNode* node = ParseLogicalAnd(parser);
    while (check(parser, TOKEN_OROR)) {
        advance(parser);
        node = ASTNewBinaryOp(TOKEN_OROR, node, ParseLogicalAnd(parser));
    }

    if (match(parser, TOKEN_QUESTION)) {
        ASTNode* true_expr = ParseExpression(parser);
        if (!match(parser, TOKEN_COLON)) {
            parser->had_error = 1;
            printf("Parse error: Expected ':' in conditional expression on line %zu\n",
                   parser->lexer.line);
            return NULL;
        }

        /* Recurse at the same precedence for the false arm, making the
         * operator right-associative: a ? b : c ? d : e groups as
         * a ? b : (c ? d : e). */
        ASTNode* false_expr = ParseConditional(parser);
        return ASTNewTernaryOp(node, true_expr, false_expr);
    }

    return node;
}

static ASTNode* ParseExpression(Parser* parser) {
    return ParseConditional(parser);
}

/* `a += b` is `a = a + b`. The table is the only place the pairing between
 * a compound token and its binary operator lives; TOKEN_UNKNOWN means the
 * token was not a compound assignment at all. */
static TokenType compound_binary_op(TokenType op) {
    switch (op) {
        case TOKEN_PLUS_ASSIGN:    return TOKEN_PLUS;
        case TOKEN_MINUS_ASSIGN:   return TOKEN_MINUS;
        case TOKEN_STAR_ASSIGN:    return TOKEN_STAR;
        case TOKEN_SLASH_ASSIGN:   return TOKEN_SLASH;
        case TOKEN_PERCENT_ASSIGN: return TOKEN_PERCENT;
        case TOKEN_POW_ASSIGN:     return TOKEN_POW;
        case TOKEN_TILDE_ASSIGN:   return TOKEN_TILDE;
        case TOKEN_AND_ASSIGN:     return TOKEN_AMPERSAND;
        case TOKEN_OR_ASSIGN:      return TOKEN_OR;
        case TOKEN_XOR_ASSIGN:     return TOKEN_XOR;
        case TOKEN_SHL_ASSIGN:     return TOKEN_SHL;
        case TOKEN_SHR_ASSIGN:     return TOKEN_SHR;
        case TOKEN_USHR_ASSIGN:    return TOKEN_USHR;
        default:                   return TOKEN_UNKNOWN;
    }
}

static ASTNode* make_inc_dec(ASTNode* target, TokenType op) {
    TokenType bin_op = op == TOKEN_PLUSPLUS ? TOKEN_PLUS : TOKEN_MINUS;
    ASTNode* reference = ASTSetSpan(
        ASTNewVarRef(target->as.variable_ref.name,
                     target->as.variable_ref.name_length), target->span);
    ASTNode* one = ASTSetSpan(ASTNewNumber(1), target->span);
    ASTNode* value = ASTNewBinaryOp(bin_op, reference, one);
    return ASTSetSpan(ASTNewAssign(target->as.variable_ref.name,
                                  target->as.variable_ref.name_length, value),
                      target->span);
}

static ASTNode* ParseAssignmentExpression(Parser* parser) {
    ASTNode* expr = ParseExpression(parser);
    if (expr != NULL && expr->type == AST_VAR_REF) {
        if (match(parser, TOKEN_ASSIGN)) {
            ASTNode* value = ParseExpression(parser);
            ASTNode* assignment = ASTNewAssign(expr->as.variable_ref.name,
                                               expr->as.variable_ref.name_length,
                                               value);
            if (value != NULL)
                assignment->span = HDSourceSpanCover(expr->span, value->span);
            return assignment;
        }
        if (match(parser, TOKEN_PLUSPLUS)) {
            return make_inc_dec(expr, TOKEN_PLUSPLUS);
        }
        if (match(parser, TOKEN_MINUSMINUS)) {
            return make_inc_dec(expr, TOKEN_MINUSMINUS);
        }

        TokenType compound = compound_binary_op(parser->current.type);
        if (compound != TOKEN_UNKNOWN) {
            advance(parser);
            ASTNode* reference = ASTSetSpan(
                ASTNewVarRef(expr->as.variable_ref.name,
                             expr->as.variable_ref.name_length), expr->span);
            ASTNode* right = ParseExpression(parser);
            ASTNode* value = ASTNewBinaryOp(compound, reference, right);
            ASTNode* assignment = ASTNewAssign(
                expr->as.variable_ref.name, expr->as.variable_ref.name_length,
                value);
            if (right != NULL)
                assignment->span = HDSourceSpanCover(expr->span, right->span);
            return assignment;
        }
    }

    if (expr != NULL && expr->type == AST_INDEX) {
        if (match(parser, TOKEN_ASSIGN)) {
            return ASTNewIndexAssign(expr->as.index_expr.target,
                                     expr->as.index_expr.index,
                                     ParseExpression(parser));
        }
        /* arr[i]++ desugars to arr[i] = arr[i] + 1, which evaluates the
         * index twice. Safe for the plain expressions the parser accepts
         * today; revisit if indices ever gain side effects. */
        if (match(parser, TOKEN_PLUSPLUS) || match(parser, TOKEN_MINUSMINUS)) {
            TokenType bin_op = parser->previous.type == TOKEN_PLUSPLUS
                                   ? TOKEN_PLUS : TOKEN_MINUS;
            return ASTNewIndexAssign(expr->as.index_expr.target,
                                     expr->as.index_expr.index,
                                     ASTNewBinaryOp(bin_op, expr, ASTNewNumber(1)));
        }

        /* arr[i] += v evaluates the index twice, for the same reason and
         * with the same caveat as arr[i]++ above. */
        TokenType compound = compound_binary_op(parser->current.type);
        if (compound != TOKEN_UNKNOWN) {
            advance(parser);
            return ASTNewIndexAssign(expr->as.index_expr.target,
                                     expr->as.index_expr.index,
                                     ASTNewBinaryOp(compound, expr,
                                                    ParseExpression(parser)));
        }
    }
    return expr;
}

static ASTNode* ParseVarDecl(Parser* parser, TypeSyntax* type, Token name) {
    ASTNode* init = NULL;
    if (match(parser, TOKEN_ASSIGN)) {
        init = ParseExpression(parser);
    }
    int terminated = 0;
    if (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
        advance(parser);
        terminated = 1;
    }
    ASTNode* declaration = ASTNewVarDecl(type, name.start, name.length, init);
    HDSourceSpan end = terminated ? parser->previous.span
                                  : init != NULL ? init->span : name.span;
    return span_between(declaration, name.span, end);
}

static ASTNode* ParseIfStatement(Parser* parser) {
    HDSourceSpan start = parser->previous.span;
    match(parser, TOKEN_LPAREN); // Consume '('
    ASTNode* cond = ParseExpression(parser);
    match(parser, TOKEN_RPAREN); // Consume ')'

    ASTNode* then_block = ParseBlock(parser);
    ASTNode* else_block = NULL;
    if (match(parser, TOKEN_ELSE)) {
        else_block = ParseBlock(parser);
    }
    ASTNode* statement = ASTNewIf(cond, then_block, else_block);
    ASTNode* last = else_block != NULL ? else_block : then_block;
    if (last != NULL) statement->span = HDSourceSpanCover(start, last->span);
    return statement;
}

static ASTNode* ParseWhileStatement(Parser* parser) {
    HDSourceSpan start = parser->previous.span;
    match(parser, TOKEN_LPAREN);
    ASTNode* cond = ParseExpression(parser);
    match(parser, TOKEN_RPAREN);

    ASTNode* body = ParseBlock(parser);
    ASTNode* statement = ASTNewWhile(cond, body);
    if (body != NULL) statement->span = HDSourceSpanCover(start, body->span);
    return statement;
}

static ASTNode* ParseForStatement(Parser* parser) {
    HDSourceSpan start = parser->previous.span;
    match(parser, TOKEN_LPAREN);

    ASTNode* init = NULL;
    if (!match(parser, TOKEN_SEMICOLON)) {
        HDSourceSpan declaration_start = parser->current.span;
        TypeSyntax* type = ParseOptionalDeclarationType(parser);
        if (type != NULL) {
            if (!check(parser, TOKEN_IDENTIFIER)) {
                parser->had_error = 1;
                printf("Parse error: Expected variable name in for initializer on line %zu\n", parser->lexer.line);
                return NULL;
            }
            Token name_token = parser->current;
            advance(parser);
            init = ParseVarDecl(parser, type, name_token);
            if (init != NULL)
                init->span = HDSourceSpanCover(declaration_start, init->span);
        } else {
            init = ParseAssignmentExpression(parser);
            match(parser, TOKEN_SEMICOLON);
        }
    }

    ASTNode* cond = NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
        cond = ParseExpression(parser);
    }
    match(parser, TOKEN_SEMICOLON);

    ASTNode* inc = NULL;
    if (!check(parser, TOKEN_RPAREN)) {
        inc = ParseAssignmentExpression(parser);
    }
    match(parser, TOKEN_RPAREN);

    ASTNode* body = ParseBlock(parser);
    ASTNode* statement = ASTNewFor(init, cond, inc, body);
    if (body != NULL) statement->span = HDSourceSpanCover(start, body->span);
    return statement;
}

static ASTNode* ParseForeach(Parser* parser) {
    HDSourceSpan start = parser->previous.span;
    match(parser, TOKEN_LPAREN);

    TypeSyntax* first_type = ParseOptionalDeclarationType(parser);

    if (!match(parser, TOKEN_IDENTIFIER)) {
        parser->had_error = 1;
        printf("Parse error: Expected foreach variable on line %zu\n", parser->lexer.line);
        return NULL;
    }

    const char* first_name = parser->previous.start;
    int first_len = parser->previous.length;
    const char* index_name = NULL;
    int index_len = 0;
    TypeSyntax* index_type = NULL;
    const char* var_name = first_name;
    int var_len = first_len;
    TypeSyntax* variable_type = first_type;

    if (match(parser, TOKEN_COMMA)) {
        index_name = first_name;
        index_len = first_len;
        index_type = first_type;
        variable_type = NULL;

        variable_type = ParseOptionalDeclarationType(parser);
        if (!match(parser, TOKEN_IDENTIFIER)) {
            parser->had_error = 1;
            printf("Parse error: Expected foreach value variable on line %zu\n", parser->lexer.line);
            return NULL;
        }
        var_name = parser->previous.start;
        var_len = parser->previous.length;
    }

    match(parser, TOKEN_SEMICOLON); // Consume ';'
    ASTNode* array_expr = ParseExpression(parser);
    match(parser, TOKEN_RPAREN); // Consume ')'

    ASTNode* body = ParseBlock(parser);
    ASTNode* statement = ASTNewForeach(variable_type, var_name, var_len,
                                       index_type, index_name, index_len,
                                       array_expr, body);
    if (body != NULL) statement->span = HDSourceSpanCover(start, body->span);
    return statement;
}

/* A loop or conditional body. With braces it is a statement list; without
 * them it is exactly one statement, as in C and HolyC.
 *
 * The brace used to be optional here and nothing replaced it, so the loop
 * below ran to '}' or EOF either way: `if (c) Foo();` quietly pulled every
 * following statement into the body. */
static ASTNode* ParseBlock(Parser* parser) {
    if (!check(parser, TOKEN_LBRACE)) {
        if (match(parser, TOKEN_SEMICOLON)) {
            return span_token(ASTNewBlock(NULL, 0), parser->previous);
            // `if (c) ;` , an empty body.
        }

        ASTNode* stmt = ParseStatement(parser);
        if (stmt == NULL) {
            parser->had_error = 1;
            printf("Parse error: Expected a statement or '{' on line %zu\n",
                   parser->lexer.line);
            return ASTNewBlock(NULL, 0);
        }

        ASTNode** only = (ASTNode**)malloc(sizeof(ASTNode*));
        if (only == NULL) {
            parser->had_error = 1;
            printf("Parse error: out of memory while building a block.\n");
            return ASTNewBlock(NULL, 0);
        }

        only[0] = stmt;
        return ASTNewBlock(only, 1);
    }

    Token opening = parser->current;
    advance(parser);

    skip_terminators(parser);    // Skip newlines after '{'

    ASTNode** stmts = NULL;
    int count = 0;
    int capacity = 0;

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        ASTNode* stmt = ParseStatement(parser);
        if (stmt != NULL) {
            if (!append_node(&stmts, &count, &capacity, stmt)) {
                break;
            }
        } else {
            parser->had_error = 1;
            printf("Parse error: Unexpected token '%.*s' on line %zu\n",
                   (int)parser->current.length, parser->current.start,
                   parser->lexer.line);
            advance(parser);
        }
        skip_terminators(parser); // NEW: Skip newlines between statements in block
    }

    int closed = match(parser, TOKEN_RBRACE);
    if (!closed) {
        parser->had_error = 1;
        printf("Parse error: Expected '}' on line %zu\n", parser->lexer.line);
    }

    ASTNode* block = ASTNewBlock(stmts, count);
    HDSourceSpan end = closed ? parser->previous.span
                              : count > 0 ? stmts[count - 1]->span
                                          : opening.span;
    return span_between(block, opening.span, end);
}

static ASTNode* ParseFuncDecl(Parser* parser, TypeSyntax* return_type,
                              Token name) {
    ParameterSyntax* parameters = NULL;
    int parameter_count = 0;
    advance(parser); /* opening '(' */
    if (!ParseParameterList(parser, 1, &parameters, &parameter_count)) {
        return NULL;
    }
    skip_terminators(parser);    // Allow newline before '{'

    ASTNode* body = ParseBlock(parser);
    ASTNode* function = ASTNewFuncDecl(return_type, name.start, name.length,
                                       parameters, parameter_count, body);
    if (body != NULL) function->span = HDSourceSpanCover(name.span, body->span);
    return function;
}

static ASTNode* ParseIgnoredDirective(Parser* parser) {
    HDSourceSpan start = parser->previous.span;
    while (!check(parser, TOKEN_SEMICOLON) &&
           !check(parser, TOKEN_NEWLINE) &&
           !check(parser, TOKEN_EOF)) {
        advance(parser);
    }
    if (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
        advance(parser);
    }
    return span_between(ASTNewBlock(NULL, 0), start, parser->previous.span);
}

/* `.name:` marks a jump target. The leading dot is what makes it a
 * statement the parser can recognise without lookahead: nothing else in the
 * grammar begins with one, so a bare name stays an expression statement. */
static ASTNode* ParseLabel(Parser* parser) {
    HDSourceSpan start = parser->previous.span;
    if (!check(parser, TOKEN_IDENTIFIER)) {
        parser->had_error = 1;
        printf("Parse error: Expected a label name after '.' on line %zu\n",
               parser->lexer.line);
        return NULL;
    }
    Token name = parser->current;
    advance(parser);

    if (!match(parser, TOKEN_COLON)) {
        parser->had_error = 1;
        printf("Parse error: Expected ':' after label '.%.*s' on line %zu\n",
               (int)name.length, name.start, parser->lexer.line);
        return NULL;
    }
    return span_between(ASTNewLabel(name.start, (int)name.length), start,
                        parser->previous.span);
}

/* `goto name;` and `goto .name;` both jump to `.name:`. The dot is what the
 * declaration needs to be recognisable as one; on a jump it is optional,
 * since `goto` has already said what follows is a label. */
static ASTNode* ParseGoto(Parser* parser) {
    HDSourceSpan start = parser->previous.span;
    match(parser, TOKEN_DOT);

    if (!check(parser, TOKEN_IDENTIFIER)) {
        parser->had_error = 1;
        printf("Parse error: Expected a label name after 'goto' on line %zu\n",
               parser->lexer.line);
        return NULL;
    }
    Token target = parser->current;
    advance(parser);

    if (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
        advance(parser);
    }
    return span_between(ASTNewGoto(target.start, (int)target.length), start,
                        parser->previous.span);
}

static ASTNode* ParseStatement(Parser* parser) {
    if (match(parser, TOKEN_MODULE) || match(parser, TOKEN_IMPORT)) {
        return ParseIgnoredDirective(parser);
    }

    if (match(parser, TOKEN_DOT)) return ParseLabel(parser);
    if (match(parser, TOKEN_GOTO)) return ParseGoto(parser);

    HDSourceSpan declaration_start = parser->current.span;
    TypeSyntax* declared_type = ParseOptionalDeclarationType(parser);
    if (declared_type != NULL) {

        if (check(parser, TOKEN_IDENTIFIER)) {
            Token name_token = parser->current;
            advance(parser); // Consume identifier

            // If next token is '(', it's a function!
            if (check(parser, TOKEN_LPAREN)) {
                ASTNode* function =
                    ParseFuncDecl(parser, declared_type, name_token);
                if (function != NULL)
                    function->span = HDSourceSpanCover(declaration_start,
                                                       function->span);
                return function;
            }

            // Otherwise, it's a variable declaration
            ASTNode* declaration =
                ParseVarDecl(parser, declared_type, name_token);
            if (declaration != NULL)
                declaration->span = HDSourceSpanCover(declaration_start,
                                                      declaration->span);
            return declaration;
        }

        parser->had_error = 1;
        printf("Parse error: Expected identifier after type on line %zu\n", parser->lexer.line);
        return NULL;
    }

    if (match(parser, TOKEN_IF)) return ParseIfStatement(parser);
    if (match(parser, TOKEN_WHILE)) return ParseWhileStatement(parser);
    if (match(parser, TOKEN_FOR)) return ParseForStatement(parser);
    if (match(parser, TOKEN_FOREACH)) return ParseForeach(parser);
    if (check(parser, TOKEN_LBRACE)) return ParseBlock(parser);

    if (match(parser, TOKEN_RETURN)) {
        HDSourceSpan start = parser->previous.span;
        ASTNode* expr = NULL;
        if (!check(parser, TOKEN_SEMICOLON) && !check(parser, TOKEN_NEWLINE)) {
            expr = ParseExpression(parser);
        }
        if (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
            advance(parser);
        }
        ASTNode* statement = ASTNewReturn(expr);
        return span_between(statement, start, parser->previous.span);
    }

    // It's an expression statement
    ASTNode* expr = ParseAssignmentExpression(parser);
    if (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
        advance(parser);
    }
    return expr;
}

void ParserInit(Parser* parser, const char* source) {
    memset(&parser->current, 0, sizeof(parser->current));
    memset(&parser->previous, 0, sizeof(parser->previous));
    LexerInit(&parser->lexer, source);
    parser->had_error = 0;
    advance(parser); // Load the first token
}

ASTNode* ParseProgram(Parser* parser) {
    ASTNode** stmts = NULL;
    int count = 0;
    int capacity = 0;

    skip_terminators(parser); // Skip blank lines at the top of the file

    while (!check(parser, TOKEN_EOF)) {
        ASTNode* stmt = ParseStatement(parser);
        if (stmt != NULL) {
            if (!append_node(&stmts, &count, &capacity, stmt)) {
                break;
            }
        } else {
            parser->had_error = 1;
            printf("Parse error: Unexpected token '%.*s' on line %zu\n",
                   (int)parser->current.length, parser->current.start,
                   parser->lexer.line);
            advance(parser);
        }
        skip_terminators(parser); // Skip blank lines between statements
    }

    /* Diagnostics used to print and let parsing continue, so a malformed
     * file still produced an AST and "compiled". Fail the parse instead. */
    if (parser->had_error) {
        return NULL;
    }

    return ASTNewBlock(stmts, count);
}
