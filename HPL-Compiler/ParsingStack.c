#include "ParsingStack.h"
#include "ErrorHandler.h"
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/* Removes the top node and returns its CST child — does NOT free the node's token */
CSTNode* pop(ParsingStack** stack) {
    ParsingStack* node;
    CSTNode* cstNode;

    if (!stack || !*stack) return NULL;

    node = *stack;
    cstNode = node->value.cstNode;
    *stack = node->next;
    free(node);
    return cstNode;
}

ParsingStack* initParsingStack() {
    return NULL; /* an empty stack is just a NULL pointer */
}

void shift(ParsingStack** stack, ParsingStackItem item) {
    ParsingStack* node;

    if (!stack)
        reportError(ERROR_INTERNAL, 0, "Invalid stack pointer");

    node = malloc(sizeof(ParsingStack));
    if (!node)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for Parsing Stack");

    assert(stack);
    assert(node);
    node->value = item;
    node->next = *stack;
    *stack = node;
}

/*
 * Pops `amount` items, collects their CST nodes in order, attaches them
 * as children of a new NON_TERMINAL node, and returns that node.
 * The caller is responsible for then pushing a GOTO item that holds it.
 */
CSTNode* reduce(ParsingStack** stack, int amount, int lhs) {
    int i, j;
    CSTNode* parent;
    CSTNode** children;
    int stackOk = 1;

    parent = createCSTNode(NON_TERMINAL, lhs, NULL);

    if (amount > 0) {
        children = malloc(sizeof(CSTNode*) * amount);
        if (!children)
            reportError(ERROR_INTERNAL, 0,
                        "Failed to allocate memory for children array in reduce");

        assert(children);

        /* Pop items in reverse so children end up in left-to-right order */
        for (i = amount - 1; i >= 0 && stackOk; i--) {
            if (!stack || !*stack) {
                logError(ERROR_SYNTAX, 0,
                         "Reduce operation attempted beyond stack size");

                for (j = i + 1; j < amount; j++)
                    if (children[j]) freeCST(children[j]);

                amount = i;
                stackOk = 0;
            } else {
                children[i] = pop(stack);
            }
        }

        for (i = 0; i < amount; i++)
            addChild(parent, children[i]);

        free(children);
    }

    return parent;
}

ParsingStackItem lookahead(ParsingStack* stack) {
    if (!stack)
        reportError(ERROR_INTERNAL, 0, "Cannot peek from empty stack");

    assert(stack);
    return stack->value;
}

void freeParsingStack(ParsingStack* stack) {
    while (stack) {
        ParsingStack* next = stack->next;
        free(stack);
        stack = next;
    }
}

/* Prints the stack bottom-up so the output reads left-to-right */
static void printStackRecursive(ParsingStack* node) {
    if (node == NULL) return;

    printStackRecursive(node->next);

    if (node->value.state == 0 && node->next == NULL)
        printf("%d ", node->value.state);
    else
        printf("%s %d ", node->value.token.lexeme, node->value.state);
}

void printStack(ParsingStack* stack) {
    printStackRecursive(stack);
    printf("\n\n");
}
