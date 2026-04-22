#include "CST.h"
#include "ErrorHandler.h"
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

CSTNode* createCSTNode(SymbolType type, int symbol, Token* token) {
    CSTNode* node = (CSTNode*)malloc(sizeof(CSTNode));

    if (!node)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for CST Node");

    assert(node);
    node->type = type;
    node->symbol = symbol;
    node->suLabel = 0;
    node->token = token;

    node->firstChild  = NULL;
    node->nextSibling = NULL;

    return node;
}

/* Appends child to the end of parent's child list */
void addChild(CSTNode* parent, CSTNode* child) {
    CSTNode* sibling;

    if (!parent) reportError(ERROR_INTERNAL, 0, "addChild: parent is NULL");
    if (!child)  reportError(ERROR_INTERNAL, 0, "addChild: child is NULL");

    if (parent && child) {
        if (parent->firstChild == NULL) {
            parent->firstChild = child;
        } else {
            /* Walk to the last sibling and attach there */
            sibling = parent->firstChild;
            while (sibling->nextSibling)
                sibling = sibling->nextSibling;
            sibling->nextSibling = child;
        }
    }
}

/* Recursively frees the entire subtree rooted at node */
void freeCST(CSTNode* node) {
    if (!node) return;

    freeCST(node->firstChild);
    freeCST(node->nextSibling);

    if (node->token != NULL) {
        free(node->token->lexeme);
        free(node->token);
    }

    free(node);
}

/* Recursive helper — prints one level, indented by 2 spaces per level */
void printCSTLevel(CSTNode* node, int level) {
    CSTNode* child;
    int i;

    if (!node) return;

    for (i = 0; i < level; i++) printf("  ");

    if (node->type == TERMINAL && node->token)
        printf("Leaf: %s (token: %d, su: %d)\n", node->token->lexeme, node->token->type, node->suLabel);
    else if (node->symbol >= NON_TERMINAL_PROG && node->symbol < NON_TERMINAL_PROG + 100)
        printf("Node: %s, su: %d\n", nonTerminalNames[node->symbol - NON_TERMINAL_PROG], node->suLabel);
    else
        printf("Node: (symbol: %d, su: %d)\n", node->symbol, node->suLabel);

    child = node->firstChild;
    while (child != NULL) {
        printCSTLevel(child, level + 1);
        child = child->nextSibling;
    }
}

void printCST(CSTNode* node) {
    if (!node) {
        printf("CST is empty.\n");
        return;
    }
    printf("\n--- Concrete Syntax Tree ---\n");
    printCSTLevel(node, 0);
    printf("----------------------------\n\n");
}
