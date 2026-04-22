#include "ParsingDefinitions.h"
#include "HashingFunctions.h"
#include "ErrorHandler.h"
#include "ParsingTable.h"
#include "Global.h"
#include <stdlib.h>
#include <assert.h>

/*
 * ParsingTable — the SLR(1) action/goto table, stored as a two-level
 * hash map:  state ID -> StateNode -> StateRow -> ActionNode -> ParseAction
 *
 * initParsingTable() allocates the structure and immediately calls
 * populateTable() and populateArrays() to fill in every entry.
 * Those two functions are purely mechanical data and contain no logic.
 *
 * The ruleLength / ruleLHS flat arrays are indexed directly by rule ID
 * during every reduce action, so they live on the struct rather than in
 * the hash map for O(1) access.
 */

/* Forward declarations for the two data-population functions below */
static void freeSymbolList(ActionNode* head);
static void freeStateRow(StateRow* row);
static void populateArrays(ParsingTable* table);
static void populateTable(ParsingTable* table);

static StateRow* allocateStateRow() {
    StateRow* row = (StateRow*)malloc(sizeof(StateRow));
    if (!row)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for StateRow");

    assert(row);
    row->capacity = CAPACITY_SYMBOLS;
    row->symbolBuckets = (ActionNode**)calloc(CAPACITY_SYMBOLS,
                                              sizeof(ActionNode*));
    if (!row->symbolBuckets)
        reportError(ERROR_INTERNAL, 0,
                    "Failed to allocate symbol buckets for StateRow");

    return row;
}

static StateNode* allocateStateNode(unsigned short stateId) {
    StateNode* node = (StateNode*)malloc(sizeof(StateNode));
    if (!node)
        reportError(ERROR_INTERNAL, 0,
                    "Failed to allocate memory for StateNode");

    assert(node);
    node->stateId = stateId;
    node->row = allocateStateRow();
    node->next = NULL;
    return node;
}

static void freeSymbolList(ActionNode* head) {
    ActionNode* temp;
    while (head) {
        temp = head;
        head = head->next;
        free(temp);
    }
}

static void freeStateRow(StateRow* row) {
    int i;
    if (!row) return;
    for (i = 0; i < row->capacity; i++)
        freeSymbolList(row->symbolBuckets[i]);
    free(row->symbolBuckets);
    free(row);
}

void freeParsingTable(ParsingTable* table) {
    StateNode* curr, *temp;
    int i;

    if (!table) return;

    for (i = 0; i < table->capacity; i++) {
        curr = table->stateBuckets[i];
        while (curr) {
            temp = curr;
            curr = curr->next;
            freeStateRow(temp->row);
            free(temp);
        }
    }
    free(table->stateBuckets);
    free(table);
}

ParsingTable* initParsingTable() {
    ParsingTable* table = (ParsingTable*)malloc(sizeof(ParsingTable));
    if (!table)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate for ParsingTable");

    assert(table);
    table->capacity = CAPACITY_STATES;
    table->stateBuckets = (StateNode**)calloc(CAPACITY_STATES,
                                              sizeof(StateNode*));
    if (!table->stateBuckets)
        reportError(ERROR_INTERNAL, 0,
                    "Failed to allocate state buckets for ParsingTable");

    assert(table->stateBuckets);
    populateTable(table);
    populateArrays(table);

    return table;
}

/* Return the existing StateNode for stateId, or allocate and insert one */
static StateNode* findOrCreateState(ParsingTable* table,
                                    unsigned short stateId) {
    unsigned idx;
    StateNode* curr;

    idx = hashNumber(stateId, table->capacity);
    curr = table->stateBuckets[idx];

    while (curr) {
        if (curr->stateId == stateId) return curr;
        curr = curr->next;
    }

    curr = allocateStateNode(stateId);
    curr->next = table->stateBuckets[idx];
    table->stateBuckets[idx] = curr;
    return curr;
}

static ActionNode* findSymbolInRow(StateRow* row, int symbol) {
    ActionNode* curr;
    unsigned idx;

    idx = hashNumber(symbol, row->capacity);
    curr = row->symbolBuckets[idx];

    while (curr) {
        if (curr->symbol == symbol) return curr;
        curr = curr->next;
    }
    return NULL;
}

static void addNewTransition(StateRow* row, int symbol,
                             ActionType type, int value) {
    ActionNode* newNode;
    unsigned idx;

    newNode = (ActionNode*)malloc(sizeof(ActionNode));
    if (!newNode)
        reportError(ERROR_INTERNAL, 0,
                    "Failed to allocate memory for ActionNode");

    assert(newNode);
    newNode->symbol = symbol;
    newNode->action.type = type;
    newNode->action.value = value;

    idx = hashNumber(symbol, row->capacity);
    newNode->next = row->symbolBuckets[idx];
    row->symbolBuckets[idx] = newNode;
}

/* Insert or overwrite one cell of the action/goto table */
void insertTableEntry(ParsingTable* table, unsigned short stateId,
                      int symbol, ActionType type, int value) {
    ActionNode* aNode;
    StateNode* stateNode;

    if (!table) return;

    stateNode = findOrCreateState(table, stateId);
    aNode = findSymbolInRow(stateNode->row, symbol);

    if (aNode) {
        /* Overwrite an existing entry (used when resolving conflicts) */
        aNode->action.type = type;
        aNode->action.value = value;
    } else {
        addNewTransition(stateNode->row, symbol, type, value);
    }
}

/* Returns an ACTION_ERROR entry when no action exists for (stateId, symbol) */
ParseAction getEntry(ParsingTable* table, unsigned short stateId, int symbol) {
    unsigned stateIdx, symbolIdx;
    StateNode* stateNode;
    ActionNode* symNode;
    ParseAction result;

    result.type = ACTION_ERROR;
    result.value = 0;

    if (!table) return result;

    stateIdx = hashNumber(stateId, table->capacity);
    stateNode = table->stateBuckets[stateIdx];

    while (stateNode) {
        if (stateNode->stateId == stateId) {
            symbolIdx = hashNumber(symbol, stateNode->row->capacity);
            symNode = stateNode->row->symbolBuckets[symbolIdx];
            while (symNode) {
                if (symNode->symbol == symbol) {
                    result = symNode->action;
                    return result;
                }
                symNode = symNode->next;
            }
            return result;
        }
        stateNode = stateNode->next;
    }

    return result;
}

int getRuleLength(const ParsingTable* table, int ruleId) {
    if (ruleId < 0 || ruleId >= RULES_COUNT) return 0;
    return table->ruleLength[ruleId];
}

int getLHS(const ParsingTable* table, int ruleId) {
    if (ruleId < 0 || ruleId >= RULES_COUNT) return -1;
    return table->ruleLHS[ruleId];
}

/* ----------------------------------------------------------------
 * populateArrays — rule metadata (rhs length and lhs non-terminal)
 * populateTable  — the full SLR(1) action/goto table entries
 * Both functions are machine-generated data; no logic to document.
 * ---------------------------------------------------------------- */

