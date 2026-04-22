#pragma once

/* ANSI escape codes for coloured console output */
#define RED     "\x1b[31m"
#define GREEN   "\x1b[32m"
#define RESET   "\x1b[0m"

/* File extension expected by the compiler */
#define EXTENSION ".hpl"

/*
 * TokenTypes — every token the lexer can produce.
 *
 * Numeric ranges are intentional:
 *   -1          : EOF sentinel
 *    0 – 100    : structural / meta tokens
 *  101 – 200    : language keywords
 *  201 – 300    : operators
 *  301+         : punctuation / indentation
 *
 * The LexerFSM relies on the range check  type > 300  to detect
 * punctuation tokens that should always terminate a lexeme, so do
 * not reorder or renumber without updating that logic.
 */
typedef enum {
    TOKEN_EOF = -1,     /* end of file — terminates the token stream      */
    TOKEN_IDLE,         /* placeholder used by the FSM while scanning      */
    TOKEN_EOS,          /* '.'  end-of-statement terminator                */
    TOKEN_NUMBER,       /* numeric literal, e.g.  42  or  -7               */
    TOKEN_STRING,       /* string literal, e.g.  "hello"                   */
    TOKEN_IDENT,        /* identifier (variable or function name)           */

    /* Keywords — start at 101 */
    TOKEN_LET = 101,    /* "Let"        variable declaration                */
    TOKEN_BE,           /* "be"         type binding in declaration         */
    TOKEN_SET,          /* "Set"        simple assignment                   */
    TOKEN_TO,           /* "to"         rhs of Set                         */
    TOKEN_BY,           /* "by"         rhs of Increase / Decrease          */
    TOKEN_SHOW,         /* "Show"       print to stdout                     */
    TOKEN_INTEGER,      /* "number"     integer type keyword                */
    TOKEN_TEXT,         /* "text"       string type keyword                 */
    TOKEN_INCREASE,     /* "Increase"   compound +=                        */
    TOKEN_DECREASE,     /* "Decrease"   compound -=                        */
    TOKEN_MULTIPLY,     /* "Multiply"   compound *=                        */
    TOKEN_DIVIDE,       /* "Divide"     compound /=                        */
    TOKEN_RETURN,       /* "Return"     return value from a function        */
    TOKEN_OTHERWISE,    /* "Otherwise"  else / else-if branch               */
    TOKEN_OTHERWISE_IF, /* "if"         the "if" after "Otherwise"          */
    TOKEN_IF,           /* "If"         conditional statement               */
    TOKEN_THEN,         /* "then"       separates condition from body       */
    TOKEN_REPEAT,       /* "Repeat"     counted loop                        */
    TOKEN_ITERATIONS,   /* "iterations" keyword after the repeat count      */
    TOKEN_WHILE,        /* "While"      condition-based loop                */
    TOKEN_GET,          /* "Get"        read input from the user            */
    TOKEN_ASK,          /* "ask"        prompt string in a Get statement    */
    TOKEN_STOREINTO,    /* "storeInto"  destination variable of Get         */
    TOKEN_TOFUNC,       /* "To"         function declaration                */
    TOKEN_WITH,         /* "with"       parameter list / call arguments     */
    TOKEN_NAN,          /* "nan"        empty argument list placeholder     */
    TOKEN_ATPOSITION,   /* "atPosition" character indexing into a string    */

    /* Operators — start at 201 */
    TOKEN_ADD = 201,    /* "plus"        binary addition                    */
    TOKEN_SUB,          /* "minus"       binary subtraction                 */
    TOKEN_MUL,          /* "times"       binary multiplication              */
    TOKEN_DIV,          /* "divide"      binary division                    */
    TOKEN_EQTO,         /* "equalsTo"    ==                                 */
    TOKEN_NOTEQ,        /* "notEqualsTo" !=                                 */
    TOKEN_LT,           /* "lessThan"    <                                  */
    TOKEN_ATMOST,       /* "atMost"      <=                                 */
    TOKEN_GT,           /* "greaterThan" >                                  */
    TOKEN_ATLEAST,      /* "atLeast"     >=                                 */
    TOKEN_REMAINDEROF,  /* "remainderOf" modulo lhs keyword                 */
    TOKEN_DIVIDEDBY,    /* "dividedBy"   modulo rhs keyword                 */
    TOKEN_AND,          /* "and"         logical AND (short-circuit)        */
    TOKEN_OR,           /* "or"          logical OR  (short-circuit)        */
    TOKEN_NOT,          /* "not"         logical NOT                        */

    /* Punctuation / indentation — start at 301 */
    TOKEN_LPAREN = 301, /* '('  open parenthesis                           */
    TOKEN_RPAREN,       /* ')'  close parenthesis                          */
    TOKEN_COLON,        /* ':'  opens an indented block                    */
    TOKEN_COMMA,        /* ','  argument / parameter separator              */
    TOKEN_NEWLINE,      /* newline — consumed internally by the lexer       */
    TOKEN_INDENT,       /* increase in indentation depth                   */
    TOKEN_DEDENT        /* decrease in indentation depth                   */
} TokenTypes;

/* A single token produced by the lexer */
typedef struct {
    char* lexeme;    /* null-terminated source text of the token */
    TokenTypes type; /* category of the token                    */
    unsigned line;   /* 1-based source line where it appears     */
} Token;
