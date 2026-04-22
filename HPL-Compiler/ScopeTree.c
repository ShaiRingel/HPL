#include "ScopeTree.h"
#include "ErrorHandler.h"
#include <stdlib.h>
#include <assert.h>

ScopeNode* createScope(ScopeNode* parent) {
    ScopeNode* node = (ScopeNode*)malloc(sizeof(ScopeNode));
    if (!node)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate ScopeNode");

    assert(node);
    node->symbols = initSymbolTable();
    node->parent = parent;
    node->capacity = 4; /* start small; doubles when full */
    node->numChildren = 0;
    node->children = (ScopeNode**)malloc(node->capacity * sizeof(ScopeNode*));

    if (!node->children)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate children for ScopeNode");

    return node;
}

void addChildScope(ScopeNode* parent, ScopeNode* child) {
    if (!parent || !child) return;

    if (parent->numChildren >= parent->capacity) {
        parent->capacity *= 2;

        assert(parent->children);

        parent->children = (ScopeNode**)realloc(parent->children,
                                                 parent->capacity * sizeof(ScopeNode*));
        if (!parent->children)
            reportError(ERROR_INTERNAL, 0,
                        "Failed to reallocate children array for scope");
    }

    assert(parent->children);
    parent->children[parent->numChildren++] = child;
}

/*
 * Walks up the parent chain until the symbol is found or the root is
 * reached.  Returns a zeroed SymbolData when the symbol is not declared
 * in any enclosing scope.
 */
SymbolData lookupSymbol(ScopeNode* currentScope, char* key) {
    SymbolData result = { 0 };

    if (currentScope) {
        result = getSymbol(currentScope->symbols, key);

        if (result.type == TYPE_NONE && currentScope->parent)
            result = lookupSymbol(currentScope->parent, key);
    }

    return result;
}

void freeScopeTree(ScopeNode* node) {
    int i;
    if (!node) return;

    for (i = 0; i < node->numChildren; i++)
        freeScopeTree(node->children[i]);

    freeSymbolTable(node->symbols);
    free(node->children);
    free(node);
}
