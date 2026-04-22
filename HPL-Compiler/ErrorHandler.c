#include "HashingFunctions.h"
#include "ErrorHandler.h"
#include "Global.h"
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

/* ----------------------------------------------------------------
 * State–Message Map (SMM)
 * Maps parser state numbers to human-readable "expected …" strings.
 * Built lazily on the first call to smmLookup().
 * ---------------------------------------------------------------- */

SMMEntry* s_smm_buckets[SMM_CAPACITY];
static int s_smm_built = 0;

static const struct { unsigned short state; const char* message; } s_smm_entries[] = {
    { 0,   "Expected the start of a statement. Valid keywords are: "
           "'Let', 'Set', 'Increase', 'Decrease', 'Multiply', 'Divide', "
           "'Show', 'Get', 'If', 'Repeat', 'While', 'Return', 'To', "
           "or an identifier for a function call" },
    { 1,   "Expected the start of a statement. Valid keywords are: "
           "'Let', 'Set', 'Increase', 'Decrease', 'Multiply', 'Divide', "
           "'Show', 'Get', 'If', 'Repeat', 'While', 'Return', 'To', "
           "or an identifier for a function call" },
    { 118, "Expected the start of a statement. Valid keywords are: "
           "'Let', 'Set', 'Increase', 'Decrease', 'Multiply', 'Divide', "
           "'Show', 'Get', 'If', 'Repeat', 'While', 'Return', 'To', "
           "or an identifier for a function call" },
    { 136, "Expected the start of a statement. Valid keywords are: "
           "'Let', 'Set', 'Increase', 'Decrease', 'Multiply', 'Divide', "
           "'Show', 'Get', 'If', 'Repeat', 'While', 'Return', 'To', "
           "or an identifier for a function call" },
    { 10,  "Expected a variable name after 'Let'" },
    { 28,  "Expected 'be' after the variable name in a declaration (e.g. 'Let x be number.')" },
    { 17,  "Expected a type ('number' or 'text') after 'Get' (e.g. 'Get number ask ...')" },
    { 60,  "Expected a type ('number' or 'text') after 'be'" },
    { 22,  "Expected '.' to end the variable declaration" },
    { 11,  "Expected a variable name after the assignment keyword" },
    { 12,  "Expected a variable name after the assignment keyword" },
    { 13,  "Expected a variable name after the assignment keyword" },
    { 14,  "Expected a variable name after the assignment keyword" },
    { 15,  "Expected a variable name after the assignment keyword" },
    { 29,  "Expected 'to' after the variable name in 'Set' (e.g. 'Set x to 5.')" },
    { 31,  "Expected 'by' after the variable name (e.g. 'Increase x by 1.')" },
    { 32,  "Expected 'by' after the variable name (e.g. 'Increase x by 1.')" },
    { 33,  "Expected 'by' after the variable name (e.g. 'Increase x by 1.')" },
    { 34,  "Expected 'by' after the variable name (e.g. 'Increase x by 1.')" },
    { 30,  "Expected 'atPosition' for array indexing, or a keyword/operator to continue the statement" },
    { 35,  "Expected '.' to end the statement, or 'plus'/'minus' to continue the expression" },
    { 49,  "Expected '.' to end the statement, or 'plus'/'minus' to continue the expression" },
    { 97,  "Expected an operator or '.' to continue after the array access" },
    { 99,  "Expected '.' to end the statement, or 'plus'/'minus' to continue the expression" },
    { 100, "Expected '.' to end the statement, or 'plus'/'minus' to continue the expression" },
    { 101, "Expected '.' to end the statement, or 'plus'/'minus' to continue the expression" },
    { 102, "Expected '.' to end the statement, or 'plus'/'minus' to continue the expression" },
    { 96,  "Expected '.' to end the statement" },
    { 141, "Expected '.' to end the statement" },
    { 16,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 21,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 24,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 43,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 61,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 63,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 64,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 65,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 66,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 68,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 83,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 98,  "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 138, "Expected an expression: a number, a string in quotes, a variable name, 'remainderOf', or '('" },
    { 23,  "Expected a condition: a number, string, variable, 'remainderOf', '(', or 'not'" },
    { 25,  "Expected a condition: a number, string, variable, 'remainderOf', '(', or 'not'" },
    { 55,  "Expected a condition: a number, string, variable, 'remainderOf', '(', or 'not'" },
    { 56,  "Expected a condition: a number, string, variable, 'remainderOf', '(', or 'not'" },
    { 81,  "Expected a condition: a number, string, variable, 'remainderOf', '(', or 'not'" },
    { 82,  "Expected a condition: a number, string, variable, 'remainderOf', '(', or 'not'" },
    { 151, "Expected a condition: a number, string, variable, 'remainderOf', '(', or 'not'" },
    { 36,  "Expected 'times' or 'divide' in a multiplication/division expression" },
    { 103, "Expected 'times' or 'divide' in a multiplication/division expression" },
    { 57,  "Expected 'iterations' after the repeat count (e.g. 'Repeat 5 iterations:')" },
    { 75,  "Expected ')', 'plus', or 'minus' after the expression inside parentheses" },
    { 37,  "Expected a value (number, string, or variable name) after the operator" },
    { 72,  "Expected a value (number, string, or variable name) after the operator" },
    { 105, "Expected a value (number, string, or variable name) after the operator" },
    { 20,  "Expected the function name after 'To'" },
    { 45,  "Expected 'ask' after the type in a 'Get' statement (e.g. 'Get number ask \"Prompt:\" storeInto x.')" },
    { 77,  "Expected a string literal (in double quotes) after 'ask' for the prompt text" },
    { 130, "Expected a parameter name after ',' (e.g. 'To myFunc with x, y:')" },
    { 132, "Expected 'Otherwise' for an else/else-if clause, or the next statement after the if block" },
    { 48,  "Expected 'with' after the function name in a declaration (e.g. 'To myFunc with x:')" },
    { 59,  "Expected '(' before the argument list (e.g. 'myFunc with (arg1, arg2).')" },
    { 62,  "Expected an index value after 'atPosition' (e.g. 'x atPosition 1')" },
    { 119, "Expected ',' to add another argument, or ')' to close the list" },
    { 120, "Expected ')' to close the argument list" },
    { 26,  "Expected 'with' to call a function (e.g. 'myFunc with (arg1, arg2).')" },
    { 51,  "Expected 'then' or 'or' after the condition" },
    { 153, "Expected 'then' or 'or' after the condition" },
    { 52,  "Expected 'and' to chain conditions" },
    { 112, "Expected 'and' to chain conditions" },
    { 54,  "Expected a relational operator: 'equalsTo', 'notEqualsTo', "
           "'greaterThan', 'lessThan', 'atLeast', or 'atMost'" },
    { 91,  "Expected 'or' to add another condition, or ')' to close the group" },
    { 76,  "Expected 'plus', 'minus', or ')' after the expression" },
    { 124, "Expected 'plus', 'minus', or ')' after the expression" },
    { 80,  "Expected ':' to open the block" },
    { 92,  "Expected ':' after 'iterations' (e.g. 'Repeat 5 iterations:')" },
    { 93,  "Expected ':' to open the block" },
    { 109, "Expected ':' to open the function body, or ',' to add another parameter (e.g. 'To myFunc with x, y:')" },
    { 155, "Expected ':' to open the block" },
    { 94,  "Expected an indented block after ':'" },
    { 111, "Expected an indented block after ':'" },
    { 116, "Expected an indented block after ':'" },
    { 131, "Expected an indented block after ':'" },
    { 133, "Expected an indented block after ':'" },
    { 149, "Expected an indented block after ':'" },
    { 152, "Expected an indented block after ':'" },
    { 156, "Expected an indented block after ':'" },
    { 134, "Expected the next statement or end of block" },
    { 157, "Expected the next statement or end of block" },
    { 58,  "Expected ':' to open the While loop body, or 'or' to extend the condition" },
    { 46,  "Expected '.' to end the statement, or 'ask' for a Get input "
           "statement (e.g. 'Get number ask \"Prompt:\" storeInto x.')" },
    { 74,  "Expected 'dividedBy' after the value in 'remainderOf' "
           "(e.g. 'remainderOf 10 dividedBy 3')" },
    { 79,  "Expected ':' to open the 'If' block after 'then' "
           "(e.g. 'If x greaterThan 5 then:')" },
    { 106, "Expected 'storeInto' after the prompt string in a 'Get' "
           "statement (e.g. 'Get number ask \"Enter:\" storeInto x.')" },
    { 110, "Expected an indented block after 'then:'" },
    { 128, "Expected a variable name after 'storeInto'" },
    { 143, "Expected ':' for an 'Otherwise' block, or 'if' for an "
           "'Otherwise if' clause (e.g. 'Otherwise:' or 'Otherwise if ...')" },
};
#define S_SMM_COUNT ((int)(sizeof(s_smm_entries)/sizeof(s_smm_entries[0])))

static void smmBuild() {
    int i;
    unsigned idx;
    SMMEntry* entry;

    if (s_smm_built) return;
    s_smm_built = 1;

    for (i = 0; i < S_SMM_COUNT; i++) {
        entry = (SMMEntry*)malloc(sizeof(SMMEntry));
        if (!entry) reportError(ERROR_INTERNAL, 0, "Failed to allocate SMMEntry");
        entry->state   = s_smm_entries[i].state;
        entry->message = s_smm_entries[i].message;
        idx = hashNumber(entry->state, SMM_CAPACITY);
        entry->next = s_smm_buckets[idx];
        s_smm_buckets[idx] = entry;
    }
}

const char* smmLookup(unsigned short state) {
    SMMEntry* e;
    smmBuild();
    e = s_smm_buckets[hashNumber(state, SMM_CAPACITY)];
    while (e) {
        if (e->state == state) return e->message;
        e = e->next;
    }
    return NULL;
}

/* ----------------------------------------------------------------
 * Global error state
 * ---------------------------------------------------------------- */

Compiler* compiler    = NULL;
unsigned errorCount   = 0;
unsigned g_currentLine = 0;

static const char* errorPrefixes[] = {
    "[Lexical Error] ",
    "[Syntax Error] ",
    "[Semantic Error] ",
    "[Internal Error] ",
    "[General Error] "
};
#define ERROR_PREFIX_COUNT ((int)(sizeof(errorPrefixes)/sizeof(errorPrefixes[0])))

static void printErrorPrefix(ErrorType type) {
    int idx = (int)type;
    if (idx >= 0 && idx < ERROR_PREFIX_COUNT)
        printf("%s", errorPrefixes[idx]);
    else
        printf("[General Error] ");
}

void setCurrentCompiler(Compiler* comp) {
    compiler = comp;
}

/* ----------------------------------------------------------------
 * Error reporting
 * ---------------------------------------------------------------- */

void reportError(ErrorType type, unsigned line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    printf(RED);
    printErrorPrefix(type);
    if (line > 0) printf("Line %u: ", line);
    vprintf(format, args);
    printf(RESET "\n\n");
    va_end(args);

    if (compiler)
        freeCompiler(compiler);

    printf(RED "--- Compilation Complete: %d errors found ---\n\n" RESET,
           errorCount + 1);
    exit(EXIT_FAILURE);
}

void logError(ErrorType type, unsigned line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    printf(RED);
    printErrorPrefix(type);
    if (line > 0) printf("Line %u: ", line);
    vprintf(format, args);
    printf(RESET "\n\n");
    va_end(args);
    errorCount++;
}

/* ----------------------------------------------------------------
 * Panic-mode recovery
 * ---------------------------------------------------------------- */

/* Advance past the current token, skipping newlines */
static void getNextToken(Lexer* lexer, Token** currentToken) {
    if (*currentToken) {
        free((*currentToken)->lexeme);
        free(*currentToken);
    }
    *currentToken = nextToken(lexer);
    while (*currentToken && (*currentToken)->type == TOKEN_NEWLINE) {
        free((*currentToken)->lexeme);
        free(*currentToken);
        *currentToken = nextToken(lexer);
    }
}

/*
 * Panic-mode recovery — called after a syntax error.
 *
 * Phase 1: discard tokens until a synchronising token is found.
 * Phase 2: pop the parse stack until the automaton can legally shift
 *          or reduce on the current token, or until the stack is empty.
 *
 * On return, *next indicates whether the caller should fetch a fresh
 * token (1) or reuse the one currently in *currentToken (0).
 */
void panicModeRecovery(Token** currentToken, int* next) {
    ParsingStackItem top;
    ParseAction action;
    CSTNode* discardedNode;
    Lexer* lexer   = compiler->lexer;
    Parser* parser = compiler->parser;

    *next = 1;

    /* Phase 1: skip until a statement/block boundary */
    while ((*currentToken)->type != TOKEN_EOF &&
           synchronizingTokens((*currentToken)->type))
        getNextToken(lexer, currentToken);

    /* Phase 2: pop until a valid action exists for the current token */
    if ((*currentToken)->type != TOKEN_EOF) {
        int recovered = 0;
        while (parser->stack != NULL && !recovered) {
            top    = lookahead(parser->stack);
            action = getEntry(parser->table, top.state, (*currentToken)->type);

            if (action.type != ACTION_ERROR) {
                *next     = 0;
                recovered = 1;
            } else if (parser->stack->next == NULL) {
                recovered = 1; /* bottom of stack — nothing more to pop */
            } else {
                discardedNode = pop(&(parser->stack));
                if (discardedNode)
                    freeCST(discardedNode);
            }
        }
    }
}

const char* getExpectedTokenMessage(unsigned short state) {
    return smmLookup(state);
}
