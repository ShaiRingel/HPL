#include "Parser.h"
#include "ErrorHandler.h"
#include <stdlib.h>
#include <assert.h>

Parser* initParser() {
    Parser* parser = (Parser*)malloc(sizeof(Parser));
    ParsingStackItem item;

    if (!parser)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for Parser");

    assert(parser);
    parser->table = initParsingTable();
    parser->stack = initParsingStack();
    parser->cst = createCSTNode(0, 0, NULL);
    parser->lastErrorTokenType = TOKEN_IDLE;
    parser->lastErrorCount = 0;

    /* Push the initial state-0 item onto the stack */
    item.state = 0;
    item.token.type = TOKEN_EOF;
    item.token.lexeme = "START";
    item.token.line = 0;
    item.cstNode = createCSTNode(0, 0, NULL);
    shift(&(parser->stack), item);

    return parser;
}

/* Push the current token and move to the shift target state */
static void shiftAction(ParsingStack** stack, ParseAction action,
    Token* token) {
    ParsingStackItem item;
    item.token = *token;
    item.state = (unsigned short)action.value;
    item.cstNode = createCSTNode(TERMINAL, token->type, token);
    shift(stack, item);
}

/* Pop rhs symbols, build a subtree, return it and set lhs to the rule's LHS */
static CSTNode* reduceAction(ParsingStack** stack, const ParsingTable* table,
    ParseAction action, int* lhs) {
    int ruleLen = getRuleLength(table, action.value);
    *lhs = getLHS(table, action.value);
    return reduce(stack, ruleLen, *lhs);
}

/* Push the new non-terminal node with the GOTO target state */
static void gotoAction(ParsingStack** stack, ParseAction action, int lhs,
    CSTNode* subtree) {
    ParsingStackItem item;
    item.token.type = (TokenTypes)lhs;
    item.token.lexeme = (char*)nonTerminalNames[lhs - NON_TERMINAL_PROG];
    item.token.line = 0;
    item.state = (unsigned short)action.value;
    item.cstNode = subtree;
    shift(stack, item);
}

/*
 * Process one step of the LR automaton.
 *
 * Returns 1 when the input is fully accepted (parser is done).
 * The `next` flag tells the caller whether to fetch a new token before
 * the next call: 1 = fetch, 0 = reuse the current token (reduce actions
 * do not consume input).
 */
int nextAction(Parser* parser, Lexer* lexer, Token** tokenRef, int* next) {
    ParsingStackItem top;
    ParsingStack** stack;
    ParseAction action;
    CSTNode* subTree;
    int lhs;
    int accepted = 0;

    *next = 1;
    stack = &parser->stack;
    top = lookahead(*stack);
    action = getEntry(parser->table, top.state, (*tokenRef)->type);

    switch (action.type) {
    case ACTION_SHIFT:
        shiftAction(stack, action, *tokenRef);
        break;

    case ACTION_REDUCE:
        *next = 0; /* reduce does not consume a token */
        subTree = reduceAction(stack, parser->table, action, &lhs);
        top = lookahead(*stack);
        action = getEntry(parser->table, top.state, lhs);

        if (action.type == ACTION_GOTO)
            gotoAction(stack, action, lhs, subTree);
        else
            reportError(ERROR_INTERNAL, (*tokenRef)->line,
                "Invalid GOTO action in parser");
        break;

    case ACTION_ACCEPT:
        /* Detach the root CST node from the stack and store it on the parser */
        free(parser->cst);
        parser->cst = (*stack)->value.cstNode;
        (*stack)->value.cstNode = NULL;
        accepted = 1;
        break;

    case ACTION_ERROR:
    default: {
        if ((*tokenRef)->type == TOKEN_EOF)
            reportError(ERROR_SYNTAX, (*tokenRef)->line,
                "Unexpected end of file. The file may be empty or "
                "a statement is incomplete.");

        /*
         * Duplicate-error suppression: if the same bad token triggers two
         * consecutive errors, stop reporting it and advance past it instead.
         */
        if ((*tokenRef)->type == parser->lastErrorTokenType) {
            parser->lastErrorCount++;
            if (parser->lastErrorCount >= 2) {
                parser->lastErrorTokenType = TOKEN_IDLE;
                parser->lastErrorCount = 0;
                *next = 1;
            }
        }
        else {
            parser->lastErrorTokenType = (*tokenRef)->type;
            parser->lastErrorCount = 1;
        }

        if (!(*next == 1 && parser->lastErrorCount == 0)) {
            const char* expectedHint = getExpectedTokenMessage(top.state);
            if (expectedHint)
                logError(ERROR_SYNTAX, (*tokenRef)->line,
                    "Unexpected token '%s'. %s.",
                    (*tokenRef)->lexeme, expectedHint);
            else
                logError(ERROR_SYNTAX, (*tokenRef)->line,
                    "Unexpected token '%s' at state %d. "
                    "Entering panic mode.",
                    (*tokenRef)->lexeme, top.state);

            panicModeRecovery(tokenRef, next);
        }
        break;
    }
    }

    return accepted;
}

void freeParser(Parser* parser) {
    freeParsingTable(parser->table);
    freeParsingStack(parser->stack);
    freeCST(parser->cst);
    free(parser);
}