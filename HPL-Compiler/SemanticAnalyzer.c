#include "SemanticAnalyzer.h"
#include "DispatchTable.h"
#include "ErrorHandler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ----------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------- */
static unsigned getNodeLine(CSTNode* node);
static void enterScope(SemanticAnalyzer* analyzer);
static void exitScope(SemanticAnalyzer* analyzer);
static void addIdentifier(SemanticAnalyzer* analyzer, char* lexeme,
    VarType type, TokenTypes dataType, unsigned line);
static SymbolData lookupSymbolOrError(SemanticAnalyzer* analyzer,
    char* lexeme, unsigned line);
static TokenTypes getExpressionType(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleExpressions(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleVarDecl(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleFuncDecl(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleLVal(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleAssignment(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleFuncCallStmt(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleFuncRet(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleIO(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleBlock(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleConditional(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleLoops(SemanticAnalyzer* analyzer, CSTNode* node);
static void handleCtrlFlow(SemanticAnalyzer* analyzer, CSTNode* node);

typedef void (*SemanticHandler)(SemanticAnalyzer*, CSTNode*);

/* ----------------------------------------------------------------
 * Dispatch table
 * Built once on the first call to analyzeNode(); maps each NonTerminal
 * that needs active checking to its dedicated handler function.
 * Nodes not in the table are handled by the default fallback in
 * analyzeNode() which simply recurses into children.
 * ---------------------------------------------------------------- */
static void buildSemanticDispatch(SemanticAnalyzer* analyzer) {
    if (analyzer->nodeDispatch) return;
    analyzer->nodeDispatch = initDispatchTable(20);
    dispatchSet(analyzer->nodeDispatch, NON_TERMINAL_BLOCK,
        (DispatchFn)handleBlock);
    dispatchSet(analyzer->nodeDispatch, NON_TERMINAL_VAR_DECL,
        (DispatchFn)handleVarDecl);
    dispatchSet(analyzer->nodeDispatch, NON_TERMINAL_FUNC_DECL,
        (DispatchFn)handleFuncDecl);
    dispatchSet(analyzer->nodeDispatch, NON_TERMINAL_ASSIGN_STMT,
        (DispatchFn)handleAssignment);
    dispatchSet(analyzer->nodeDispatch, NON_TERMINAL_IO_STMT,
        (DispatchFn)handleIO);
    dispatchSet(analyzer->nodeDispatch, NON_TERMINAL_CTRL_FLOW,
        (DispatchFn)handleCtrlFlow);
    dispatchSet(analyzer->nodeDispatch, NON_TERMINAL_FUNC_CALL_STMT,
        (DispatchFn)handleFuncCallStmt);
    dispatchSet(analyzer->nodeDispatch, NON_TERMINAL_FUNC_RET,
        (DispatchFn)handleFuncRet);
}

/* ----------------------------------------------------------------
 * Type helpers
 * ---------------------------------------------------------------- */

static const struct { TokenTypes type; const char* name; } s_typeNames[] = {
    { TOKEN_NUMBER,  "NUMBER" },
    { TOKEN_INTEGER, "NUMBER" },
    { TOKEN_TEXT,    "TEXT"   },
    { TOKEN_STRING,  "TEXT"   },
};
#define S_TYPE_NAMES_COUNT ((int)(sizeof(s_typeNames)/sizeof(s_typeNames[0])))

const char* tokenTypeToName(TokenTypes type) {
    int i;
    for (i = 0; i < S_TYPE_NAMES_COUNT; i++)
        if (s_typeNames[i].type == type) return s_typeNames[i].name;
    return "VOID/UNKNOWN";
}

static int isNumberType(TokenTypes type) {
    return (type == TOKEN_NUMBER || type == TOKEN_INTEGER);
}

static int isTextType(TokenTypes type) {
    return (type == TOKEN_TEXT || type == TOKEN_STRING);
}

static int typesMatch(TokenTypes t1, TokenTypes t2) {
    return (isNumberType(t1) && isNumberType(t2)) ||
        (isTextType(t1) && isTextType(t2)) ||
        (t1 == t2);
}

/* Walk down firstChild links to find the nearest token's line number */
static unsigned getNodeLine(CSTNode* node) {
    if (!node) return 0;
    if (node->token) return node->token->line;
    return getNodeLine(node->firstChild);
}

/* ----------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------- */

SemanticAnalyzer* initSemanticAnalyzer() {
    SemanticAnalyzer* analyzer =
        (SemanticAnalyzer*)malloc(sizeof(SemanticAnalyzer));
    if (!analyzer)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate Semantic Analyzer");
    analyzer->rootScope = createScope(NULL);
    analyzer->currentScope = analyzer->rootScope;
    analyzer->currentFunctionData = NULL;
    analyzer->nodeDispatch = NULL;
    return analyzer;
}

void freeSemanticAnalyzer(SemanticAnalyzer* analyzer) {
    freeScopeTree(analyzer->rootScope);
    if (analyzer->nodeDispatch)
        freeDispatchTable(analyzer->nodeDispatch);
    free(analyzer);
}

/* ----------------------------------------------------------------
 * Scope management
 * ---------------------------------------------------------------- */

static void enterScope(SemanticAnalyzer* analyzer) {
    ScopeNode* newScope = createScope(analyzer->currentScope);
    addChildScope(analyzer->currentScope, newScope);
    analyzer->currentScope = newScope;
}

static void exitScope(SemanticAnalyzer* analyzer) {
    if (analyzer->currentScope->parent)
        analyzer->currentScope = analyzer->currentScope->parent;
}

/* ----------------------------------------------------------------
 * Symbol helpers
 * ---------------------------------------------------------------- */

static void addIdentifier(SemanticAnalyzer* analyzer, char* lexeme,
    VarType type, TokenTypes dataType, unsigned line) {
    SymbolData data = getSymbol(analyzer->currentScope->symbols, lexeme);
    if (data.type != TYPE_NONE) {
        logError(ERROR_SEMANTIC, line,
            "Variable '%s' is already declared in this scope", lexeme);
        return;
    }
    data.type = type;
    data.dataType = dataType;
    data.value = 1;
    data.isArray = 0;
    data.numParams = 0;
    putSymbol(analyzer->currentScope->symbols, lexeme, data);
}

static SymbolData lookupSymbolOrError(SemanticAnalyzer* analyzer,
    char* lexeme, unsigned line) {
    SymbolData data = lookupSymbol(analyzer->currentScope, lexeme);
    if (data.type == TYPE_NONE)
        logError(ERROR_SEMANTIC, line, "Undeclared identifier '%s'", lexeme);
    return data;
}

/* ----------------------------------------------------------------
 * Parameter type inference
 *
 * When a function parameter has no explicit type annotation, we try to
 * infer its type from how it is used.  inferParamType() walks an
 * expression node down to its first LVAL leaf and, if that leaf is an
 * untyped parameter, stamps it with `inferredType`.
 *
 * inferParamTypesFromCondition() applies the same logic to comparison
 * operands: if an operator requires numbers (>, <, >=, <=) we can infer
 * both sides; for == and != we propagate the known side to the unknown one.
 * ---------------------------------------------------------------- */

static void inferParamType(SemanticAnalyzer* analyzer, CSTNode* exprNode,
    TokenTypes inferredType) {
    CSTNode* cur = exprNode;
    while (cur) {
        if (cur->symbol == NON_TERMINAL_LVAL) {
            CSTNode* identNode = cur->firstChild;
            if (identNode && identNode->token) {
                ScopeNode* scope = analyzer->currentScope;
                SymbolData* ptr = NULL;
                while (scope && ptr == NULL) {
                    ptr = getSymbolPointer(scope->symbols,
                        identNode->token->lexeme);
                    if (ptr == NULL) scope = scope->parent;
                }
                if (ptr && ptr->type == TYPE_PARAM && ptr->dataType == 0)
                    ptr->dataType = inferredType;
            }
            return;
        }
        /* Keep descending as long as there is exactly one child */
        if (cur->firstChild && cur->firstChild->nextSibling == NULL)
            cur = cur->firstChild;
        else
            break;
    }
}

static void inferParamTypesFromCondition(SemanticAnalyzer* analyzer,
    CSTNode* node) {
    CSTNode* child;
    if (!node) return;

    if (node->symbol == NON_TERMINAL_BOOL_F) {
        CSTNode* lhs = node->firstChild;
        CSTNode* relOp = lhs ? lhs->nextSibling : NULL;
        CSTNode* rhs = relOp ? relOp->nextSibling : NULL;

        if (relOp && relOp->firstChild && relOp->firstChild->token) {
            TokenTypes op = relOp->firstChild->token->type;
            if (op == TOKEN_GT || op == TOKEN_LT ||
                op == TOKEN_ATLEAST || op == TOKEN_ATMOST) {
                inferParamType(analyzer, lhs, TOKEN_NUMBER);
                inferParamType(analyzer, rhs, TOKEN_NUMBER);
            }
            else if (op == TOKEN_EQTO || op == TOKEN_NOTEQ) {
                TokenTypes lhsType = getExpressionType(analyzer, lhs);
                TokenTypes rhsType = getExpressionType(analyzer, rhs);
                if (lhsType != 0 && rhsType == 0)
                    inferParamType(analyzer, rhs, lhsType);
                else if (rhsType != 0 && lhsType == 0)
                    inferParamType(analyzer, lhs, rhsType);
            }
        }
        return;
    }

    child = node->firstChild;
    while (child) {
        inferParamTypesFromCondition(analyzer, child);
        child = child->nextSibling;
    }
}


/* ----------------------------------------------------------------
 * Expression type resolution
 *
 * Returns the TokenTypes value (TOKEN_NUMBER or TOKEN_TEXT) for the
 * expression rooted at `node`, or 0 if indeterminate (e.g. untyped param).
 *
 * Sethi-Ullman labelling is performed inline: each case stamps
 * node->suLabel with the minimum number of registers needed to evaluate
 * this subtree spill-free, using the labels already computed on children
 * by the recursive calls that precede each return.  No second traversal
 * is needed — the label falls out naturally from the recursion.
 *
 * Label rules (k = RA_POOL_SIZE + 1 marks an opaque/expensive node):
 *   LVAL, FUNC_CALL          -> k  (memory load or call — treat as heavy)
 *   literal token            -> 1
 *   unary / passthrough      -> child.suLabel
 *   binary, l == r           -> l + 1
 *   binary, l != r           -> max(l, r)
 * ---------------------------------------------------------------- */
static TokenTypes getExpressionType(SemanticAnalyzer* analyzer, CSTNode* node) {
    CSTNode* child;
    TokenTypes currentType;
    TokenTypes resolvedType = 0;

    if (!node) return 0;

    /* ---- Opaque leaves: LVAL and FUNC_CALL ---- */
    if (node->symbol == NON_TERMINAL_LVAL) {
        CSTNode* identNode = node->firstChild;
        SymbolData data = lookupSymbolOrError(analyzer,
            identNode->token->lexeme,
            identNode->token->line);

        if (data.type == TYPE_FUNC) {
            logError(ERROR_SEMANTIC, identNode->token->line,
                "'%s' is a function and must be called with 'with'. "
                "Did you mean '%s with (...)'?",
                identNode->token->lexeme, identNode->token->lexeme);
            resolvedType = 0;
            node->suLabel = 1; /* Error case */
        }
        else if (identNode->nextSibling &&
            identNode->nextSibling->token &&
            identNode->nextSibling->token->type == TOKEN_ATPOSITION) {
            resolvedType = TOKEN_TEXT;
            /* atPosition clobbers heavily with bounds checking, treat as expensive */
            node->suLabel = RA_POOL_SIZE + 1;
        }
        else {
            resolvedType = data.dataType;
            /* FIX: Plain variable loads only require 1 register! */
            node->suLabel = 1;
        }

        return resolvedType;
    }

    if (node->symbol == NON_TERMINAL_FUNC_CALL) {
        CSTNode* identNode = node->firstChild;
        SymbolData data = lookupSymbolOrError(analyzer,
            identNode->token->lexeme,
            identNode->token->line);
        if (data.type == TYPE_FUNC && data.dataType == 0)
            logError(ERROR_SEMANTIC, identNode->token->line,
                "'%s' does not return a value and cannot be used "
                "in an expression.", identNode->token->lexeme);
        /* Function call — always maximally expensive */
        node->suLabel = RA_POOL_SIZE + 1;
        return data.dataType;
    }

    /* ---- Literal terminal ---- */
    if (node->token) {
        if (isNumberType(node->token->type))    resolvedType = TOKEN_NUMBER;
        else if (isTextType(node->token->type)) resolvedType = TOKEN_TEXT;
        node->suLabel = 1;
        return resolvedType;
    }

    /* ---- Interior expression nodes: recurse into children ---- */
    {
        int need = 0;
        int prevWasOp = 0; /* 1 when the previous sibling was an E_OP or T_OP */

        child = node->firstChild;
        while (child) {
            int childNeed;
            currentType = getExpressionType(analyzer, child);
            childNeed = child->suLabel;

            if (child->symbol == NON_TERMINAL_E_OP ||
                child->symbol == NON_TERMINAL_T_OP) {
                /* Operator node signals a binary split; mark type as NUMBER */
                if (resolvedType != 0 && !isNumberType(resolvedType))
                    logError(ERROR_SEMANTIC, getNodeLine(child),
                        "Arithmetic operator used on non-number type %s.",
                        tokenTypeToName(resolvedType));
                resolvedType = TOKEN_NUMBER;
                prevWasOp = 1;
            }
            else {
                /* Type propagation */
                if (currentType != 0 && resolvedType == 0)
                    resolvedType = currentType;
                else if (currentType != 0 && resolvedType != 0 &&
                    !isNumberType(currentType))
                    logError(ERROR_SEMANTIC, getNodeLine(child),
                        "Arithmetic operator used on non-number type %s.",
                        tokenTypeToName(currentType));

                /*
                 * SU label update.
                 *
                 * For the very first operand in a chain, propagate its label
                 * directly — it has not been combined with anything yet.
                 *
                 * For every subsequent rhs (prevWasOp == 1) the lhs is the
                 * already-computed running accumulator sitting in rax.  That
                 * intermediate result costs exactly 1 register to hold (just
                 * rax itself), NOT the original leaf cost stored in `need`.
                 * Using `need` here was the bug: it inflated the label by
                 * treating the accumulated intermediate as if it were still a
                 * raw LVAL that had never been evaluated.
                 *
                 * Correct binary SU rule with lhsLabel fixed to 1:
                 *   lhs == rhs  →  rhs + 1   (need one extra to hold lhs)
                 *   lhs >  rhs  →  1          (lhs in rax is enough; rhs is cheaper)
                 *   lhs <  rhs  →  rhs        (rhs dominates)
                 */
                if (prevWasOp) {
                    int lhsLabel = 1; /* accumulated result is already in rax */
                    if (lhsLabel == childNeed)     need = childNeed + 1;
                    else if (childNeed > lhsLabel) need = childNeed;
                    else                           need = lhsLabel;
                }
                else {
                    if (childNeed > need) need = childNeed;
                }
                prevWasOp = 0;
            }
            child = child->nextSibling;
        }

        node->suLabel = (need == 0) ? 1 : need;
    }

    return resolvedType;
}


/* ----------------------------------------------------------------
 * Statement handlers
 * ---------------------------------------------------------------- */

 /*
  * Recursively validate all expression nodes inside a subtree,
  * dispatching to the specific handlers for LVAL, FUNC_CALL, and
  * E/T/F nodes.
  */
static void handleExpressions(SemanticAnalyzer* analyzer, CSTNode* node) {
    CSTNode* child;
    if (!node) return;

    if (node->symbol == NON_TERMINAL_LVAL) {
        handleLVal(analyzer, node);
    }
    else if (node->symbol == NON_TERMINAL_FUNC_CALL) {
        handleFuncCallStmt(analyzer, node);
    }
    else if (node->symbol == NON_TERMINAL_E ||
        node->symbol == NON_TERMINAL_T ||
        node->symbol == NON_TERMINAL_F) {
        getExpressionType(analyzer, node);
    }
    else {
        child = node->firstChild;
        while (child) {
            handleExpressions(analyzer, child);
            child = child->nextSibling;
        }
    }
}

static void handleVarDecl(SemanticAnalyzer* analyzer, CSTNode* node) {
    CSTNode* identNode = node->firstChild->nextSibling;
    CSTNode* beNode = identNode->nextSibling;
    CSTNode* typeNode = beNode->nextSibling;

    if (typeNode && typeNode->firstChild && typeNode->firstChild->token)
        addIdentifier(analyzer, identNode->token->lexeme, TYPE_VAR,
            typeNode->firstChild->token->type,
            identNode->token->line);
    else
        logError(ERROR_SEMANTIC, getNodeLine(node),
            "Failed to resolve type for variable '%s'",
            identNode->token->lexeme);
}

/*
 * Register the function name in the current scope, open a new scope
 * for the body, register each parameter as TYPE_PARAM (type unknown
 * until inferred), analyse the body, then restore the outer scope.
 *
 * Parameters are collected via a small iterative stack to avoid deep
 * C-stack recursion on parameter lists.
 */
static void handleFuncDecl(SemanticAnalyzer* analyzer, CSTNode* node) {
    CSTNode* identNode = node->firstChild->nextSibling;
    CSTNode* paramsNode = identNode->nextSibling->nextSibling;
    CSTNode* block, * curr;
    char* funcName = identNode->token->lexeme;
    SymbolData* funcData, * previousFunc;

    addIdentifier(analyzer, funcName, TYPE_FUNC, 0, identNode->token->line);
    funcData = getSymbolPointer(analyzer->currentScope->symbols, funcName);
    previousFunc = analyzer->currentFunctionData;
    analyzer->currentFunctionData = funcData;

    enterScope(analyzer);

    if (paramsNode->symbol == NON_TERMINAL_PARAM_LIST) {
        CSTNode* stk[64]; int top = 0;
        stk[top++] = paramsNode;
        while (top > 0) {
            curr = stk[--top];
            if (!curr) continue;
            if (curr->token && curr->token->type == TOKEN_IDENT) {
                addIdentifier(analyzer, curr->token->lexeme,
                    TYPE_PARAM, 0, curr->token->line);
                if (funcData) funcData->numParams++;
            }
            else if (curr->symbol == NON_TERMINAL_PARAM_LIST) {
                CSTNode* child = curr->firstChild;
                int base = top;
                int lo, hi;
                while (child && top < 64) {
                    stk[top++] = child;
                    child = child->nextSibling;
                }
                /* Reverse so children are processed left-to-right */
                lo = base; hi = top - 1;
                while (lo < hi) {
                    CSTNode* tmp = stk[lo];
                    stk[lo] = stk[hi];
                    stk[hi] = tmp;
                    lo++; hi--;
                }
            }
        }
    }

    block = node->firstChild;
    while (block && block->symbol != NON_TERMINAL_BLOCK)
        block = block->nextSibling;
    if (block) analyzeNode(analyzer, block);

    exitScope(analyzer);
    analyzer->currentFunctionData = previousFunc;
}

static void handleLVal(SemanticAnalyzer* analyzer, CSTNode* node) {
    CSTNode* identNode = node->firstChild;
    SymbolData data = lookupSymbolOrError(analyzer, identNode->token->lexeme,
        identNode->token->line);

    if (data.type == TYPE_FUNC)
        logError(ERROR_SEMANTIC, identNode->token->line,
            "'%s' is a function and cannot be used as a variable.",
            identNode->token->lexeme);

    if (identNode->nextSibling &&
        identNode->nextSibling->token->type == TOKEN_ATPOSITION) {
        if (!isTextType(data.dataType))
            logError(ERROR_SEMANTIC, identNode->token->line,
                "'%s' is not a text variable. atPosition can only be "
                "used on text.", identNode->token->lexeme);
        handleExpressions(analyzer, identNode->nextSibling->nextSibling);
    }
}

static void handleAssignment(SemanticAnalyzer* analyzer, CSTNode* node) {
    CSTNode* lvalNode = node->firstChild->nextSibling;
    CSTNode* identNode = lvalNode->firstChild;
    SymbolData lvalData = lookupSymbolOrError(analyzer,
        identNode->token->lexeme,
        identNode->token->line);
    CSTNode* toBy;
    CSTNode* exprNode;
    TokenTypes exprType;

    if (lvalData.type == TYPE_FUNC)
        logError(ERROR_SEMANTIC, identNode->token->line,
            "'%s' is a function and cannot be used as a variable.",
            identNode->token->lexeme);

    toBy = lvalNode->nextSibling;
    exprNode = toBy ? toBy->nextSibling : NULL;
    if (!exprNode) return;

    /*
     * If the lhs is an untyped parameter, try to infer its type from
     * the rhs before checking for mismatches.
     */
    if (lvalData.type == TYPE_PARAM && lvalData.dataType == 0) {
        CSTNode* rhsLval = exprNode;
        while (rhsLval && rhsLval->symbol != NON_TERMINAL_LVAL)
            rhsLval = rhsLval->firstChild;
        if (rhsLval && rhsLval->firstChild && rhsLval->firstChild->token) {
            SymbolData rhsData = lookupSymbol(analyzer->currentScope,
                rhsLval->firstChild->token->lexeme);
            if (rhsData.dataType != 0) {
                ScopeNode* scope = analyzer->currentScope;
                SymbolData* paramPtr = NULL;
                while (scope && paramPtr == NULL) {
                    paramPtr = getSymbolPointer(scope->symbols,
                        identNode->token->lexeme);
                    if (paramPtr == NULL) scope = scope->parent;
                }
                if (paramPtr) {
                    paramPtr->dataType = rhsData.dataType;
                    lvalData.dataType = rhsData.dataType;
                }
            }
        }
    }

    if (lvalData.dataType != 0)
        inferParamType(analyzer, exprNode, lvalData.dataType);

    exprType = getExpressionType(analyzer, exprNode);

    if (lvalData.dataType != 0 && exprType != 0 &&
        !typesMatch(lvalData.dataType, exprType))
        logError(ERROR_SEMANTIC, identNode->token->line,
            "Type mismatch in assignment. Cannot assign type %s to "
            "variable '%s' of type %s.",
            tokenTypeToName(exprType), identNode->token->lexeme,
            tokenTypeToName(lvalData.dataType));

    /* Increase/Decrease/Multiply/Divide only work on numbers */
    if (node->firstChild->token->type != TOKEN_SET &&
        !isNumberType(lvalData.dataType))
        logError(ERROR_SEMANTIC, identNode->token->line,
            "Arithmetic operations (Increase/Decrease/etc) can only be "
            "performed on numbers. '%s' is not a number.",
            identNode->token->lexeme);
}

static void handleFuncCallStmt(SemanticAnalyzer* analyzer, CSTNode* node) {
    /* The first child differs slightly between FUNC_CALL_STMT and FUNC_CALL */
    CSTNode* identNode = (node->symbol == NON_TERMINAL_FUNC_CALL)
        ? node->firstChild
        : node->firstChild->firstChild;
    CSTNode* argsNode;
    SymbolData data = lookupSymbolOrError(analyzer, identNode->token->lexeme,
        identNode->token->line);

    if (data.type != TYPE_FUNC)
        logError(ERROR_SEMANTIC, identNode->token->line,
            "'%s' is not a function.", identNode->token->lexeme);

    argsNode = identNode->nextSibling;
    while (argsNode && argsNode->symbol != NON_TERMINAL_ARG_LIST)
        argsNode = argsNode->nextSibling;
    if (argsNode)
        handleExpressions(analyzer, argsNode);
}

static void handleFuncRet(SemanticAnalyzer* analyzer, CSTNode* node) {
    CSTNode* exprNode;
    TokenTypes returnType;

    if (!analyzer->currentFunctionData) {
        logError(ERROR_SEMANTIC, getNodeLine(node),
            "Return statement outside of any function.");
    }
    else {
        exprNode = node->firstChild->nextSibling;
        returnType = getExpressionType(analyzer, exprNode);

        if (analyzer->currentFunctionData->dataType == 0 && returnType != 0)
            /* First return encountered — set the function's return type */
            analyzer->currentFunctionData->dataType = returnType;
        else if (returnType != 0 &&
            !typesMatch(analyzer->currentFunctionData->dataType, returnType))
            logError(ERROR_SEMANTIC, getNodeLine(node),
                "Function return type mismatch. Expected %s, got %s.",
                tokenTypeToName(analyzer->currentFunctionData->dataType),
                tokenTypeToName(returnType));
    }
}

static void handleIO(SemanticAnalyzer* analyzer, CSTNode* node) {
    if (node->firstChild->token &&
        node->firstChild->token->type == TOKEN_SHOW) {
        handleExpressions(analyzer, node->firstChild->nextSibling);
    }
    else if (node->firstChild->token &&
        node->firstChild->token->type == TOKEN_GET) {
        CSTNode* typeNode = node->firstChild->nextSibling;
        CSTNode* lval = typeNode;
        while (lval && lval->symbol != NON_TERMINAL_LVAL)
            lval = lval->nextSibling;

        if (lval) {
            CSTNode* identNode;
            SymbolData* paramPtr;

            handleLVal(analyzer, lval);
            identNode = lval->firstChild;
            paramPtr = getSymbolPointer(analyzer->currentScope->symbols,
                identNode->token->lexeme);
            /* If the target is an untyped param, stamp it with the Get's type */
            if (paramPtr && paramPtr->type == TYPE_PARAM &&
                paramPtr->dataType == 0) {
                if (typeNode && typeNode->firstChild &&
                    typeNode->firstChild->token)
                    paramPtr->dataType = typeNode->firstChild->token->type;
            }
        }
    }
}

/* Opens a new scope, walks all children, then closes the scope */
static void handleBlock(SemanticAnalyzer* analyzer, CSTNode* node) {
    CSTNode* bChild;
    enterScope(analyzer);
    bChild = node->firstChild;
    while (bChild) {
        analyzeNode(analyzer, bChild);
        bChild = bChild->nextSibling;
    }
    exitScope(analyzer);
}

/* ----------------------------------------------------------------
 * Condition validation
 * Checks both sides of every relational operator for type legality.
 * ---------------------------------------------------------------- */
static void validateConditionTypes(SemanticAnalyzer* analyzer, CSTNode* node) {
    CSTNode* child;
    if (!node) return;

    if (node->symbol == NON_TERMINAL_BOOL_F) {
        CSTNode* lhs = node->firstChild;
        CSTNode* relOp = lhs ? lhs->nextSibling : NULL;
        CSTNode* rhs = relOp ? relOp->nextSibling : NULL;

        if (relOp && relOp->firstChild && relOp->firstChild->token) {
            TokenTypes op = relOp->firstChild->token->type;
            TokenTypes lhsType = getExpressionType(analyzer, lhs);
            TokenTypes rhsType = getExpressionType(analyzer, rhs);
            unsigned line = getNodeLine(relOp);

            if (op == TOKEN_GT || op == TOKEN_LT ||
                op == TOKEN_ATLEAST || op == TOKEN_ATMOST) {
                if (lhsType != 0 && !isNumberType(lhsType))
                    logError(ERROR_SEMANTIC, line,
                        "Operator '%s' requires a number on the left "
                        "side, got %s.",
                        relOp->firstChild->token->lexeme,
                        tokenTypeToName(lhsType));
                if (rhsType != 0 && !isNumberType(rhsType))
                    logError(ERROR_SEMANTIC, line,
                        "Operator '%s' requires a number on the right "
                        "side, got %s.",
                        relOp->firstChild->token->lexeme,
                        tokenTypeToName(rhsType));
            }
            else if (op == TOKEN_EQTO || op == TOKEN_NOTEQ) {
                if (lhsType != 0 && rhsType != 0 &&
                    !typesMatch(lhsType, rhsType))
                    logError(ERROR_SEMANTIC, line,
                        "Operator '%s' requires both sides to be the "
                        "same type, got %s and %s.",
                        relOp->firstChild->token->lexeme,
                        tokenTypeToName(lhsType),
                        tokenTypeToName(rhsType));
            }
        }
        return;
    }

    child = node->firstChild;
    while (child) {
        validateConditionTypes(analyzer, child);
        child = child->nextSibling;
    }
}

static void handleConditional(SemanticAnalyzer* analyzer, CSTNode* node) {
    inferParamTypesFromCondition(analyzer, node);
    validateConditionTypes(analyzer, node);
    handleExpressions(analyzer, node);
}

static void handleLoops(SemanticAnalyzer* analyzer, CSTNode* node) {
    /* node is the loop keyword token; the count/condition follows as nextSibling */
    handleExpressions(analyzer, node->nextSibling);
}

static void handleCtrlFlow(SemanticAnalyzer* analyzer, CSTNode* node) {
    CSTNode* child = node->firstChild;
    if (!child) return;

    if (child->symbol == NON_TERMINAL_IF_STMT) {
        CSTNode* condNode = child->firstChild->nextSibling;
        CSTNode* blockNode = condNode;
        CSTNode* elifNode;
        int elifDone = 0;

        handleConditional(analyzer, condNode);

        while (blockNode && blockNode->symbol != NON_TERMINAL_BLOCK)
            blockNode = blockNode->nextSibling;
        if (blockNode) analyzeNode(analyzer, blockNode);

        elifNode = blockNode ? blockNode->nextSibling : NULL;

        while (elifNode && elifNode->symbol == NON_TERMINAL_ELIF_LIST &&
            !elifDone) {
            CSTNode* ec = elifNode->firstChild;
            if (!ec) {
                elifDone = 1;
            }
            else if (ec->token && ec->token->type == TOKEN_OTHERWISE) {
                CSTNode* next = ec->nextSibling;
                if (next && next->token &&
                    next->token->type == TOKEN_OTHERWISE_IF) {
                    /* "Otherwise if ..." — another conditional branch */
                    CSTNode* ic = next->nextSibling;
                    CSTNode* ib = ic;
                    handleConditional(analyzer, ic);
                    while (ib && ib->symbol != NON_TERMINAL_BLOCK)
                        ib = ib->nextSibling;
                    if (ib) analyzeNode(analyzer, ib);
                    elifNode = ib ? ib->nextSibling : NULL;
                }
                else {
                    /* "Otherwise:" — plain else block */
                    CSTNode* eb = next;
                    while (eb && eb->symbol != NON_TERMINAL_BLOCK)
                        eb = eb->nextSibling;
                    if (eb) analyzeNode(analyzer, eb);
                    elifDone = 1;
                }
            }
            else if (ec->symbol == NON_TERMINAL_ELSE_PART) {
                CSTNode* eb = ec->firstChild;
                while (eb && eb->symbol != NON_TERMINAL_BLOCK)
                    eb = eb->nextSibling;
                if (eb) analyzeNode(analyzer, eb);
                elifDone = 1;
            }
            else {
                elifDone = 1;
            }
        }
    }
    else if (child->symbol == NON_TERMINAL_LOOP_STMT) {
        CSTNode* block = child->firstChild;
        handleLoops(analyzer, child->firstChild);
        while (block && block->symbol != NON_TERMINAL_BLOCK)
            block = block->nextSibling;
        if (block) analyzeNode(analyzer, block);
    }
}

/* ----------------------------------------------------------------
 * Public entry points
 * ---------------------------------------------------------------- */

void analyzeNode(SemanticAnalyzer* analyzer, CSTNode* node) {
    SemanticHandler handler;
    CSTNode* child;

    if (!node) return;
    buildSemanticDispatch(analyzer);
    handler = (SemanticHandler)dispatchGet(analyzer->nodeDispatch, node->symbol);
    if (handler) {
        handler(analyzer, node);
    }
    else {
        /* No specific handler — just recurse into children */
        child = node->firstChild;
        while (child) {
            analyzeNode(analyzer, child);
            child = child->nextSibling;
        }
    }
}

void analyzeCST(SemanticAnalyzer* analyzer, CSTNode* root) {
    if (!root) return;
    analyzeNode(analyzer, root);
}