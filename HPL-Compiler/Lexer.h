#pragma once
#include "Global.h"
#include "LexerFSM.h"
#include <stdio.h>

#define MAX_TOKEN_SIZE      255  /* maximum length of a single lexeme     */
#define MAX_INDENT_DEPTH    10   /* maximum nesting depth for indentation  */
#define LONGEST_WORD_LENGTH 11   /* longest reserved keyword ("remainderOf" is 11 chars) */

/*
 * Lexer — reads source characters from a file and produces a stream of
 * tokens one at a time via nextToken().
 *
 * Indentation handling:
 *   The lexer tracks indentation with a stack.  When indentation increases
 *   an INDENT token is emitted; when it decreases one DEDENT is emitted per
 *   level.  Multiple pending DEDENTs are queued in pendingDedents so that
 *   nextToken() always returns exactly one token per call.
 */
typedef struct {
    LexerFSM* lexerFSM;
    FILE* fp;
    int indentStack[MAX_INDENT_DEPTH]; /* column widths at each nesting level */
    int indentTop;                     /* index of the current top of stack   */
    int pendingDedents;                /* DEDENT tokens queued for emission   */
    int isStartOfLine;                 /* non-zero when at the first token of a new line */
    int lineNumber;                    /* current 1-based source line         */
} Lexer;

Lexer* initLexer(const char* path);
Token* nextToken(Lexer* lexer);
void   freeLexer(Lexer* lexer);