static void populateArrays(ParsingTable* table) {
    /* Length 0 (Epsilon) */
    table->ruleLength[29] = 0;

    /* Length 1 */
    table->ruleLength[0] = table->ruleLength[2] = table->ruleLength[4] = table->ruleLength[5] =
        table->ruleLength[6] = table->ruleLength[7] = table->ruleLength[8] = table->ruleLength[9] =
        table->ruleLength[10] = table->ruleLength[12] = table->ruleLength[13] = table->ruleLength[19] =
        table->ruleLength[23] = table->ruleLength[24] = table->ruleLength[27] = table->ruleLength[34] =
        table->ruleLength[40] = table->ruleLength[43] = table->ruleLength[46] = table->ruleLength[47] =
        table->ruleLength[48] = table->ruleLength[49] = table->ruleLength[50] = table->ruleLength[53] =
        table->ruleLength[55] = table->ruleLength[59] = table->ruleLength[60] = table->ruleLength[61] =
        table->ruleLength[62] = table->ruleLength[63] = table->ruleLength[64] = table->ruleLength[65] =
        table->ruleLength[66] = table->ruleLength[67] = table->ruleLength[68] = 1;

    /* Length 2 */
    table->ruleLength[3] = table->ruleLength[37] = table->ruleLength[57] = 2;

    /* Length 3 */
    table->ruleLength[1] = table->ruleLength[20] = table->ruleLength[21] = table->ruleLength[28] =
        table->ruleLength[35] = table->ruleLength[36] = table->ruleLength[41] = table->ruleLength[42] =
        table->ruleLength[44] = table->ruleLength[51] = table->ruleLength[52] = table->ruleLength[54] =
        table->ruleLength[56] = table->ruleLength[58] = 3;

    /* Length 4 */
    table->ruleLength[31] = table->ruleLength[45] = 4;

    /* Length 5 */
    table->ruleLength[11] = table->ruleLength[14] = table->ruleLength[15] = table->ruleLength[16] =
        table->ruleLength[17] = table->ruleLength[18] = table->ruleLength[30] = table->ruleLength[38] =
        table->ruleLength[39] = 5;

    /* Length 6 */
    table->ruleLength[25] = table->ruleLength[32] = table->ruleLength[33] = 6;

    /* Length 7 */
    table->ruleLength[22] = table->ruleLength[26] = 7;

    table->ruleLHS[0] = NON_TERMINAL_PROG;
    table->ruleLHS[1] = NON_TERMINAL_BLOCK;

    table->ruleLHS[2] = table->ruleLHS[3] = NON_TERMINAL_STMT_LIST;

    table->ruleLHS[4] = table->ruleLHS[5] = table->ruleLHS[6] = table->ruleLHS[7] =
        table->ruleLHS[8] = table->ruleLHS[9] = table->ruleLHS[10] = NON_TERMINAL_STMT;

    table->ruleLHS[11] = NON_TERMINAL_VAR_DECL;

    table->ruleLHS[12] = table->ruleLHS[13] = NON_TERMINAL_TYPE;

    table->ruleLHS[14] = table->ruleLHS[15] = table->ruleLHS[16] = table->ruleLHS[17] =
        table->ruleLHS[18] = NON_TERMINAL_ASSIGN_STMT;

    table->ruleLHS[19] = table->ruleLHS[20] = NON_TERMINAL_LVAL;

    table->ruleLHS[21] = table->ruleLHS[22] = NON_TERMINAL_IO_STMT;

    table->ruleLHS[23] = table->ruleLHS[24] = NON_TERMINAL_CTRL_FLOW;

    table->ruleLHS[25] = NON_TERMINAL_IF_STMT;

    table->ruleLHS[26] = table->ruleLHS[27] = NON_TERMINAL_ELIF_LIST;

    table->ruleLHS[28] = table->ruleLHS[29] = NON_TERMINAL_ELSE_PART;

    table->ruleLHS[30] = table->ruleLHS[31] = NON_TERMINAL_LOOP_STMT;

    table->ruleLHS[32] = table->ruleLHS[33] = NON_TERMINAL_FUNC_DECL;

    table->ruleLHS[34] = table->ruleLHS[35] = NON_TERMINAL_PARAM_LIST;

    table->ruleLHS[36] = NON_TERMINAL_FUNC_RET;

    table->ruleLHS[37] = NON_TERMINAL_FUNC_CALL_STMT;

    table->ruleLHS[38] = table->ruleLHS[39] = NON_TERMINAL_FUNC_CALL;

    table->ruleLHS[40] = table->ruleLHS[41] = NON_TERMINAL_ARG_LIST;

    table->ruleLHS[42] = table->ruleLHS[43] = NON_TERMINAL_E;

    table->ruleLHS[44] = table->ruleLHS[45] = table->ruleLHS[46] = NON_TERMINAL_T;

    table->ruleLHS[47] = table->ruleLHS[48] = table->ruleLHS[49] = table->ruleLHS[50] =
        table->ruleLHS[51] = NON_TERMINAL_F;

    table->ruleLHS[52] = table->ruleLHS[53] = NON_TERMINAL_COND;

    table->ruleLHS[54] = table->ruleLHS[55] = NON_TERMINAL_BOOL_T;

    table->ruleLHS[56] = table->ruleLHS[57] = table->ruleLHS[58] = NON_TERMINAL_BOOL_F;

    table->ruleLHS[59] = table->ruleLHS[60] = NON_TERMINAL_E_OP;

    table->ruleLHS[61] = table->ruleLHS[62] = NON_TERMINAL_T_OP;

    table->ruleLHS[63] = table->ruleLHS[64] = table->ruleLHS[65] = table->ruleLHS[66] =
        table->ruleLHS[67] = table->ruleLHS[68] = NON_TERMINAL_REL_OP;
}

