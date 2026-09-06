#ifndef HOLYD_LEXER_H
#define HOLYD_LEXER_H

// All the types of tokens our language understands
#include <stddef.h>

typedef struct {
  size_t start_offset;
  size_t end_offset;
  size_t start_line;
  size_t start_column;
  size_t end_line;
  size_t end_column;
} HDSourceSpan;

typedef enum {
  TOKEN_EOF,     				// End of file
  TOKEN_NEWLINE, 				// \n (useful for REPL)

  // Literals and Identifiers
  TOKEN_NUMBER,     		// 123
  TOKEN_FLOAT,      		// 1.5
  TOKEN_STRING,     		// "Hello"
  TOKEN_CHAR,       		// 'a', with escapes left raw as in TOKEN_STRING
  TOKEN_IDENTIFIER, 		// variable names like x, Print, myVar

  // Holy D Keywords
  TOKEN_U0,          		// U0
  TOKEN_I8,          		// I8
  TOKEN_U8,          		// U8
  TOKEN_I16,         		// I16
  TOKEN_U16,         		// U16
  TOKEN_I32,         		// I32
  TOKEN_U64,         		// U64
  TOKEN_I64,         		// I64
  TOKEN_U32,         		// U32
  TOKEN_F64,         		// F64
  TOKEN_VOID,        		// void
  TOKEN_INT,         		// int
  TOKEN_UINT,        		// uint
  TOKEN_LONG,        		// long
  TOKEN_ULONG,       		// ulong
  TOKEN_DOUBLE,      		// double
  TOKEN_BOOL,        		// bool
  TOKEN_STRING_TYPE, 		// string
  TOKEN_AUTO,        		// auto
  TOKEN_FOREACH,     		// foreach
  TOKEN_FOR,         		// for
  TOKEN_IF,          		// if
  TOKEN_ELSE,        		// else
  TOKEN_WHILE,       		// while
  TOKEN_RETURN,      		// return
  TOKEN_GOTO,        		// goto
  TOKEN_TRUE,        		// true
  TOKEN_FALSE,       		// false
  TOKEN_MODULE,      		// module
  TOKEN_IMPORT,      		// import
  TOKEN_CONST,       		// const
  TOKEN_IMMUTABLE,   		// immutable
  TOKEN_STATIC,      		// static
  TOKEN_SHARED,      		// shared
  TOKEN_INOUT,       		// inout
  TOKEN_FUNCTION,    		// function
  TOKEN_CLASS,       		// class
  TOKEN_STRUCT,      		// struct
  TOKEN_INTERFACE,   		// interface
  TOKEN_ENUM,        		// enum
  TOKEN_DELEGATE,    		// delegate
  TOKEN_TYPEOF,      		// typeof
  TOKEN_REF,         		// ref
  TOKEN_OUT,         		// out
  TOKEN_LAZY,        		// lazy
  TOKEN_SCOPE,       		// scope
  TOKEN_ABSTRACT,    		// abstract
  TOKEN_FINAL,       		// final
  TOKEN_OVERRIDE,    		// override
  TOKEN_NAMESPACE,			// namespace (x) { ... }
  TOKEN_EXTERN,      		// extern
  TOKEN_BREAK,					// break
  TOKEN_CONTINUE,				// continue

  /* Reserved but not yet parsed. D is the language of record, so every word
   * D reserves is claimed here whether or not HolyD implements the feature
   * yet: a word that lexes as an identifier today cannot become a keyword
   * later without breaking programs that used it as a name. `body` is the
   * one D keyword deliberately absent -- D itself retired it in favour of
   * `do` for contract bodies, so there is nothing to reserve it for. */
  TOKEN_SWITCH,    // switch
  TOKEN_CASE,      // case
  TOKEN_DEFAULT,   // default
  TOKEN_DO,        // do ... while, and D contract bodies
  TOKEN_FOREACH_REVERSE, // foreach_reverse
  TOKEN_TRY,       // try
  TOKEN_CATCH,     // catch
  TOKEN_FINALLY,   // finally
  TOKEN_THROW,     // throw
  TOKEN_CAST,      // cast(Type) expression
  TOKEN_IS,        // is, and the !is identity operator
  TOKEN_IN,        // in, and the !in membership operator
  TOKEN_NEW,       // new
  TOKEN_DELETE,    // delete (deprecated in D, reserved regardless)
  TOKEN_ASSERT,    // assert(expression)
  TOKEN_TYPEID,    // typeid
  TOKEN_NULL,      // null
  TOKEN_CHAR_TYPE, // char, the element type behind char[] and string
  TOKEN_THIS,      // this(...) constructors, and the receiver
  TOKEN_SUPER,     // super
  TOKEN_UNION,     // union
  TOKEN_ALIGN,     // align
  TOKEN_INVARIANT, // invariant
  TOKEN_PRIVATE,   // private
  TOKEN_PROTECTED, // protected
  TOKEN_PUBLIC,    // public
  TOKEN_PACKAGE,   // package
  TOKEN_EXPORT,    // export
  TOKEN_DEPRECATED,// deprecated
  TOKEN_PURE,      // pure
  TOKEN_NOTHROW,   // nothrow
  TOKEN_SYNCHRONIZED, // synchronized
  TOKEN_VERSION,   // version (NAME) { ... }
  TOKEN_DEBUG,     // debug
  TOKEN_PRAGMA,    // pragma
  TOKEN_ALIAS,     // alias Name = Type;
  TOKEN_UNITTEST,  // unittest { ... }
  TOKEN_TEMPLATE,  // template
  TOKEN_MIXIN,     // mixin
  TOKEN_MACRO,     // macro (reserved and unused in D too)
  TOKEN_WITH,      // with (expression) { ... }
  TOKEN_ASM,       // asm { ... }

  // Operators
  TOKEN_ASSIGN,     		// =
  TOKEN_PLUSPLUS,   		// ++
  TOKEN_MINUSMINUS, 		// --
  TOKEN_PLUS,       		// +
  TOKEN_MINUS,      		// -
  TOKEN_STAR,       		// *
  TOKEN_SLASH,      		// /
  TOKEN_PERCENT,    		// %
  TOKEN_POW,        		// ^^
  TOKEN_TILDE,      		// ~
  TOKEN_BANG,       		// !

  // Logical and comparison operators
  TOKEN_ANDAND, 				// &&
  TOKEN_OROR,   				// ||
  TOKEN_EQEQ,   				// ==
  TOKEN_NEQ,    				// !=
  TOKEN_LT,     				// <
  TOKEN_GT,     				// >
  TOKEN_LTEQ,   				// <=
  TOKEN_GTEQ,   				// >=

  // Bitwise and shift operators
  TOKEN_AMPERSAND, 			// &
  TOKEN_OR,        			// |
  TOKEN_XOR,       			// ^
  TOKEN_SHL,       			// <<
  TOKEN_SHR,       			// >>
  TOKEN_USHR,      			// >>>

  /* Compound-assignment operators for arithmetic, power,
   * concatenation, bitwise, and shift operations. */
  TOKEN_PLUS_ASSIGN,    // +=
  TOKEN_MINUS_ASSIGN,   // -=
  TOKEN_STAR_ASSIGN,    // *=
  TOKEN_SLASH_ASSIGN,   // /=
  TOKEN_PERCENT_ASSIGN, // %=
  TOKEN_POW_ASSIGN,     // ^^=
  TOKEN_TILDE_ASSIGN,   // ~=
  TOKEN_AND_ASSIGN,     // &=
  TOKEN_OR_ASSIGN,      // |=
  TOKEN_XOR_ASSIGN,     // ^=
  TOKEN_SHL_ASSIGN,     // <<=
  TOKEN_SHR_ASSIGN,     // >>=
  TOKEN_USHR_ASSIGN,    // >>>=

  // Punctuation
  TOKEN_SEMICOLON, // ;
  TOKEN_COLON,     // :
  TOKEN_COMMA,     // ,
  TOKEN_DOT,       // .
  TOKEN_DOTDOT,    // .. (D slice bounds)
  TOKEN_DOTDOTDOT, // ... (HolyC case ranges, variadic parameters)
  TOKEN_LPAREN,    // (
  TOKEN_RPAREN,    // )
  TOKEN_LBRACE,    // {
  TOKEN_RBRACE,    // }
  TOKEN_LBRACKET,  // [
  TOKEN_RBRACKET,  // ]
  TOKEN_QUESTION,  // ?
  TOKEN_DOLLAR,    // $ (array length inside an index or slice)
  TOKEN_AT,        // @ (attribute sigil: @safe, @nogc, @property)

  TOKEN_LAMBDA, // =>

  TOKEN_UNKNOWN // Anything we don't recognize
} TokenType;

typedef struct {
  TokenType type;
  const char *start; // Pointer to the start of the token in source
  size_t length;     // Length of the token
  HDSourceSpan span; // Half-open source range [start, end)
} Token;

typedef struct {
  const char *source;  // Start of the whole source code
  const char *start;   // Start of the current token
  const char *current; // Current scanning position
  size_t line;         // Position of current (one-based)
  size_t column;
  size_t token_line;
  size_t token_column;
} Lexer;

void LexerInit(Lexer *lexer, const char *source);
Token LexerNextToken(Lexer *lexer);

// Helper to print tokens (for debugging)
const char *TokenTypeToString(TokenType type);

#endif
