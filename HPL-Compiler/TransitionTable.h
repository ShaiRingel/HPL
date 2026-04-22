#pragma once
#include "Global.h"

/*
 * TransitionTable — the DFA backing the lexer FSM.
 *
 * Structure (two-level hash map):
 *   state ID  ->  StateBucket  ->  CharMap  ->  CharBucket  ->  next state
 *
 * Each state has an associated TokenTypes value that identifies what
 * token should be emitted when the FSM accepts from that state.
 *
 * Capacity constants:
 *   STATE_CAPACITY         : hash-table buckets for state nodes
 *   INITIAL_CHAR_CAPACITY  : initial bucket count for the per-state char map
 *   EXPANDED_CHAR_CAPACITY : bucket count after the first expansion
 */
#define STATE_CAPACITY         251
#define INITIAL_CHAR_CAPACITY  1
#define EXPANDED_CHAR_CAPACITY 11

/* Named sentinel values used throughout the FSM */
typedef enum {
    STATE_START   = 0,
    STATE_ACCEPT  = (unsigned short)-6, /* signals the FSM to stop and emit */
    STATE_ERROR,                        /* no valid transition exists        */
    STATE_IDENT,                        /* currently building an identifier  */
    STATE_NUMBER,                       /* currently building a number       */
    STATE_COMMENT,                      /* inside a NOTE: ... comment        */
    STATE_TEXT                          /* inside a "..." string literal     */
} SpecialStates;

/* One entry in a per-state character map */
typedef struct CharBucket {
    char key;                  /* input character              */
    unsigned short value;      /* destination state            */
    struct CharBucket* next;   /* next entry (chaining)        */
} CharBucket;

/* Hash map from characters to next-state values, owned by a StateBucket */
typedef struct {
    CharBucket** buckets;
    int capacity;
} CharMap;

/* One entry in the top-level state hash map */
typedef struct StateBucket {
    int key;                    /* state ID                                  */
    CharMap* value;             /* transitions out of this state             */
    TokenTypes token;           /* token emitted when accepting in this state */
    struct StateBucket* next;   /* next entry (chaining)                     */
} StateBucket;

/* The full DFA transition table */
typedef struct {
    StateBucket** buckets;
    int stateCounter; /* highest state ID allocated so far */
    int capacity;
} TransitionTable;

TransitionTable* initTransitionTable();
void             insertTransition(TransitionTable* table, unsigned short state,
                                  char symbol, unsigned short newState);
unsigned short   getState(const TransitionTable* table, unsigned short state,
                          char symbol);
void             setToken(TransitionTable* table, unsigned short state,
                          TokenTypes token);
TokenTypes       getTokenType(const TransitionTable* table, unsigned short state);
void             freeTransitionTable(TransitionTable* table);
