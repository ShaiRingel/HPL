#pragma once
#include "Compiler.h"
#include "Lexer.h"

/*
 * State–Message Map (SMM) — maps parser state numbers to human-readable
 * "expected …" strings shown when a syntax error occurs in that state.
 *
 * Stored as a fixed-capacity hash map with chaining.  The table is built
 * lazily on the first call to smmLookup().
 */
#define SMM_CAPACITY 64

typedef struct SMMEntry {
    unsigned short state;
    const char* message;
    struct SMMEntry* next;
} SMMEntry;

extern SMMEntry* s_smm_buckets[SMM_CAPACITY];

const char* smmLookup(unsigned short state);

/*
 * Tokens that act as synchronisation points during panic-mode recovery.
 * The lexer skips tokens until it finds one of these, which are reliable
 * statement or block boundaries.
 */
#define synchronizingTokens(s) \
    ((s) != TOKEN_EOS && (s) != TOKEN_DEDENT && \
     (s) != TOKEN_COLON && (s) != TOKEN_EOF)

typedef enum {
    ERROR_LEXICAL,   /* bad character or malformed token              */
    ERROR_SYNTAX,    /* token not expected by the parser              */
    ERROR_SEMANTIC,  /* type mismatch, undeclared identifier, etc.    */
    ERROR_INTERNAL,  /* bug in the compiler itself                    */
    ERROR_GLOBAL     /* command-line or file-open error               */
} ErrorType;

/* Total number of non-fatal errors accumulated so far */
extern unsigned errorCount;

/* The source line currently being processed, updated by the lexer */
extern unsigned g_currentLine;

/* Register the active Compiler so reportError() can free it on exit */
void setCurrentCompiler(Compiler* compiler);

/* Fatal: print the error, free the compiler, and exit */
void reportError(ErrorType type, unsigned line, const char* format, ...);

/* Non-fatal: print the error and increment errorCount */
void logError(ErrorType type, unsigned line, const char* format, ...);

/*
 * Panic-mode recovery — called after a syntax error.
 * Discards tokens until a synchronising token is found, then pops the
 * parse stack until the automaton can legally continue.
 */
void panicModeRecovery(Token** currentToken, int* next);

/* Returns the "expected …" hint for a given parser state, or NULL */
const char* getExpectedTokenMessage(unsigned short state);
