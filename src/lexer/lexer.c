#include "lexer.h"
#include <stddef.h>

// --- Freestanding helpers (no libc needed) ---
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static int string_match(const char* a, const char* b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* One source of truth for identifier-shaped tokens. The generated table
 * keeps spelling, length, and TokenType together, so adding a keyword cannot
 * put it in the wrong length arm (the old `struct` entry did exactly that). */
#define HD_KEYWORD_LIST(KW)            \
    KW("U0", U0)                       \
    KW("I8", I8)                       \
    KW("U8", U8)                       \
    KW("I16", I16)                     \
    KW("U16", U16)                     \
    KW("I32", I32)                     \
    KW("U32", U32)                     \
    KW("I64", I64)                     \
    KW("U64", U64)                     \
    KW("F64", F64)                     \
    KW("void", VOID)                   \
    KW("int", INT)                     \
    KW("uint", UINT)                   \
    KW("long", LONG)                   \
    KW("ulong", ULONG)                 \
    KW("double", DOUBLE)               \
    KW("bool", BOOL)                   \
    KW("string", STRING_TYPE)          \
    KW("auto", AUTO)                   \
    KW("foreach", FOREACH)             \
    KW("for", FOR)                     \
    KW("if", IF)                       \
    KW("else", ELSE)                   \
    KW("while", WHILE)                 \
    KW("return", RETURN)               \
    KW("goto", GOTO)                   \
    KW("true", TRUE)                   \
    KW("false", FALSE)                 \
    KW("module", MODULE)               \
    KW("import", IMPORT)               \
    KW("const", CONST)                 \
    KW("immutable", IMMUTABLE)         \
    KW("static", STATIC)               \
    KW("shared", SHARED)               \
    KW("inout", INOUT)                 \
    KW("function", FUNCTION)           \
    KW("class", CLASS)                 \
    KW("struct", STRUCT)               \
    KW("interface", INTERFACE)         \
    KW("enum", ENUM)                   \
    KW("delegate", DELEGATE)           \
    KW("typeof", TYPEOF)               \
    KW("ref", REF)                     \
    KW("out", OUT)                     \
    KW("lazy", LAZY)                   \
    KW("scope", SCOPE)                 \
    KW("abstract", ABSTRACT)           \
    KW("final", FINAL)                 \
    KW("override", OVERRIDE)           \
    KW("namespace", NAMESPACE)         \
    KW("extern", EXTERN)               \
    KW("break", BREAK)                 \
    KW("continue", CONTINUE)

static const struct {
    const char *text;
    size_t length;
    TokenType type;
} keywords[] = {
#define KW(text, type) {text, sizeof(text) - 1, TOKEN_##type},
    HD_KEYWORD_LIST(KW)
#undef KW
};

void LexerInit(Lexer* lexer, const char* source) {
    lexer->source = source;
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->column = 1;
    lexer->token_line = 1;
    lexer->token_column = 1;
}

static Token make_token(Lexer* lexer, TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.span.start_offset = (size_t)(lexer->start - lexer->source);
    token.span.end_offset = (size_t)(lexer->current - lexer->source);
    token.span.start_line = lexer->token_line;
    token.span.start_column = lexer->token_column;
    token.span.end_line = lexer->line;
    token.span.end_column = lexer->column;
    return token;
}

static char advance(Lexer* lexer) {
    char c = *lexer->current++;
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return c;
}

static char peek(Lexer* lexer) {
    return *lexer->current;
}

static char peek_next(Lexer* lexer) {
    if (*lexer->current == '\0') return '\0';
    return lexer->current[1];
}

static int match(Lexer* lexer, char expected) {
    if (*lexer->current == expected) {
        advance(lexer);
        return 1;
    }
    return 0;
}

static void skip_whitespace(Lexer* lexer) {
    while (1) {
        char c = peek(lexer);
        if (is_space(c)) {
            advance(lexer);
        } else if (c == '\n') {
            advance(lexer);
        } else if (c == '/' && peek_next(lexer) == '/') {
            // Skip line comments
            while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                advance(lexer);
            }
        } else if (c == '/' && peek_next(lexer) == '*') {
            advance(lexer);
            advance(lexer);
            while (!(peek(lexer) == '*' && peek_next(lexer) == '/') && peek(lexer) != '\0') {
                advance(lexer);
            }
            if (peek(lexer) == '*') {
                advance(lexer);
                advance(lexer);
            }
        } else {
            break;
        }
    }
}