static void populateTable(ParsingTable* table) {
    insertTableEntry(table, 0, TOKEN_LET, ACTION_SHIFT, 10);
    insertTableEntry(table, 0, TOKEN_IDENT, ACTION_SHIFT, 26);
    insertTableEntry(table, 0, TOKEN_SET, ACTION_SHIFT, 11);
    insertTableEntry(table, 0, TOKEN_INCREASE, ACTION_SHIFT, 12);
    insertTableEntry(table, 0, TOKEN_DECREASE, ACTION_SHIFT, 13);
    insertTableEntry(table, 0, TOKEN_MULTIPLY, ACTION_SHIFT, 14);
    insertTableEntry(table, 0, TOKEN_DIVIDE, ACTION_SHIFT, 15);
    insertTableEntry(table, 0, TOKEN_SHOW, ACTION_SHIFT, 16);
    insertTableEntry(table, 0, TOKEN_GET, ACTION_SHIFT, 17);
    insertTableEntry(table, 0, TOKEN_IF, ACTION_SHIFT, 23);
    insertTableEntry(table, 0, TOKEN_REPEAT, ACTION_SHIFT, 24);
    insertTableEntry(table, 0, TOKEN_WHILE, ACTION_SHIFT, 25);
    insertTableEntry(table, 0, TOKEN_TOFUNC, ACTION_SHIFT, 20);
    insertTableEntry(table, 0, TOKEN_RETURN, ACTION_SHIFT, 21);
    insertTableEntry(table, 0, NON_TERMINAL_STMT_LIST, ACTION_GOTO, 1);
    insertTableEntry(table, 0, NON_TERMINAL_STMT, ACTION_GOTO, 2);
    insertTableEntry(table, 0, NON_TERMINAL_VAR_DECL, ACTION_GOTO, 3);
    insertTableEntry(table, 0, NON_TERMINAL_ASSIGN_STMT, ACTION_GOTO, 4);
    insertTableEntry(table, 0, NON_TERMINAL_IO_STMT, ACTION_GOTO, 5);
    insertTableEntry(table, 0, NON_TERMINAL_CTRL_FLOW, ACTION_GOTO, 6);
    insertTableEntry(table, 0, NON_TERMINAL_IF_STMT, ACTION_GOTO, 18);
    insertTableEntry(table, 0, NON_TERMINAL_LOOP_STMT, ACTION_GOTO, 19);
    insertTableEntry(table, 0, NON_TERMINAL_FUNC_DECL, ACTION_GOTO, 7);
    insertTableEntry(table, 0, NON_TERMINAL_FUNC_RET, ACTION_GOTO, 8);
    insertTableEntry(table, 0, NON_TERMINAL_FUNC_CALL_STMT, ACTION_GOTO, 9);
    insertTableEntry(table, 0, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 22);
    insertTableEntry(table, 0, TOKEN_EOF, ACTION_ACCEPT, 0);
    insertTableEntry(table, 1, TOKEN_LET, ACTION_SHIFT, 10);
    insertTableEntry(table, 1, TOKEN_IDENT, ACTION_SHIFT, 26);
    insertTableEntry(table, 1, TOKEN_SET, ACTION_SHIFT, 11);
    insertTableEntry(table, 1, TOKEN_INCREASE, ACTION_SHIFT, 12);
    insertTableEntry(table, 1, TOKEN_DECREASE, ACTION_SHIFT, 13);
    insertTableEntry(table, 1, TOKEN_MULTIPLY, ACTION_SHIFT, 14);
    insertTableEntry(table, 1, TOKEN_DIVIDE, ACTION_SHIFT, 15);
    insertTableEntry(table, 1, TOKEN_SHOW, ACTION_SHIFT, 16);
    insertTableEntry(table, 1, TOKEN_GET, ACTION_SHIFT, 17);
    insertTableEntry(table, 1, TOKEN_IF, ACTION_SHIFT, 23);
    insertTableEntry(table, 1, TOKEN_REPEAT, ACTION_SHIFT, 24);
    insertTableEntry(table, 1, TOKEN_WHILE, ACTION_SHIFT, 25);
    insertTableEntry(table, 1, TOKEN_TOFUNC, ACTION_SHIFT, 20);
    insertTableEntry(table, 1, TOKEN_RETURN, ACTION_SHIFT, 21);
    insertTableEntry(table, 1, TOKEN_EOF, ACTION_ACCEPT, 0);
    insertTableEntry(table, 1, NON_TERMINAL_STMT, ACTION_GOTO, 27);
    insertTableEntry(table, 1, NON_TERMINAL_VAR_DECL, ACTION_GOTO, 3);
    insertTableEntry(table, 1, NON_TERMINAL_ASSIGN_STMT, ACTION_GOTO, 4);
    insertTableEntry(table, 1, NON_TERMINAL_IO_STMT, ACTION_GOTO, 5);
    insertTableEntry(table, 1, NON_TERMINAL_CTRL_FLOW, ACTION_GOTO, 6);
    insertTableEntry(table, 1, NON_TERMINAL_IF_STMT, ACTION_GOTO, 18);
    insertTableEntry(table, 1, NON_TERMINAL_LOOP_STMT, ACTION_GOTO, 19);
    insertTableEntry(table, 1, NON_TERMINAL_FUNC_DECL, ACTION_GOTO, 7);
    insertTableEntry(table, 1, NON_TERMINAL_FUNC_RET, ACTION_GOTO, 8);
    insertTableEntry(table, 1, NON_TERMINAL_FUNC_CALL_STMT, ACTION_GOTO, 9);
    insertTableEntry(table, 1, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 22);
    insertTableEntry(table, 2, TOKEN_DEDENT, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_LET, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_IDENT, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_SET, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_INCREASE, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_DECREASE, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_MULTIPLY, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_DIVIDE, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_SHOW, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_GET, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_IF, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_REPEAT, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_WHILE, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_TOFUNC, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_RETURN, ACTION_REDUCE, 2);
    insertTableEntry(table, 2, TOKEN_EOF, ACTION_REDUCE, 2);
    insertTableEntry(table, 3, TOKEN_DEDENT, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_LET, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_IDENT, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_SET, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_INCREASE, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_DECREASE, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_MULTIPLY, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_DIVIDE, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_SHOW, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_GET, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_IF, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_REPEAT, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_WHILE, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_TOFUNC, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_RETURN, ACTION_REDUCE, 4);
    insertTableEntry(table, 3, TOKEN_EOF, ACTION_REDUCE, 4);
    insertTableEntry(table, 4, TOKEN_DEDENT, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_LET, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_IDENT, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_SET, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_INCREASE, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_DECREASE, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_MULTIPLY, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_DIVIDE, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_SHOW, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_GET, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_IF, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_REPEAT, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_WHILE, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_TOFUNC, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_RETURN, ACTION_REDUCE, 5);
    insertTableEntry(table, 4, TOKEN_EOF, ACTION_REDUCE, 5);
    insertTableEntry(table, 5, TOKEN_DEDENT, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_LET, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_IDENT, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_SET, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_INCREASE, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_DECREASE, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_MULTIPLY, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_DIVIDE, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_SHOW, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_GET, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_IF, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_REPEAT, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_WHILE, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_TOFUNC, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_RETURN, ACTION_REDUCE, 6);
    insertTableEntry(table, 5, TOKEN_EOF, ACTION_REDUCE, 6);
    insertTableEntry(table, 6, TOKEN_DEDENT, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_LET, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_IDENT, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_SET, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_INCREASE, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_DECREASE, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_MULTIPLY, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_DIVIDE, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_SHOW, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_GET, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_IF, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_REPEAT, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_WHILE, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_TOFUNC, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_RETURN, ACTION_REDUCE, 7);
    insertTableEntry(table, 6, TOKEN_EOF, ACTION_REDUCE, 7);
    insertTableEntry(table, 7, TOKEN_DEDENT, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_LET, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_IDENT, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_SET, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_INCREASE, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_DECREASE, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_MULTIPLY, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_DIVIDE, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_SHOW, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_GET, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_IF, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_REPEAT, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_WHILE, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_TOFUNC, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_RETURN, ACTION_REDUCE, 8);
    insertTableEntry(table, 7, TOKEN_EOF, ACTION_REDUCE, 8);
    insertTableEntry(table, 8, TOKEN_DEDENT, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_LET, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_IDENT, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_SET, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_INCREASE, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_DECREASE, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_MULTIPLY, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_DIVIDE, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_SHOW, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_GET, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_IF, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_REPEAT, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_WHILE, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_TOFUNC, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_RETURN, ACTION_REDUCE, 9);
    insertTableEntry(table, 8, TOKEN_EOF, ACTION_REDUCE, 9);
    insertTableEntry(table, 9, TOKEN_DEDENT, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_LET, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_IDENT, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_SET, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_INCREASE, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_DECREASE, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_MULTIPLY, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_DIVIDE, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_SHOW, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_GET, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_IF, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_REPEAT, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_WHILE, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_TOFUNC, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_RETURN, ACTION_REDUCE, 10);
    insertTableEntry(table, 9, TOKEN_EOF, ACTION_REDUCE, 10);
    insertTableEntry(table, 10, TOKEN_IDENT, ACTION_SHIFT, 28);
    insertTableEntry(table, 11, TOKEN_IDENT, ACTION_SHIFT, 30);
    insertTableEntry(table, 11, NON_TERMINAL_LVAL, ACTION_GOTO, 29);
    insertTableEntry(table, 12, TOKEN_IDENT, ACTION_SHIFT, 30);
    insertTableEntry(table, 12, NON_TERMINAL_LVAL, ACTION_GOTO, 31);
    insertTableEntry(table, 13, TOKEN_IDENT, ACTION_SHIFT, 30);
    insertTableEntry(table, 13, NON_TERMINAL_LVAL, ACTION_GOTO, 32);
    insertTableEntry(table, 14, TOKEN_IDENT, ACTION_SHIFT, 30);
    insertTableEntry(table, 14, NON_TERMINAL_LVAL, ACTION_GOTO, 33);
    insertTableEntry(table, 15, TOKEN_IDENT, ACTION_SHIFT, 30);
    insertTableEntry(table, 15, NON_TERMINAL_LVAL, ACTION_GOTO, 34);
    insertTableEntry(table, 16, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 16, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 16, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 16, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 16, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 16, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 16, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 16, NON_TERMINAL_E, ACTION_GOTO, 35);
    insertTableEntry(table, 16, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 16, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 17, TOKEN_INTEGER, ACTION_SHIFT, 46);
    insertTableEntry(table, 17, TOKEN_TEXT, ACTION_SHIFT, 47);
    insertTableEntry(table, 17, NON_TERMINAL_TYPE, ACTION_GOTO, 45);
    insertTableEntry(table, 18, TOKEN_DEDENT, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_LET, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_IDENT, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_SET, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_INCREASE, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_DECREASE, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_MULTIPLY, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_DIVIDE, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_SHOW, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_GET, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_IF, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_REPEAT, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_WHILE, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_TOFUNC, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_RETURN, ACTION_REDUCE, 23);
    insertTableEntry(table, 18, TOKEN_EOF, ACTION_REDUCE, 23);
    insertTableEntry(table, 19, TOKEN_DEDENT, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_LET, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_IDENT, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_SET, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_INCREASE, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_DECREASE, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_MULTIPLY, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_DIVIDE, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_SHOW, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_GET, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_IF, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_REPEAT, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_WHILE, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_TOFUNC, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_RETURN, ACTION_REDUCE, 24);
    insertTableEntry(table, 19, TOKEN_EOF, ACTION_REDUCE, 24);
    insertTableEntry(table, 20, TOKEN_IDENT, ACTION_SHIFT, 48);
    insertTableEntry(table, 21, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 21, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 21, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 21, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 21, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 21, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 21, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 21, NON_TERMINAL_E, ACTION_GOTO, 49);
    insertTableEntry(table, 21, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 21, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 22, TOKEN_EOS, ACTION_SHIFT, 50);
    insertTableEntry(table, 23, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 23, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 23, TOKEN_LPAREN, ACTION_SHIFT, 56);
    insertTableEntry(table, 23, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 23, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 23, TOKEN_NOT, ACTION_SHIFT, 55);
    insertTableEntry(table, 23, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 23, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 23, NON_TERMINAL_E, ACTION_GOTO, 54);
    insertTableEntry(table, 23, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 23, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 23, NON_TERMINAL_COND, ACTION_GOTO, 51);
    insertTableEntry(table, 23, NON_TERMINAL_BOOL_T, ACTION_GOTO, 52);
    insertTableEntry(table, 23, NON_TERMINAL_BOOL_F, ACTION_GOTO, 53);
    insertTableEntry(table, 24, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 24, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 24, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 24, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 24, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 24, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 24, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 24, NON_TERMINAL_E, ACTION_GOTO, 57);
    insertTableEntry(table, 24, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 24, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 25, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 25, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 25, TOKEN_LPAREN, ACTION_SHIFT, 56);
    insertTableEntry(table, 25, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 25, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 25, TOKEN_NOT, ACTION_SHIFT, 55);
    insertTableEntry(table, 25, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 25, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 25, NON_TERMINAL_E, ACTION_GOTO, 54);
    insertTableEntry(table, 25, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 25, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 25, NON_TERMINAL_COND, ACTION_GOTO, 58);
    insertTableEntry(table, 25, NON_TERMINAL_BOOL_T, ACTION_GOTO, 52);
    insertTableEntry(table, 25, NON_TERMINAL_BOOL_F, ACTION_GOTO, 53);
    insertTableEntry(table, 26, TOKEN_WITH, ACTION_SHIFT, 59);
    insertTableEntry(table, 27, TOKEN_DEDENT, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_LET, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_IDENT, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_SET, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_INCREASE, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_DECREASE, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_MULTIPLY, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_DIVIDE, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_SHOW, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_GET, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_IF, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_REPEAT, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_WHILE, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_TOFUNC, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_RETURN, ACTION_REDUCE, 3);
    insertTableEntry(table, 27, TOKEN_EOF, ACTION_REDUCE, 3);
    insertTableEntry(table, 28, TOKEN_BE, ACTION_SHIFT, 60);
    insertTableEntry(table, 29, TOKEN_TO, ACTION_SHIFT, 61);
    insertTableEntry(table, 30, TOKEN_EOS, ACTION_REDUCE, 19);
    insertTableEntry(table, 30, TOKEN_TO, ACTION_REDUCE, 19);
    insertTableEntry(table, 30, TOKEN_BY, ACTION_REDUCE, 19);
    insertTableEntry(table, 30, TOKEN_ATPOSITION, ACTION_SHIFT, 62);
    insertTableEntry(table, 31, TOKEN_BY, ACTION_SHIFT, 63);
    insertTableEntry(table, 32, TOKEN_BY, ACTION_SHIFT, 64);
    insertTableEntry(table, 33, TOKEN_BY, ACTION_SHIFT, 65);
    insertTableEntry(table, 34, TOKEN_BY, ACTION_SHIFT, 66);
    insertTableEntry(table, 35, TOKEN_EOS, ACTION_SHIFT, 67);
    insertTableEntry(table, 35, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 35, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 35, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 36, TOKEN_EOS, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_THEN, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_COLON, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_ITERATIONS, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_COMMA, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_RPAREN, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_OR, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_AND, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_ADD, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_SUB, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_MUL, ACTION_SHIFT, 72);
    insertTableEntry(table, 36, TOKEN_DIV, ACTION_SHIFT, 73);
    insertTableEntry(table, 36, TOKEN_EQTO, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_NOTEQ, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_GT, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_LT, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_ATLEAST, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, TOKEN_ATMOST, ACTION_REDUCE, 43);
    insertTableEntry(table, 36, NON_TERMINAL_T_OP, ACTION_GOTO, 71);
    insertTableEntry(table, 37, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 37, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 37, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 37, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 37, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 37, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 37, NON_TERMINAL_F, ACTION_GOTO, 74);
    insertTableEntry(table, 38, TOKEN_EOS, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_THEN, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_COLON, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_ITERATIONS, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_COMMA, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_RPAREN, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_OR, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_AND, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_ADD, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_SUB, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_MUL, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_DIV, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_EQTO, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_NOTEQ, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_GT, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_LT, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_ATLEAST, ACTION_REDUCE, 46);
    insertTableEntry(table, 38, TOKEN_ATMOST, ACTION_REDUCE, 46);
    insertTableEntry(table, 39, TOKEN_EOS, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_TO, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_BY, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_THEN, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_COLON, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_ITERATIONS, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_COMMA, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_RPAREN, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_DIVIDEDBY, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_OR, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_AND, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_ADD, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_SUB, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_MUL, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_DIV, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_EQTO, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_NOTEQ, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_GT, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_LT, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_ATLEAST, ACTION_REDUCE, 47);
    insertTableEntry(table, 39, TOKEN_ATMOST, ACTION_REDUCE, 47);
    insertTableEntry(table, 40, TOKEN_EOS, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_TO, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_BY, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_THEN, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_COLON, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_ITERATIONS, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_COMMA, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_RPAREN, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_DIVIDEDBY, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_OR, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_AND, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_ADD, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_SUB, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_MUL, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_DIV, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_EQTO, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_NOTEQ, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_GT, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_LT, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_ATLEAST, ACTION_REDUCE, 48);
    insertTableEntry(table, 40, TOKEN_ATMOST, ACTION_REDUCE, 48);
    insertTableEntry(table, 41, TOKEN_EOS, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_TO, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_BY, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_THEN, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_COLON, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_ITERATIONS, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_COMMA, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_RPAREN, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_DIVIDEDBY, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_OR, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_AND, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_ADD, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_SUB, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_MUL, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_DIV, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_EQTO, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_NOTEQ, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_GT, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_LT, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_ATLEAST, ACTION_REDUCE, 49);
    insertTableEntry(table, 41, TOKEN_ATMOST, ACTION_REDUCE, 49);
    insertTableEntry(table, 42, TOKEN_EOS, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_TO, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_BY, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_THEN, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_COLON, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_ITERATIONS, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_COMMA, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_RPAREN, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_DIVIDEDBY, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_OR, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_AND, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_ADD, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_SUB, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_MUL, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_DIV, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_EQTO, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_NOTEQ, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_GT, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_LT, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_ATLEAST, ACTION_REDUCE, 50);
    insertTableEntry(table, 42, TOKEN_ATMOST, ACTION_REDUCE, 50);
    insertTableEntry(table, 43, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 43, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 43, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 43, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 43, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 43, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 43, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 43, NON_TERMINAL_E, ACTION_GOTO, 75);
    insertTableEntry(table, 43, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 43, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 44, TOKEN_EOS, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_TO, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_BY, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_ATPOSITION, ACTION_SHIFT, 62);
    insertTableEntry(table, 44, TOKEN_THEN, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_COLON, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_ITERATIONS, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_WITH, ACTION_SHIFT, 59);
    insertTableEntry(table, 44, TOKEN_COMMA, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_RPAREN, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_DIVIDEDBY, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_OR, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_AND, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_ADD, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_SUB, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_MUL, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_DIV, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_EQTO, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_NOTEQ, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_GT, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_LT, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_ATLEAST, ACTION_REDUCE, 19);
    insertTableEntry(table, 44, TOKEN_ATMOST, ACTION_REDUCE, 19);
    insertTableEntry(table, 45, TOKEN_ASK, ACTION_SHIFT, 76);
    insertTableEntry(table, 46, TOKEN_EOS, ACTION_REDUCE, 12);
    insertTableEntry(table, 46, TOKEN_ASK, ACTION_REDUCE, 12);
    insertTableEntry(table, 47, TOKEN_EOS, ACTION_REDUCE, 13);
    insertTableEntry(table, 47, TOKEN_ASK, ACTION_REDUCE, 13);
    insertTableEntry(table, 48, TOKEN_WITH, ACTION_SHIFT, 77);
    insertTableEntry(table, 49, TOKEN_EOS, ACTION_SHIFT, 78);
    insertTableEntry(table, 49, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 49, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 49, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 50, TOKEN_DEDENT, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_LET, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_IDENT, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_SET, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_INCREASE, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_DECREASE, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_MULTIPLY, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_DIVIDE, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_SHOW, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_GET, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_IF, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_REPEAT, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_WHILE, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_TOFUNC, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_RETURN, ACTION_REDUCE, 37);
    insertTableEntry(table, 50, TOKEN_EOF, ACTION_REDUCE, 37);
    insertTableEntry(table, 51, TOKEN_THEN, ACTION_SHIFT, 79);
    insertTableEntry(table, 51, TOKEN_OR, ACTION_SHIFT, 80);
    insertTableEntry(table, 52, TOKEN_THEN, ACTION_REDUCE, 53);
    insertTableEntry(table, 52, TOKEN_COLON, ACTION_REDUCE, 53);
    insertTableEntry(table, 52, TOKEN_RPAREN, ACTION_REDUCE, 53);
    insertTableEntry(table, 52, TOKEN_OR, ACTION_REDUCE, 53);
    insertTableEntry(table, 52, TOKEN_AND, ACTION_SHIFT, 81);
    insertTableEntry(table, 53, TOKEN_THEN, ACTION_REDUCE, 55);
    insertTableEntry(table, 53, TOKEN_COLON, ACTION_REDUCE, 55);
    insertTableEntry(table, 53, TOKEN_RPAREN, ACTION_REDUCE, 55);
    insertTableEntry(table, 53, TOKEN_OR, ACTION_REDUCE, 55);
    insertTableEntry(table, 53, TOKEN_AND, ACTION_REDUCE, 55);
    insertTableEntry(table, 54, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 54, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 54, TOKEN_EQTO, ACTION_SHIFT, 83);
    insertTableEntry(table, 54, TOKEN_NOTEQ, ACTION_SHIFT, 84);
    insertTableEntry(table, 54, TOKEN_GT, ACTION_SHIFT, 85);
    insertTableEntry(table, 54, TOKEN_LT, ACTION_SHIFT, 86);
    insertTableEntry(table, 54, TOKEN_ATLEAST, ACTION_SHIFT, 87);
    insertTableEntry(table, 54, TOKEN_ATMOST, ACTION_SHIFT, 88);
    insertTableEntry(table, 54, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 54, NON_TERMINAL_REL_OP, ACTION_GOTO, 82);
    insertTableEntry(table, 55, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 55, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 55, TOKEN_LPAREN, ACTION_SHIFT, 56);
    insertTableEntry(table, 55, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 55, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 55, TOKEN_NOT, ACTION_SHIFT, 55);
    insertTableEntry(table, 55, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 55, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 55, NON_TERMINAL_E, ACTION_GOTO, 54);
    insertTableEntry(table, 55, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 55, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 55, NON_TERMINAL_BOOL_F, ACTION_GOTO, 89);
    insertTableEntry(table, 56, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 56, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 56, TOKEN_LPAREN, ACTION_SHIFT, 56);
    insertTableEntry(table, 56, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 56, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 56, TOKEN_NOT, ACTION_SHIFT, 55);
    insertTableEntry(table, 56, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 56, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 56, NON_TERMINAL_E, ACTION_GOTO, 91);
    insertTableEntry(table, 56, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 56, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 56, NON_TERMINAL_COND, ACTION_GOTO, 90);
    insertTableEntry(table, 56, NON_TERMINAL_BOOL_T, ACTION_GOTO, 52);
    insertTableEntry(table, 56, NON_TERMINAL_BOOL_F, ACTION_GOTO, 53);
    insertTableEntry(table, 57, TOKEN_ITERATIONS, ACTION_SHIFT, 92);
    insertTableEntry(table, 57, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 57, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 57, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 58, TOKEN_COLON, ACTION_SHIFT, 93);
    insertTableEntry(table, 58, TOKEN_OR, ACTION_SHIFT, 80);
    insertTableEntry(table, 59, TOKEN_LPAREN, ACTION_SHIFT, 94);
    insertTableEntry(table, 60, TOKEN_INTEGER, ACTION_SHIFT, 46);
    insertTableEntry(table, 60, TOKEN_TEXT, ACTION_SHIFT, 47);
    insertTableEntry(table, 60, NON_TERMINAL_TYPE, ACTION_GOTO, 95);
    insertTableEntry(table, 61, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 61, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 61, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 61, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 61, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 61, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 61, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 61, NON_TERMINAL_E, ACTION_GOTO, 96);
    insertTableEntry(table, 61, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 61, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 62, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 62, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 62, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 62, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 62, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 62, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 62, NON_TERMINAL_F, ACTION_GOTO, 97);
    insertTableEntry(table, 63, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 63, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 63, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 63, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 63, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 63, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 63, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 63, NON_TERMINAL_E, ACTION_GOTO, 98);
    insertTableEntry(table, 63, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 63, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 64, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 64, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 64, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 64, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 64, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 64, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 64, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 64, NON_TERMINAL_E, ACTION_GOTO, 99);
    insertTableEntry(table, 64, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 64, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 65, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 65, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 65, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 65, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 65, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 65, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 65, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 65, NON_TERMINAL_E, ACTION_GOTO, 100);
    insertTableEntry(table, 65, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 65, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 66, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 66, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 66, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 66, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 66, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 66, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 66, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 66, NON_TERMINAL_E, ACTION_GOTO, 101);
    insertTableEntry(table, 66, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 66, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 67, TOKEN_DEDENT, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_LET, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_IDENT, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_SET, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_INCREASE, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_DECREASE, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_MULTIPLY, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_DIVIDE, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_SHOW, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_GET, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_IF, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_REPEAT, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_WHILE, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_TOFUNC, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_RETURN, ACTION_REDUCE, 21);
    insertTableEntry(table, 67, TOKEN_EOF, ACTION_REDUCE, 21);
    insertTableEntry(table, 68, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 68, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 68, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 68, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 68, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 68, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 68, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 68, NON_TERMINAL_T, ACTION_GOTO, 102);
    insertTableEntry(table, 68, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 69, TOKEN_IDENT, ACTION_REDUCE, 59);
    insertTableEntry(table, 69, TOKEN_STRING, ACTION_REDUCE, 59);
    insertTableEntry(table, 69, TOKEN_LPAREN, ACTION_REDUCE, 59);
    insertTableEntry(table, 69, TOKEN_REMAINDEROF, ACTION_REDUCE, 59);
    insertTableEntry(table, 69, TOKEN_NUMBER, ACTION_REDUCE, 59);
    insertTableEntry(table, 70, TOKEN_IDENT, ACTION_REDUCE, 60);
    insertTableEntry(table, 70, TOKEN_STRING, ACTION_REDUCE, 60);
    insertTableEntry(table, 70, TOKEN_LPAREN, ACTION_REDUCE, 60);
    insertTableEntry(table, 70, TOKEN_REMAINDEROF, ACTION_REDUCE, 60);
    insertTableEntry(table, 70, TOKEN_NUMBER, ACTION_REDUCE, 60);
    insertTableEntry(table, 71, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 71, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 71, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 71, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 71, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 71, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 71, NON_TERMINAL_F, ACTION_GOTO, 103);
    insertTableEntry(table, 72, TOKEN_IDENT, ACTION_REDUCE, 61);
    insertTableEntry(table, 72, TOKEN_STRING, ACTION_REDUCE, 61);
    insertTableEntry(table, 72, TOKEN_LPAREN, ACTION_REDUCE, 61);
    insertTableEntry(table, 72, TOKEN_NUMBER, ACTION_REDUCE, 61);
    insertTableEntry(table, 73, TOKEN_IDENT, ACTION_REDUCE, 62);
    insertTableEntry(table, 73, TOKEN_STRING, ACTION_REDUCE, 62);
    insertTableEntry(table, 73, TOKEN_LPAREN, ACTION_REDUCE, 62);
    insertTableEntry(table, 73, TOKEN_NUMBER, ACTION_REDUCE, 62);
    insertTableEntry(table, 74, TOKEN_DIVIDEDBY, ACTION_SHIFT, 104);
    insertTableEntry(table, 75, TOKEN_RPAREN, ACTION_SHIFT, 105);
    insertTableEntry(table, 75, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 75, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 75, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 76, TOKEN_STRING, ACTION_SHIFT, 106);
    insertTableEntry(table, 77, TOKEN_IDENT, ACTION_SHIFT, 109);
    insertTableEntry(table, 77, TOKEN_NAN, ACTION_SHIFT, 108);
    insertTableEntry(table, 77, NON_TERMINAL_PARAM_LIST, ACTION_GOTO, 107);
    insertTableEntry(table, 78, TOKEN_DEDENT, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_LET, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_IDENT, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_SET, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_INCREASE, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_DECREASE, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_MULTIPLY, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_DIVIDE, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_SHOW, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_GET, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_IF, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_REPEAT, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_WHILE, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_TOFUNC, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_RETURN, ACTION_REDUCE, 36);
    insertTableEntry(table, 78, TOKEN_EOF, ACTION_REDUCE, 36);
    insertTableEntry(table, 79, TOKEN_COLON, ACTION_SHIFT, 110);
    insertTableEntry(table, 80, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 80, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 80, TOKEN_LPAREN, ACTION_SHIFT, 56);
    insertTableEntry(table, 80, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 80, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 80, TOKEN_NOT, ACTION_SHIFT, 55);
    insertTableEntry(table, 80, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 80, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 80, NON_TERMINAL_E, ACTION_GOTO, 54);
    insertTableEntry(table, 80, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 80, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 80, NON_TERMINAL_BOOL_T, ACTION_GOTO, 111);
    insertTableEntry(table, 80, NON_TERMINAL_BOOL_F, ACTION_GOTO, 53);
    insertTableEntry(table, 81, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 81, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 81, TOKEN_LPAREN, ACTION_SHIFT, 56);
    insertTableEntry(table, 81, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 81, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 81, TOKEN_NOT, ACTION_SHIFT, 55);
    insertTableEntry(table, 81, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 81, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 81, NON_TERMINAL_E, ACTION_GOTO, 54);
    insertTableEntry(table, 81, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 81, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 81, NON_TERMINAL_BOOL_F, ACTION_GOTO, 112);
    insertTableEntry(table, 82, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 82, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 82, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 82, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 82, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 82, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 82, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 82, NON_TERMINAL_E, ACTION_GOTO, 113);
    insertTableEntry(table, 82, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 82, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 83, TOKEN_IDENT, ACTION_REDUCE, 63);
    insertTableEntry(table, 83, TOKEN_STRING, ACTION_REDUCE, 63);
    insertTableEntry(table, 83, TOKEN_LPAREN, ACTION_REDUCE, 63);
    insertTableEntry(table, 83, TOKEN_REMAINDEROF, ACTION_REDUCE, 63);
    insertTableEntry(table, 83, TOKEN_NUMBER, ACTION_REDUCE, 63);
    insertTableEntry(table, 84, TOKEN_IDENT, ACTION_REDUCE, 64);
    insertTableEntry(table, 84, TOKEN_STRING, ACTION_REDUCE, 64);
    insertTableEntry(table, 84, TOKEN_LPAREN, ACTION_REDUCE, 64);
    insertTableEntry(table, 84, TOKEN_REMAINDEROF, ACTION_REDUCE, 64);
    insertTableEntry(table, 84, TOKEN_NUMBER, ACTION_REDUCE, 64);
    insertTableEntry(table, 85, TOKEN_IDENT, ACTION_REDUCE, 65);
    insertTableEntry(table, 85, TOKEN_STRING, ACTION_REDUCE, 65);
    insertTableEntry(table, 85, TOKEN_LPAREN, ACTION_REDUCE, 65);
    insertTableEntry(table, 85, TOKEN_REMAINDEROF, ACTION_REDUCE, 65);
    insertTableEntry(table, 85, TOKEN_NUMBER, ACTION_REDUCE, 65);
    insertTableEntry(table, 86, TOKEN_IDENT, ACTION_REDUCE, 66);
    insertTableEntry(table, 86, TOKEN_STRING, ACTION_REDUCE, 66);
    insertTableEntry(table, 86, TOKEN_LPAREN, ACTION_REDUCE, 66);
    insertTableEntry(table, 86, TOKEN_REMAINDEROF, ACTION_REDUCE, 66);
    insertTableEntry(table, 86, TOKEN_NUMBER, ACTION_REDUCE, 66);
    insertTableEntry(table, 87, TOKEN_IDENT, ACTION_REDUCE, 67);
    insertTableEntry(table, 87, TOKEN_STRING, ACTION_REDUCE, 67);
    insertTableEntry(table, 87, TOKEN_LPAREN, ACTION_REDUCE, 67);
    insertTableEntry(table, 87, TOKEN_REMAINDEROF, ACTION_REDUCE, 67);
    insertTableEntry(table, 87, TOKEN_NUMBER, ACTION_REDUCE, 67);
    insertTableEntry(table, 88, TOKEN_IDENT, ACTION_REDUCE, 68);
    insertTableEntry(table, 88, TOKEN_STRING, ACTION_REDUCE, 68);
    insertTableEntry(table, 88, TOKEN_LPAREN, ACTION_REDUCE, 68);
    insertTableEntry(table, 88, TOKEN_REMAINDEROF, ACTION_REDUCE, 68);
    insertTableEntry(table, 88, TOKEN_NUMBER, ACTION_REDUCE, 68);
    insertTableEntry(table, 89, TOKEN_THEN, ACTION_REDUCE, 57);
    insertTableEntry(table, 89, TOKEN_COLON, ACTION_REDUCE, 57);
    insertTableEntry(table, 89, TOKEN_RPAREN, ACTION_REDUCE, 57);
    insertTableEntry(table, 89, TOKEN_OR, ACTION_REDUCE, 57);
    insertTableEntry(table, 89, TOKEN_AND, ACTION_REDUCE, 57);
    insertTableEntry(table, 90, TOKEN_RPAREN, ACTION_SHIFT, 114);
    insertTableEntry(table, 90, TOKEN_OR, ACTION_SHIFT, 80);
    insertTableEntry(table, 91, TOKEN_RPAREN, ACTION_SHIFT, 105);
    insertTableEntry(table, 91, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 91, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 91, TOKEN_EQTO, ACTION_SHIFT, 83);
    insertTableEntry(table, 91, TOKEN_NOTEQ, ACTION_SHIFT, 84);
    insertTableEntry(table, 91, TOKEN_GT, ACTION_SHIFT, 85);
    insertTableEntry(table, 91, TOKEN_LT, ACTION_SHIFT, 86);
    insertTableEntry(table, 91, TOKEN_ATLEAST, ACTION_SHIFT, 87);
    insertTableEntry(table, 91, TOKEN_ATMOST, ACTION_SHIFT, 88);
    insertTableEntry(table, 91, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 91, NON_TERMINAL_REL_OP, ACTION_GOTO, 82);
    insertTableEntry(table, 92, TOKEN_COLON, ACTION_SHIFT, 115);
    insertTableEntry(table, 93, TOKEN_INDENT, ACTION_SHIFT, 117);
    insertTableEntry(table, 93, NON_TERMINAL_BLOCK, ACTION_GOTO, 116);
    insertTableEntry(table, 94, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 94, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 94, TOKEN_NAN, ACTION_SHIFT, 119);
    insertTableEntry(table, 94, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 94, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 94, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 94, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 94, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 94, NON_TERMINAL_ARG_LIST, ACTION_GOTO, 118);
    insertTableEntry(table, 94, NON_TERMINAL_E, ACTION_GOTO, 120);
    insertTableEntry(table, 94, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 94, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 95, TOKEN_EOS, ACTION_SHIFT, 121);
    insertTableEntry(table, 96, TOKEN_EOS, ACTION_SHIFT, 122);
    insertTableEntry(table, 96, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 96, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 96, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 97, TOKEN_EOS, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_TO, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_BY, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_THEN, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_COLON, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_ITERATIONS, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_COMMA, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_RPAREN, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_DIVIDEDBY, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_OR, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_AND, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_ADD, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_SUB, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_MUL, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_DIV, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_EQTO, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_NOTEQ, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_GT, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_LT, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_ATLEAST, ACTION_REDUCE, 20);
    insertTableEntry(table, 97, TOKEN_ATMOST, ACTION_REDUCE, 20);
    insertTableEntry(table, 98, TOKEN_EOS, ACTION_SHIFT, 123);
    insertTableEntry(table, 98, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 98, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 98, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 99, TOKEN_EOS, ACTION_SHIFT, 124);
    insertTableEntry(table, 99, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 99, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 99, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 100, TOKEN_EOS, ACTION_SHIFT, 125);
    insertTableEntry(table, 100, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 100, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 100, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 101, TOKEN_EOS, ACTION_SHIFT, 126);
    insertTableEntry(table, 101, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 101, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 101, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 102, TOKEN_EOS, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_THEN, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_COLON, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_ITERATIONS, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_COMMA, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_RPAREN, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_OR, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_AND, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_ADD, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_SUB, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_MUL, ACTION_SHIFT, 72);
    insertTableEntry(table, 102, TOKEN_DIV, ACTION_SHIFT, 73);
    insertTableEntry(table, 102, TOKEN_EQTO, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_NOTEQ, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_GT, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_LT, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_ATLEAST, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, TOKEN_ATMOST, ACTION_REDUCE, 42);
    insertTableEntry(table, 102, NON_TERMINAL_T_OP, ACTION_GOTO, 71);
    insertTableEntry(table, 103, TOKEN_EOS, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_THEN, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_COLON, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_ITERATIONS, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_COMMA, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_RPAREN, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_OR, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_AND, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_ADD, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_SUB, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_MUL, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_DIV, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_EQTO, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_NOTEQ, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_GT, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_LT, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_ATLEAST, ACTION_REDUCE, 44);
    insertTableEntry(table, 103, TOKEN_ATMOST, ACTION_REDUCE, 44);
    insertTableEntry(table, 104, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 104, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 104, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 104, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 104, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 104, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 104, NON_TERMINAL_F, ACTION_GOTO, 127);
    insertTableEntry(table, 105, TOKEN_EOS, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_TO, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_BY, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_THEN, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_COLON, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_ITERATIONS, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_COMMA, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_RPAREN, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_DIVIDEDBY, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_OR, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_AND, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_ADD, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_SUB, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_MUL, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_DIV, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_EQTO, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_NOTEQ, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_GT, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_LT, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_ATLEAST, ACTION_REDUCE, 51);
    insertTableEntry(table, 105, TOKEN_ATMOST, ACTION_REDUCE, 51);
    insertTableEntry(table, 106, TOKEN_STOREINTO, ACTION_SHIFT, 128);
    insertTableEntry(table, 107, TOKEN_COLON, ACTION_SHIFT, 129);
    insertTableEntry(table, 107, TOKEN_COMMA, ACTION_SHIFT, 130);
    insertTableEntry(table, 108, TOKEN_COLON, ACTION_SHIFT, 131);
    insertTableEntry(table, 109, TOKEN_COLON, ACTION_REDUCE, 34);
    insertTableEntry(table, 109, TOKEN_COMMA, ACTION_REDUCE, 34);
    insertTableEntry(table, 110, TOKEN_INDENT, ACTION_SHIFT, 117);
    insertTableEntry(table, 110, NON_TERMINAL_BLOCK, ACTION_GOTO, 132);
    insertTableEntry(table, 111, TOKEN_THEN, ACTION_REDUCE, 52);
    insertTableEntry(table, 111, TOKEN_COLON, ACTION_REDUCE, 52);
    insertTableEntry(table, 111, TOKEN_RPAREN, ACTION_REDUCE, 52);
    insertTableEntry(table, 111, TOKEN_OR, ACTION_REDUCE, 52);
    insertTableEntry(table, 111, TOKEN_AND, ACTION_SHIFT, 81);
    insertTableEntry(table, 112, TOKEN_THEN, ACTION_REDUCE, 54);
    insertTableEntry(table, 112, TOKEN_COLON, ACTION_REDUCE, 54);
    insertTableEntry(table, 112, TOKEN_RPAREN, ACTION_REDUCE, 54);
    insertTableEntry(table, 112, TOKEN_OR, ACTION_REDUCE, 54);
    insertTableEntry(table, 112, TOKEN_AND, ACTION_REDUCE, 54);
    insertTableEntry(table, 113, TOKEN_THEN, ACTION_REDUCE, 56);
    insertTableEntry(table, 113, TOKEN_COLON, ACTION_REDUCE, 56);
    insertTableEntry(table, 113, TOKEN_RPAREN, ACTION_REDUCE, 56);
    insertTableEntry(table, 113, TOKEN_OR, ACTION_REDUCE, 56);
    insertTableEntry(table, 113, TOKEN_AND, ACTION_REDUCE, 56);
    insertTableEntry(table, 113, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 113, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 113, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 114, TOKEN_THEN, ACTION_REDUCE, 58);
    insertTableEntry(table, 114, TOKEN_COLON, ACTION_REDUCE, 58);
    insertTableEntry(table, 114, TOKEN_RPAREN, ACTION_REDUCE, 58);
    insertTableEntry(table, 114, TOKEN_OR, ACTION_REDUCE, 58);
    insertTableEntry(table, 114, TOKEN_AND, ACTION_REDUCE, 58);
    insertTableEntry(table, 115, TOKEN_INDENT, ACTION_SHIFT, 117);
    insertTableEntry(table, 115, NON_TERMINAL_BLOCK, ACTION_GOTO, 133);
    insertTableEntry(table, 116, TOKEN_DEDENT, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_LET, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_IDENT, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_SET, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_INCREASE, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_DECREASE, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_MULTIPLY, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_DIVIDE, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_SHOW, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_GET, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_IF, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_REPEAT, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_WHILE, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_TOFUNC, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_RETURN, ACTION_REDUCE, 31);
    insertTableEntry(table, 116, TOKEN_EOF, ACTION_REDUCE, 31);
    insertTableEntry(table, 117, TOKEN_LET, ACTION_SHIFT, 10);
    insertTableEntry(table, 117, TOKEN_IDENT, ACTION_SHIFT, 26);
    insertTableEntry(table, 117, TOKEN_SET, ACTION_SHIFT, 11);
    insertTableEntry(table, 117, TOKEN_INCREASE, ACTION_SHIFT, 12);
    insertTableEntry(table, 117, TOKEN_DECREASE, ACTION_SHIFT, 13);
    insertTableEntry(table, 117, TOKEN_MULTIPLY, ACTION_SHIFT, 14);
    insertTableEntry(table, 117, TOKEN_DIVIDE, ACTION_SHIFT, 15);
    insertTableEntry(table, 117, TOKEN_SHOW, ACTION_SHIFT, 16);
    insertTableEntry(table, 117, TOKEN_GET, ACTION_SHIFT, 17);
    insertTableEntry(table, 117, TOKEN_IF, ACTION_SHIFT, 23);
    insertTableEntry(table, 117, TOKEN_REPEAT, ACTION_SHIFT, 24);
    insertTableEntry(table, 117, TOKEN_WHILE, ACTION_SHIFT, 25);
    insertTableEntry(table, 117, TOKEN_TOFUNC, ACTION_SHIFT, 20);
    insertTableEntry(table, 117, TOKEN_RETURN, ACTION_SHIFT, 21);
    insertTableEntry(table, 117, NON_TERMINAL_STMT_LIST, ACTION_GOTO, 134);
    insertTableEntry(table, 117, NON_TERMINAL_STMT, ACTION_GOTO, 2);
    insertTableEntry(table, 117, NON_TERMINAL_VAR_DECL, ACTION_GOTO, 3);
    insertTableEntry(table, 117, NON_TERMINAL_ASSIGN_STMT, ACTION_GOTO, 4);
    insertTableEntry(table, 117, NON_TERMINAL_IO_STMT, ACTION_GOTO, 5);
    insertTableEntry(table, 117, NON_TERMINAL_CTRL_FLOW, ACTION_GOTO, 6);
    insertTableEntry(table, 117, NON_TERMINAL_IF_STMT, ACTION_GOTO, 18);
    insertTableEntry(table, 117, NON_TERMINAL_LOOP_STMT, ACTION_GOTO, 19);
    insertTableEntry(table, 117, NON_TERMINAL_FUNC_DECL, ACTION_GOTO, 7);
    insertTableEntry(table, 117, NON_TERMINAL_FUNC_RET, ACTION_GOTO, 8);
    insertTableEntry(table, 117, NON_TERMINAL_FUNC_CALL_STMT, ACTION_GOTO, 9);
    insertTableEntry(table, 117, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 22);
    insertTableEntry(table, 118, TOKEN_COMMA, ACTION_SHIFT, 136);
    insertTableEntry(table, 118, TOKEN_RPAREN, ACTION_SHIFT, 135);
    insertTableEntry(table, 119, TOKEN_RPAREN, ACTION_SHIFT, 137);
    insertTableEntry(table, 120, TOKEN_COMMA, ACTION_REDUCE, 40);
    insertTableEntry(table, 120, TOKEN_RPAREN, ACTION_REDUCE, 40);
    insertTableEntry(table, 120, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 120, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 120, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 121, TOKEN_DEDENT, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_LET, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_IDENT, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_SET, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_INCREASE, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_DECREASE, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_MULTIPLY, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_DIVIDE, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_SHOW, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_GET, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_IF, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_REPEAT, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_WHILE, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_TOFUNC, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_RETURN, ACTION_REDUCE, 11);
    insertTableEntry(table, 121, TOKEN_EOF, ACTION_REDUCE, 11);
    insertTableEntry(table, 122, TOKEN_DEDENT, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_LET, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_IDENT, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_SET, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_INCREASE, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_DECREASE, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_MULTIPLY, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_DIVIDE, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_SHOW, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_GET, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_IF, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_REPEAT, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_WHILE, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_TOFUNC, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_RETURN, ACTION_REDUCE, 14);
    insertTableEntry(table, 122, TOKEN_EOF, ACTION_REDUCE, 14);
    insertTableEntry(table, 123, TOKEN_DEDENT, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_LET, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_IDENT, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_SET, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_INCREASE, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_DECREASE, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_MULTIPLY, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_DIVIDE, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_SHOW, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_GET, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_IF, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_REPEAT, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_WHILE, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_TOFUNC, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_RETURN, ACTION_REDUCE, 15);
    insertTableEntry(table, 123, TOKEN_EOF, ACTION_REDUCE, 15);
    insertTableEntry(table, 124, TOKEN_DEDENT, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_LET, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_IDENT, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_SET, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_INCREASE, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_DECREASE, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_MULTIPLY, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_DIVIDE, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_SHOW, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_GET, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_IF, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_REPEAT, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_WHILE, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_TOFUNC, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_RETURN, ACTION_REDUCE, 16);
    insertTableEntry(table, 124, TOKEN_EOF, ACTION_REDUCE, 16);
    insertTableEntry(table, 125, TOKEN_DEDENT, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_LET, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_IDENT, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_SET, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_INCREASE, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_DECREASE, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_MULTIPLY, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_DIVIDE, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_SHOW, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_GET, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_IF, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_REPEAT, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_WHILE, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_TOFUNC, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_RETURN, ACTION_REDUCE, 17);
    insertTableEntry(table, 125, TOKEN_EOF, ACTION_REDUCE, 17);
    insertTableEntry(table, 126, TOKEN_DEDENT, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_LET, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_IDENT, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_SET, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_INCREASE, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_DECREASE, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_MULTIPLY, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_DIVIDE, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_SHOW, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_GET, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_IF, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_REPEAT, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_WHILE, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_TOFUNC, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_RETURN, ACTION_REDUCE, 18);
    insertTableEntry(table, 126, TOKEN_EOF, ACTION_REDUCE, 18);
    insertTableEntry(table, 127, TOKEN_EOS, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_THEN, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_COLON, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_ITERATIONS, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_COMMA, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_RPAREN, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_OR, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_AND, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_ADD, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_SUB, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_MUL, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_DIV, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_EQTO, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_NOTEQ, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_GT, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_LT, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_ATLEAST, ACTION_REDUCE, 45);
    insertTableEntry(table, 127, TOKEN_ATMOST, ACTION_REDUCE, 45);
    insertTableEntry(table, 128, TOKEN_IDENT, ACTION_SHIFT, 30);
    insertTableEntry(table, 128, NON_TERMINAL_LVAL, ACTION_GOTO, 138);
    insertTableEntry(table, 129, TOKEN_INDENT, ACTION_SHIFT, 117);
    insertTableEntry(table, 129, NON_TERMINAL_BLOCK, ACTION_GOTO, 139);
    insertTableEntry(table, 130, TOKEN_IDENT, ACTION_SHIFT, 140);
    insertTableEntry(table, 131, TOKEN_INDENT, ACTION_SHIFT, 117);
    insertTableEntry(table, 131, NON_TERMINAL_BLOCK, ACTION_GOTO, 141);
    insertTableEntry(table, 132, TOKEN_DEDENT, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_LET, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_IDENT, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_SET, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_INCREASE, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_DECREASE, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_MULTIPLY, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_DIVIDE, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_SHOW, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_GET, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_IF, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_OTHERWISE, ACTION_SHIFT, 143);
    insertTableEntry(table, 132, TOKEN_REPEAT, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_WHILE, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_TOFUNC, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_RETURN, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, TOKEN_EOF, ACTION_REDUCE, 29);
    insertTableEntry(table, 132, NON_TERMINAL_ELIF_LIST, ACTION_GOTO, 142);
    insertTableEntry(table, 132, NON_TERMINAL_ELSE_PART, ACTION_GOTO, 144);
    insertTableEntry(table, 133, TOKEN_DEDENT, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_LET, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_IDENT, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_SET, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_INCREASE, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_DECREASE, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_MULTIPLY, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_DIVIDE, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_SHOW, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_GET, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_IF, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_REPEAT, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_WHILE, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_TOFUNC, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_RETURN, ACTION_REDUCE, 30);
    insertTableEntry(table, 133, TOKEN_EOF, ACTION_REDUCE, 30);
    insertTableEntry(table, 134, TOKEN_DEDENT, ACTION_SHIFT, 145);
    insertTableEntry(table, 134, TOKEN_LET, ACTION_SHIFT, 10);
    insertTableEntry(table, 134, TOKEN_IDENT, ACTION_SHIFT, 26);
    insertTableEntry(table, 134, TOKEN_SET, ACTION_SHIFT, 11);
    insertTableEntry(table, 134, TOKEN_INCREASE, ACTION_SHIFT, 12);
    insertTableEntry(table, 134, TOKEN_DECREASE, ACTION_SHIFT, 13);
    insertTableEntry(table, 134, TOKEN_MULTIPLY, ACTION_SHIFT, 14);
    insertTableEntry(table, 134, TOKEN_DIVIDE, ACTION_SHIFT, 15);
    insertTableEntry(table, 134, TOKEN_SHOW, ACTION_SHIFT, 16);
    insertTableEntry(table, 134, TOKEN_GET, ACTION_SHIFT, 17);
    insertTableEntry(table, 134, TOKEN_IF, ACTION_SHIFT, 23);
    insertTableEntry(table, 134, TOKEN_REPEAT, ACTION_SHIFT, 24);
    insertTableEntry(table, 134, TOKEN_WHILE, ACTION_SHIFT, 25);
    insertTableEntry(table, 134, TOKEN_TOFUNC, ACTION_SHIFT, 20);
    insertTableEntry(table, 134, TOKEN_RETURN, ACTION_SHIFT, 21);
    insertTableEntry(table, 134, NON_TERMINAL_STMT, ACTION_GOTO, 27);
    insertTableEntry(table, 134, NON_TERMINAL_VAR_DECL, ACTION_GOTO, 3);
    insertTableEntry(table, 134, NON_TERMINAL_ASSIGN_STMT, ACTION_GOTO, 4);
    insertTableEntry(table, 134, NON_TERMINAL_IO_STMT, ACTION_GOTO, 5);
    insertTableEntry(table, 134, NON_TERMINAL_CTRL_FLOW, ACTION_GOTO, 6);
    insertTableEntry(table, 134, NON_TERMINAL_IF_STMT, ACTION_GOTO, 18);
    insertTableEntry(table, 134, NON_TERMINAL_LOOP_STMT, ACTION_GOTO, 19);
    insertTableEntry(table, 134, NON_TERMINAL_FUNC_DECL, ACTION_GOTO, 7);
    insertTableEntry(table, 134, NON_TERMINAL_FUNC_RET, ACTION_GOTO, 8);
    insertTableEntry(table, 134, NON_TERMINAL_FUNC_CALL_STMT, ACTION_GOTO, 9);
    insertTableEntry(table, 134, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 22);
    insertTableEntry(table, 135, TOKEN_EOS, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_TO, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_BY, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_THEN, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_COLON, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_ITERATIONS, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_COMMA, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_RPAREN, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_DIVIDEDBY, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_OR, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_AND, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_ADD, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_SUB, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_MUL, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_DIV, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_EQTO, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_NOTEQ, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_GT, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_LT, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_ATLEAST, ACTION_REDUCE, 38);
    insertTableEntry(table, 135, TOKEN_ATMOST, ACTION_REDUCE, 38);
    insertTableEntry(table, 136, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 136, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 136, TOKEN_LPAREN, ACTION_SHIFT, 43);
    insertTableEntry(table, 136, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 136, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 136, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 136, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 136, NON_TERMINAL_E, ACTION_GOTO, 146);
    insertTableEntry(table, 136, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 136, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 137, TOKEN_EOS, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_TO, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_BY, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_THEN, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_COLON, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_ITERATIONS, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_COMMA, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_RPAREN, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_DIVIDEDBY, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_OR, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_AND, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_ADD, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_SUB, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_MUL, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_DIV, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_EQTO, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_NOTEQ, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_GT, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_LT, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_ATLEAST, ACTION_REDUCE, 39);
    insertTableEntry(table, 137, TOKEN_ATMOST, ACTION_REDUCE, 39);
    insertTableEntry(table, 138, TOKEN_EOS, ACTION_SHIFT, 147);
    insertTableEntry(table, 139, TOKEN_DEDENT, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_LET, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_IDENT, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_SET, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_INCREASE, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_DECREASE, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_MULTIPLY, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_DIVIDE, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_SHOW, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_GET, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_IF, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_REPEAT, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_WHILE, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_TOFUNC, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_RETURN, ACTION_REDUCE, 32);
    insertTableEntry(table, 139, TOKEN_EOF, ACTION_REDUCE, 32);
    insertTableEntry(table, 140, TOKEN_COLON, ACTION_REDUCE, 35);
    insertTableEntry(table, 140, TOKEN_COMMA, ACTION_REDUCE, 35);
    insertTableEntry(table, 141, TOKEN_DEDENT, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_LET, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_IDENT, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_SET, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_INCREASE, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_DECREASE, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_MULTIPLY, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_DIVIDE, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_SHOW, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_GET, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_IF, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_REPEAT, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_WHILE, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_TOFUNC, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_RETURN, ACTION_REDUCE, 33);
    insertTableEntry(table, 141, TOKEN_EOF, ACTION_REDUCE, 33);
    insertTableEntry(table, 142, TOKEN_DEDENT, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_LET, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_IDENT, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_SET, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_INCREASE, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_DECREASE, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_MULTIPLY, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_DIVIDE, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_SHOW, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_GET, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_IF, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_REPEAT, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_WHILE, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_TOFUNC, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_RETURN, ACTION_REDUCE, 25);
    insertTableEntry(table, 142, TOKEN_EOF, ACTION_REDUCE, 25);
    insertTableEntry(table, 143, TOKEN_COLON, ACTION_SHIFT, 149);
    insertTableEntry(table, 143, TOKEN_OTHERWISE_IF, ACTION_SHIFT, 148);
    insertTableEntry(table, 144, TOKEN_DEDENT, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_LET, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_IDENT, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_SET, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_INCREASE, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_DECREASE, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_MULTIPLY, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_DIVIDE, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_SHOW, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_GET, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_IF, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_REPEAT, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_WHILE, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_TOFUNC, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_RETURN, ACTION_REDUCE, 27);
    insertTableEntry(table, 144, TOKEN_EOF, ACTION_REDUCE, 27);
    insertTableEntry(table, 145, TOKEN_DEDENT, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_LET, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_IDENT, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_SET, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_INCREASE, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_DECREASE, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_MULTIPLY, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_DIVIDE, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_SHOW, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_GET, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_IF, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_OTHERWISE, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_REPEAT, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_WHILE, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_TOFUNC, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_RETURN, ACTION_REDUCE, 1);
    insertTableEntry(table, 145, TOKEN_EOF, ACTION_REDUCE, 1);
    insertTableEntry(table, 146, TOKEN_COMMA, ACTION_REDUCE, 41);
    insertTableEntry(table, 146, TOKEN_RPAREN, ACTION_REDUCE, 41);
    insertTableEntry(table, 146, TOKEN_ADD, ACTION_SHIFT, 69);
    insertTableEntry(table, 146, TOKEN_SUB, ACTION_SHIFT, 70);
    insertTableEntry(table, 146, NON_TERMINAL_E_OP, ACTION_GOTO, 68);
    insertTableEntry(table, 147, TOKEN_DEDENT, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_LET, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_IDENT, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_SET, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_INCREASE, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_DECREASE, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_MULTIPLY, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_DIVIDE, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_SHOW, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_GET, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_IF, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_REPEAT, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_WHILE, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_TOFUNC, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_RETURN, ACTION_REDUCE, 22);
    insertTableEntry(table, 147, TOKEN_EOF, ACTION_REDUCE, 22);
    insertTableEntry(table, 148, TOKEN_IDENT, ACTION_SHIFT, 44);
    insertTableEntry(table, 148, TOKEN_STRING, ACTION_SHIFT, 42);
    insertTableEntry(table, 148, TOKEN_LPAREN, ACTION_SHIFT, 56);
    insertTableEntry(table, 148, TOKEN_REMAINDEROF, ACTION_SHIFT, 37);
    insertTableEntry(table, 148, TOKEN_NUMBER, ACTION_SHIFT, 41);
    insertTableEntry(table, 148, TOKEN_NOT, ACTION_SHIFT, 55);
    insertTableEntry(table, 148, NON_TERMINAL_LVAL, ACTION_GOTO, 39);
    insertTableEntry(table, 148, NON_TERMINAL_FUNC_CALL, ACTION_GOTO, 40);
    insertTableEntry(table, 148, NON_TERMINAL_E, ACTION_GOTO, 54);
    insertTableEntry(table, 148, NON_TERMINAL_T, ACTION_GOTO, 36);
    insertTableEntry(table, 148, NON_TERMINAL_F, ACTION_GOTO, 38);
    insertTableEntry(table, 148, NON_TERMINAL_COND, ACTION_GOTO, 150);
    insertTableEntry(table, 148, NON_TERMINAL_BOOL_T, ACTION_GOTO, 52);
    insertTableEntry(table, 148, NON_TERMINAL_BOOL_F, ACTION_GOTO, 53);
    insertTableEntry(table, 149, TOKEN_INDENT, ACTION_SHIFT, 117);
    insertTableEntry(table, 149, NON_TERMINAL_BLOCK, ACTION_GOTO, 151);
    insertTableEntry(table, 150, TOKEN_THEN, ACTION_SHIFT, 152);
    insertTableEntry(table, 150, TOKEN_OR, ACTION_SHIFT, 80);
    insertTableEntry(table, 151, TOKEN_DEDENT, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_LET, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_IDENT, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_SET, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_INCREASE, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_DECREASE, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_MULTIPLY, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_DIVIDE, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_SHOW, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_GET, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_IF, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_REPEAT, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_WHILE, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_TOFUNC, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_RETURN, ACTION_REDUCE, 28);
    insertTableEntry(table, 151, TOKEN_EOF, ACTION_REDUCE, 28);
    insertTableEntry(table, 152, TOKEN_COLON, ACTION_SHIFT, 153);
    insertTableEntry(table, 153, TOKEN_INDENT, ACTION_SHIFT, 117);
    insertTableEntry(table, 153, NON_TERMINAL_BLOCK, ACTION_GOTO, 154);
    insertTableEntry(table, 154, TOKEN_DEDENT, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_LET, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_IDENT, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_SET, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_INCREASE, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_DECREASE, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_MULTIPLY, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_DIVIDE, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_SHOW, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_GET, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_IF, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_OTHERWISE, ACTION_SHIFT, 143);
    insertTableEntry(table, 154, TOKEN_REPEAT, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_WHILE, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_TOFUNC, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_RETURN, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, TOKEN_EOF, ACTION_REDUCE, 29);
    insertTableEntry(table, 154, NON_TERMINAL_ELIF_LIST, ACTION_GOTO, 155);
    insertTableEntry(table, 154, NON_TERMINAL_ELSE_PART, ACTION_GOTO, 144);
    insertTableEntry(table, 155, TOKEN_DEDENT, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_LET, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_IDENT, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_SET, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_INCREASE, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_DECREASE, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_MULTIPLY, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_DIVIDE, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_SHOW, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_GET, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_IF, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_REPEAT, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_WHILE, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_TOFUNC, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_RETURN, ACTION_REDUCE, 26);
    insertTableEntry(table, 155, TOKEN_EOF, ACTION_REDUCE, 26);
}