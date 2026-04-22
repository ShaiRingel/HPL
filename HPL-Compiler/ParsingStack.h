#pragma once
#include "Global.h"
#include "CST.h"

/*
 * ParsingStack — the LR parser's working stack.
 *
 * Each item pairs a parser state number with the CST node that was
 * produced when that state was entered.  shift() pushes items onto the
 * front; reduce() pops `amount` items, builds a new non-terminal node
 * from their CST children, and returns it so the caller can push a
 * GOTO item.
 */
typedef struct {
    Token token;           /* the token that triggered this state entry   */
    CSTNode* cstNode;      /* CST node associated with this stack frame   */
    unsigned short state;  /* LR automaton state number                   */
} ParsingStackItem;

typedef struct ParsingStack {
    ParsingStackItem value;
    struct ParsingStack* next;
} ParsingStack;

ParsingStack* initParsingStack();
void shift(ParsingStack** stack, ParsingStackItem item);
CSTNode* reduce(ParsingStack** stack, int amount, int lhs);
ParsingStackItem lookahead(ParsingStack* stack);
void freeParsingStack(ParsingStack* stack);
CSTNode* pop(ParsingStack** stack);
void printStack(ParsingStack* stack);