static Token string(Lexer* lexer) {
    while (peek(lexer) != '"' && peek(lexer) != '\0') {
        advance(lexer);
    }

    if (peek(lexer) == '\0') {
        /* Unterminated strings are invalid tokens. */
        return make_token(lexer, TOKEN_UNKNOWN);
    }

    // Consume the closing quote
    advance(lexer);
    return make_token(lexer, TOKEN_STRING);
}

static Token number(Lexer* lexer) {
    if (lexer->start[0] == '0' && (peek(lexer) == 'x' || peek(lexer) == 'X')) {
        advance(lexer);
        while (is_hex_digit(peek(lexer))) {
            advance(lexer);
        }
        return make_token(lexer, TOKEN_NUMBER);
    }

    while (is_digit(peek(lexer))) {
        advance(lexer);
    }

    /* A '.' only opens a fraction when a digit follows. That keeps the
     * number short in `1..5` (a range) and `1.length` (a property), which
     * would otherwise be swallowed as part of the literal. */
    if (peek(lexer) == '.' && is_digit(peek_next(lexer))) {
        advance(lexer);
        while (is_digit(peek(lexer))) {
            advance(lexer);
        }
        return make_token(lexer, TOKEN_FLOAT);
    }

    return make_token(lexer, TOKEN_NUMBER);
}

static Token identifier(Lexer* lexer) {
    while (is_alnum(peek(lexer))) {
        advance(lexer);
    }

    size_t length = (size_t)(lexer->current - lexer->start);
    size_t keyword_count = sizeof(keywords) / sizeof(keywords[0]);
    for (size_t i = 0; i < keyword_count; i++) {
        if (keywords[i].text[0] == lexer->start[0] &&
            keywords[i].length == length &&
            string_match(lexer->start, keywords[i].text, length)) {
            return make_token(lexer, keywords[i].type);
        }
    }

    return make_token(lexer, TOKEN_IDENTIFIER);
}

