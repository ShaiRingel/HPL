#pragma once

/* Whether a CST node is a terminal (leaf) or a non-terminal (inner node) */
typedef enum {
    TERMINAL,
    NON_TERMINAL
} SymbolType;

/*
 * NonTerminal — the full set of grammar non-terminals used by the parser.
 *
 * Values start at 400 to sit above all TokenTypes values, so both sets
 * can be stored in the same integer field without collision.
 */
typedef enum {
    NON_TERMINAL_PROG = 400, /* top-level program                          */
    NON_TERMINAL_BLOCK,      /* indented statement block                   */
    NON_TERMINAL_STMT_LIST,  /* list of statements inside a block          */
    NON_TERMINAL_STMT,       /* a single statement                         */
    NON_TERMINAL_VAR_DECL,   /* "Let x be number."                         */
    NON_TERMINAL_TYPE,       /* "number" or "text" type keyword            */
    NON_TERMINAL_ASSIGN_STMT,/* Set / Increase / Decrease / Multiply / Divide */
    NON_TERMINAL_LVAL,       /* assignable location: x  or  x atPosition i */
    NON_TERMINAL_IO_STMT,    /* Show / Get                                 */
    NON_TERMINAL_CTRL_FLOW,  /* if-statement or loop                       */
    NON_TERMINAL_IF_STMT,    /* "If ... then: ..."                         */
    NON_TERMINAL_ELIF_LIST,  /* chain of Otherwise / Otherwise if clauses  */
    NON_TERMINAL_ELSE_PART,  /* final "Otherwise:" block                   */
    NON_TERMINAL_LOOP_STMT,  /* Repeat or While loop                       */
    NON_TERMINAL_FUNC_DECL,  /* "To funcName with params:"                 */
    NON_TERMINAL_PARAM_LIST, /* comma-separated parameter names            */
    NON_TERMINAL_FUNC_RET,   /* "Return expr."                             */
    NON_TERMINAL_FUNC_CALL_STMT, /* function call used as a statement      */
    NON_TERMINAL_FUNC_CALL,  /* function call used as an expression        */
    NON_TERMINAL_ARG_LIST,   /* comma-separated argument expressions       */
    NON_TERMINAL_E,          /* expression (additive level)                */
    NON_TERMINAL_T,          /* term (multiplicative / remainder level)    */
    NON_TERMINAL_F,          /* factor (atom)                              */
    NON_TERMINAL_COND,       /* full condition (OR of BOOL_T terms)        */
    NON_TERMINAL_BOOL_T,     /* boolean term (AND of BOOL_F factors)       */
    NON_TERMINAL_BOOL_F,     /* boolean factor: comparison or "not" prefix */
    NON_TERMINAL_E_OP,       /* "plus" or "minus" operator node            */
    NON_TERMINAL_T_OP,       /* "times" or "divide" operator node         */
    NON_TERMINAL_REL_OP      /* relational operator node                   */
} NonTerminal;

/*
 * Parallel string table for NonTerminal — indexed by
 * (symbol - NON_TERMINAL_PROG), used when printing the CST.
 */
static const char* nonTerminalNames[] = {
    "PROG",
    "BLOCK",
    "STMT_LIST",
    "STMT",
    "VAR_DECL",
    "TYPE",
    "ASSIGN_STMT",
    "LVAL",
    "IO_STMT",
    "CTRL_FLOW",
    "IF_STMT",
    "ELIF_LIST",
    "ELSE_PART",
    "LOOP_STMT",
    "FUNC_DECL",
    "PARAM_LIST",
    "FUNC_RET",
    "FUNC_CALL_STMT",
    "FUNC_CALL",
    "ARG_LIST",
    "E",
    "T",
    "F",
    "COND",
    "BOOL_T",
    "BOOL_F",
    "E_OP",
    "T_OP",
    "REL_OP"
};
