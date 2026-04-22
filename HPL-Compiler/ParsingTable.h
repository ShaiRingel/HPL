#pragma once

/*
 * ParsingTable — the SLR(1) action/goto table used by the parser.
 *
 * Layout (two-level hash map):
 *   state ID  ->  StateNode  ->  StateRow  ->  ActionNode  ->  ParseAction
 *
 * The rule metadata arrays (ruleLength, ruleLHS) are stored flat because
 * they are indexed directly by rule ID during every reduce action.
 */
#define CAPACITY_STATES  179  /* hash-table bucket count for state nodes    */
#define CAPACITY_SYMBOLS 11   /* hash-table bucket count inside a StateRow  */
#define RULES_COUNT      69   /* total number of grammar production rules   */

/* The four actions an LR parser can take */
typedef enum {
    ACTION_ERROR = -1,
    ACTION_SHIFT,
    ACTION_REDUCE,
    ACTION_GOTO,
    ACTION_ACCEPT
} ActionType;

/* A single action: what to do and the associated rule/state number */
typedef struct {
    ActionType type;
    int value; /* shift/goto: target state; reduce: rule index */
} ParseAction;

/* One entry in a per-state symbol hash map */
typedef struct ActionNode {
    int symbol;
    ParseAction action;
    struct ActionNode* next;
} ActionNode;

/* Hash map from symbol to ParseAction, owned by a StateNode */
typedef struct {
    ActionNode** symbolBuckets;
    int capacity;
} StateRow;

/* One entry in the top-level state hash map */
typedef struct StateNode {
    unsigned short stateId;
    StateRow* row;
    struct StateNode* next;
} StateNode;

typedef struct {
    StateNode** stateBuckets;
    int capacity;
    int ruleLength[RULES_COUNT]; /* number of symbols on the rhs of each rule */
    int ruleLHS[RULES_COUNT];    /* non-terminal on the lhs of each rule      */
} ParsingTable;

ParsingTable* initParsingTable();
void insertTableEntry(ParsingTable* table, unsigned short stateId,
                               int symbol, ActionType type, int value);
ParseAction getEntry(ParsingTable* table, unsigned short stateId, int symbol);
void freeParsingTable(ParsingTable* table);
int getRuleLength(const ParsingTable* table, int ruleId);
int getLHS(const ParsingTable* table, int ruleId);