Token LexerNextToken(Lexer* lexer) {
    skip_whitespace(lexer);
    lexer->start = lexer->current;
    lexer->token_line = lexer->line;
    lexer->token_column = lexer->column;

    if (peek(lexer) == '\0') return make_token(lexer, TOKEN_EOF);

    char c = advance(lexer);

    if (c == '\n') {
        // Consume any consecutive newlines so we only emit ONE TOKEN_NEWLINE
        while (peek(lexer) == '\n') {
            advance(lexer);
        }
        return make_token(lexer, TOKEN_NEWLINE);
    }

    if (is_digit(c)) return number(lexer);
    if (is_alpha(c)) return identifier(lexer);
    switch (c) {
        // case '\\': return make_token(lexer, TOKEN_BACKSLASH);
        // case '"': return make_token(lexer, TOKEN_QUOTE);
        case '(': return make_token(lexer, TOKEN_LPAREN);
        case ')': return make_token(lexer, TOKEN_RPAREN);
        case '{': return make_token(lexer, TOKEN_LBRACE);
        case '}': return make_token(lexer, TOKEN_RBRACE);
        case '[': return make_token(lexer, TOKEN_LBRACKET);
        case ']': return make_token(lexer, TOKEN_RBRACKET);
        case ';': return make_token(lexer, TOKEN_SEMICOLON);
        case ':': return make_token(lexer, TOKEN_COLON);
        case '?': return make_token(lexer, TOKEN_QUESTION);
        case ',': return make_token(lexer, TOKEN_COMMA);
        /* Longest run wins throughout: every operator that is a prefix of a
         * longer one has to test for the longer one first, or `>>>=` lexes
         * as `>>` `>=` and the error lands nowhere near the cause. The
         * nesting below reads as the munch order. */
        case '+':
            if (match(lexer, '+')) return make_token(lexer, TOKEN_PLUSPLUS);
            return make_token(lexer, match(lexer, '=') ? TOKEN_PLUS_ASSIGN : TOKEN_PLUS);
        case '-':
            if (match(lexer, '-')) return make_token(lexer, TOKEN_MINUSMINUS);
            return make_token(lexer, match(lexer, '=') ? TOKEN_MINUS_ASSIGN : TOKEN_MINUS);
        case '*': return make_token(lexer, match(lexer, '=') ? TOKEN_STAR_ASSIGN : TOKEN_STAR);
        /* Line and block comments were already consumed by
         * skip_whitespace, so a '/' reaching here is division. */
        case '/': return make_token(lexer, match(lexer, '=') ? TOKEN_SLASH_ASSIGN : TOKEN_SLASH);
        case '%': return make_token(lexer, match(lexer, '=') ? TOKEN_PERCENT_ASSIGN : TOKEN_PERCENT);
        case '~': return make_token(lexer, match(lexer, '=') ? TOKEN_TILDE_ASSIGN : TOKEN_TILDE);
        case '&':
            if (match(lexer, '&')) return make_token(lexer, TOKEN_ANDAND);
            return make_token(lexer, match(lexer, '=') ? TOKEN_AND_ASSIGN : TOKEN_AMPERSAND);
        case '|':
            if (match(lexer, '|')) return make_token(lexer, TOKEN_OROR);
            return make_token(lexer, match(lexer, '=') ? TOKEN_OR_ASSIGN : TOKEN_OR);
        /* '^^' is exponentiation and '^' is xor, so the doubled form has to
         * be taken before the '=' test , otherwise '^^=' reads as '^' '^='. */
        case '^':
            if (match(lexer, '^')) {
                return make_token(lexer, match(lexer, '=') ? TOKEN_POW_ASSIGN : TOKEN_POW);
            }
            return make_token(lexer, match(lexer, '=') ? TOKEN_XOR_ASSIGN : TOKEN_XOR);
        case '!': return make_token(lexer, match(lexer, '=') ? TOKEN_NEQ : TOKEN_BANG);
        case '=':
            if (match(lexer, '>')) return make_token(lexer, TOKEN_LAMBDA);
            return make_token(lexer, match(lexer, '=') ? TOKEN_EQEQ : TOKEN_ASSIGN);
        case '<':
            if (match(lexer, '<')) {
                return make_token(lexer, match(lexer, '=') ? TOKEN_SHL_ASSIGN : TOKEN_SHL);
            }
            return make_token(lexer, match(lexer, '=') ? TOKEN_LTEQ : TOKEN_LT);
        /* Four deep: >>>= then >>> then >>= then >> then >= then >. */
        case '>':
            if (match(lexer, '>')) {
                if (match(lexer, '>')) {
                    return make_token(lexer, match(lexer, '=') ? TOKEN_USHR_ASSIGN : TOKEN_USHR);
                }
                return make_token(lexer, match(lexer, '=') ? TOKEN_SHR_ASSIGN : TOKEN_SHR);
            }
            return make_token(lexer, match(lexer, '=') ? TOKEN_GTEQ : TOKEN_GT);
        case '"': return string(lexer);
        /* Longer */
        case '.':
            /* Longest run wins: '...' before '..' before a lone '.'. */
            if (peek(lexer) == '.' && peek_next(lexer) == '.') {
                advance(lexer);
                advance(lexer);
                return make_token(lexer, TOKEN_DOTDOTDOT);
            }
            if (peek(lexer) == '.') {
                advance(lexer);
                return make_token(lexer, TOKEN_DOTDOT);
            }
            return make_token(lexer, TOKEN_DOT);
    }

    return make_token(lexer, TOKEN_UNKNOWN);
}

