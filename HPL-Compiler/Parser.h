#pragma once
#include "ParsingStack.h"
#include "ParsingTable.h"
#include "Lexer.h"
#include "CST.h"

/*
 * Parser — drives the SLR(1) parsing loop.
 *
 * nextAction() is called once per token.  It consults the parsing table,
 * performs a shift, reduce, or accept, and returns 1 when the input is
 * fully accepted.  On error it calls panicModeRecovery() and continues.
 *
 * lastErrorTokenType / lastErrorCount are used to avoid emitting
 * duplicate error messages when the same bad token keeps causing errors
 * across multiple recovery attempts.
 */
typedef struct {
    ParsingTable* table;
    ParsingStack* stack;
    CSTNode* cst;                   /* root of the finished CST after accept */
    TokenTypes lastErrorTokenType;  /* token type of the most recent error   */
    int lastErrorCount;             /* how many times that token has errored  */
} Parser;

Parser* initParser();
int nextAction(Parser* parser, Lexer* lexer, Token** token, int* next);
void freeParser(Parser* parser);
