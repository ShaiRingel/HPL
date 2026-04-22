#pragma once
#include "SymbolTable.h"

/*
 * ScopeTree — a tree of lexical scopes built during semantic analysis.
 *
 * Each ScopeNode owns a SymbolTable for symbols declared directly in that
 * scope.  Lookup walks up the parent chain, so inner scopes shadow outer
 * ones automatically.
 *
 * The children array is grown dynamically; capacity doubles whenever it
 * fills up.
 */
typedef struct ScopeNode {
    SymbolTable* symbols;
    struct ScopeNode* parent;
    struct ScopeNode** children;
    int numChildren;
    int capacity;
} ScopeNode;

/* Create a new scope; pass NULL for parent to create the global root scope */
ScopeNode* createScope(ScopeNode* parent);

/* Add child as a nested scope of parent */
void addChildScope(ScopeNode* parent, ScopeNode* child);

/* Search currentScope and all enclosing scopes for key */
SymbolData lookupSymbol(ScopeNode* currentScope, char* key);

void freeScopeTree(ScopeNode* node);