const char* TokenTypeToString(TokenType type) {
    switch (type) {
        case TOKEN_EOF: return "EOF";
        case TOKEN_NEWLINE: return "NEWLINE";
        case TOKEN_NUMBER: return "NUMBER";
        case TOKEN_FLOAT: return "FLOAT";
        case TOKEN_STRING: return "STRING";
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
#define KW(text, keyword_type) case TOKEN_##keyword_type: return #keyword_type;
        HD_KEYWORD_LIST(KW)
#undef KW
        case TOKEN_ASSIGN: return "ASSIGN";
        case TOKEN_ANDAND: return "ANDAND";
        case TOKEN_OROR: return "OROR";
        case TOKEN_PERCENT: return "PERCENT";
        case TOKEN_POW: return "POW";
        case TOKEN_OR: return "OR";
        case TOKEN_XOR: return "XOR";
        case TOKEN_SHL: return "SHL";
        case TOKEN_SHR: return "SHR";
        case TOKEN_USHR: return "USHR";
        case TOKEN_PLUS_ASSIGN: return "PLUS_ASSIGN";
        case TOKEN_MINUS_ASSIGN: return "MINUS_ASSIGN";
        case TOKEN_STAR_ASSIGN: return "STAR_ASSIGN";
        case TOKEN_SLASH_ASSIGN: return "SLASH_ASSIGN";
        case TOKEN_PERCENT_ASSIGN: return "PERCENT_ASSIGN";
        case TOKEN_POW_ASSIGN: return "POW_ASSIGN";
        case TOKEN_TILDE_ASSIGN: return "TILDE_ASSIGN";
        case TOKEN_AND_ASSIGN: return "AND_ASSIGN";
        case TOKEN_OR_ASSIGN: return "OR_ASSIGN";
        case TOKEN_XOR_ASSIGN: return "XOR_ASSIGN";
        case TOKEN_SHL_ASSIGN: return "SHL_ASSIGN";
        case TOKEN_SHR_ASSIGN: return "SHR_ASSIGN";
        case TOKEN_USHR_ASSIGN: return "USHR_ASSIGN";
        case TOKEN_PLUSPLUS: return "PLUSPLUS";
        case TOKEN_MINUSMINUS: return "MINUSMINUS";
        case TOKEN_PLUS: return "PLUS";
        case TOKEN_MINUS: return "MINUS";
        case TOKEN_STAR: return "STAR";
        case TOKEN_SLASH: return "SLASH";
        case TOKEN_TILDE: return "TILDE";
        case TOKEN_AMPERSAND: return "AMPERSAND";
        case TOKEN_BANG: return "BANG";
        case TOKEN_EQEQ: return "EQEQ";
        case TOKEN_NEQ: return "NEQ";
        case TOKEN_LT: return "LT";
        case TOKEN_GT: return "GT";
        case TOKEN_LTEQ: return "LTEQ";
        case TOKEN_GTEQ: return "GTEQ";
        case TOKEN_SEMICOLON: return "SEMICOLON";
        case TOKEN_COLON: return "COLON";
        case TOKEN_QUESTION: return "QUESTION";
        case TOKEN_COMMA: return "COMMA";
        case TOKEN_DOT: return "DOT";
        case TOKEN_DOTDOT: return "DOTDOT";
        case TOKEN_DOTDOTDOT: return "DOTDOTDOT";
        case TOKEN_LPAREN: return "LPAREN";
        case TOKEN_RPAREN: return "RPAREN";
        case TOKEN_LBRACE: return "LBRACE";
        case TOKEN_RBRACE: return "RBRACE";
        case TOKEN_LBRACKET: return "LBRACKET";
        case TOKEN_RBRACKET: return "RBRACKET";
        case TOKEN_LAMBDA: return "LAMBDA";
        case TOKEN_UNKNOWN: return "UNKNOWN";
        default: return "UNHANDLED";
    }
}
