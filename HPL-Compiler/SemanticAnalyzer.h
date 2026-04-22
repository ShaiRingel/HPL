#pragma once
#include "ScopeTree.h"
#include "CST.h"
#include "DispatchTable.h"

/*
 * SemanticAnalyzer — walks the CST and enforces type rules.
 *
 * A dispatch table maps each NonTerminal to a dedicated handler function,
 * keeping the main analyzeNode() loop clean.
 *
 * currentFunctionData points into the scope tree's symbol storage and is
 * used to record and verify return types while processing function bodies.
 */
typedef struct {
    ScopeNode* rootScope;
    ScopeNode* currentScope;
    SymbolData* currentFunctionData; /* non-NULL while inside a function body */
    DispatchTable* nodeDispatch;
} SemanticAnalyzer;

SemanticAnalyzer* initSemanticAnalyzer();
void freeSemanticAnalyzer(SemanticAnalyzer* analyzer);
void analyzeCST(SemanticAnalyzer* analyzer, CSTNode* root);
void analyzeNode(SemanticAnalyzer* analyzer, CSTNode* node);
const char* tokenTypeToName(TokenTypes type); /* "NUMBER", "TEXT", or "VOID/UNKNOWN" */
