#pragma once
#include "TransitionTable.h"
#include "Global.h"

/*
 * LexerFSM — the finite-state machine that drives the lexer.
 *
 * advance() is called once per input character. It updates currentState
 * and returns TOKEN_IDLE while still consuming characters, or the final
 * TokenTypes value when a complete token has been recognised.
 */
typedef struct {
    unsigned short currentState;
    TransitionTable* transitionTable;
} LexerFSM;

LexerFSM* initLexerFSM();
TokenTypes advance(LexerFSM* lexerFSM, char input);
void freeLexerFSM(LexerFSM* lexerFSM);
