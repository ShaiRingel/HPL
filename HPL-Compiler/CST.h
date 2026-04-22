#pragma once
#include "Global.h"
#include "ParsingDefinitions.h"

/*
 * CSTNode — a node in the Concrete Syntax Tree built by the parser.
 *
 * Children are stored as a singly-linked list hanging off firstChild;
 * siblings are chained through nextSibling.  This "left-child right-sibling"
 * layout lets a node have an arbitrary number of children without allocating
 * a separate array.
 *
 *   type    : TERMINAL for leaf nodes (hold a Token), NON_TERMINAL for inner nodes
 *   symbol  : the TokenTypes value (terminal) or NonTerminal value (non-terminal)
 *   token   : non-NULL only for terminal nodes
 *   suLabel : Sethi-Ullman register-need label, stamped by the semantic analyser.
 *             Equals the minimum number of registers needed to evaluate this
 *             subtree without any stack spills.  0 means not yet labelled.
 */
typedef struct CSTNode {
    SymbolType type;
    int symbol;
    int suLabel;
    Token* token;
    struct CSTNode* firstChild;
    struct CSTNode* nextSibling;
} CSTNode;

CSTNode* createCSTNode(SymbolType type, int symbol, Token* token);
void addChild(CSTNode* parent, CSTNode* child);
void freeCST(CSTNode* node);
void printCSTLevel(CSTNode* node, int level); /* recursive helper used by printCST */
void printCST(CSTNode* node);
