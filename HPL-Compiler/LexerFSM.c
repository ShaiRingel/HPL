#include "LexerFSM.h"
#include "ErrorHandler.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <ctype.h>

/*
 * A character is a delimiter when it can never be part of the current
 * lexeme and always terminates it.  The FSM ungets delimiter characters
 * so the next call to nextToken() can start fresh from them.
 */
#define isDelimiter(c) ( isspace(c) || (c) == EOF || (c) == '.' || (c) == ':' || \
                         (c) == ',' || (c) == '(' || (c) == ')' || (c) == '+' || \
                         (c) == '-' || (c) == '"' )

/* Characters that are legal inside an identifier */
#define ALLOWED_SPECIAL " \"()+,-.:_"

typedef struct {
    const char* keyword;
    TokenTypes type;
} KeywordEntry;

/*
 * All reserved keywords with their corresponding token types.
 * addKeyword() walks each string character by character, inserting
 * transitions into the DFA so every keyword is recognised exactly.
 */
KeywordEntry keywordTable[] = {
    { "Let",         TOKEN_LET          },
    { "be",          TOKEN_BE           },
    { "number",      TOKEN_INTEGER      },
    { "text",        TOKEN_TEXT         },
    { "Set",         TOKEN_SET          },
    { "to",          TOKEN_TO           },
    { "Increase",    TOKEN_INCREASE     },
    { "Decrease",    TOKEN_DECREASE     },
    { "Multiply",    TOKEN_MULTIPLY     },
    { "Divide",      TOKEN_DIVIDE       },
    { "by",          TOKEN_BY           },
    { "Show",        TOKEN_SHOW         },
    { "If",          TOKEN_IF           },
    { "then",        TOKEN_THEN         },
    { "if",          TOKEN_OTHERWISE_IF },
    { "Otherwise",   TOKEN_OTHERWISE    },
    { "Repeat",      TOKEN_REPEAT       },
    { "iterations",  TOKEN_ITERATIONS   },
    { "While",       TOKEN_WHILE        },
    { "Get",         TOKEN_GET          },
    { "ask",         TOKEN_ASK          },
    { "storeInto",   TOKEN_STOREINTO    },
    { "To",          TOKEN_TOFUNC       },
    { "with",        TOKEN_WITH         },
    { "Return",      TOKEN_RETURN       },
    { "nan",         TOKEN_NAN          },
    { "equalsTo",    TOKEN_EQTO         },
    { "notEqualsTo", TOKEN_NOTEQ        },
    { "greaterThan", TOKEN_GT           },
    { "lessThan",    TOKEN_LT           },
    { "atLeast",     TOKEN_ATLEAST      },
    { "atMost",      TOKEN_ATMOST       },
    { "remainderOf", TOKEN_REMAINDEROF  },
    { "dividedBy",   TOKEN_DIVIDEDBY    },
    { "plus",        TOKEN_ADD          },
    { "minus",       TOKEN_SUB          },
    { "times",       TOKEN_MUL          },
    { "divide",      TOKEN_DIV          },
    { "and",         TOKEN_AND          },
    { "or",          TOKEN_OR           },
    { "not",         TOKEN_NOT          },
    { "atPosition",  TOKEN_ATPOSITION   },
};

static int isValidCharacter(char c) {
    return isalnum((unsigned char)c) || strchr(ALLOWED_SPECIAL, c);
}

/*
 * Insert transitions for every character of `word` starting from
 * STATE_START.  Intermediate states that don't already exist are created
 * and initially tagged TOKEN_IDENT (they are still valid identifier
 * prefixes); the final state is overwritten with the keyword's token type.
 */
static void addKeyword(TransitionTable* table, const char* word,
                       TokenTypes token) {
    unsigned short current, nextId;
    int i;

    current = STATE_START;
    for (i = 0; word[i] != '\0'; i++) {
        nextId = getState(table, current, word[i]);
        if (nextId == STATE_ERROR) {
            nextId = ++table->stateCounter;
            insertTransition(table, current, word[i], nextId);
            setToken(table, nextId, TOKEN_IDENT);
        }
        current = nextId;
    }
    setToken(table, current, token);
}

/*
 * Build the full DFA by wiring up all transitions.
 *
 * Order matters: keyword transitions are added before the fallback
 * letter->STATE_IDENT transitions, so keyword prefixes get their own
 * states rather than collapsing into the generic identifier chain.
 *
 * The "NOTE:" comment syntax uses a dedicated chain (N-O-T-E-:) that
 * leads to STATE_COMMENT, where everything is discarded until a newline.
 */
static void buildTransitionTable(TransitionTable* table) {
    int i;

    /* Whitespace in the middle of a line is silently consumed */
    insertTransition(table, STATE_START, ' ',  STATE_START);
    insertTransition(table, STATE_START, '\r', STATE_START);
    insertTransition(table, STATE_START, '\t', STATE_START);
    insertTransition(table, STATE_START, '\v', STATE_START);
    insertTransition(table, STATE_START, '\f', STATE_START);
    setToken(table, STATE_START, TOKEN_IDLE);

    /* "NOTE:" comment — chain N -> O -> T -> E -> ':' -> STATE_COMMENT */
    insertTransition(table, STATE_START, 'N', table->stateCounter + 1);
    ++table->stateCounter;
    insertTransition(table, table->stateCounter, 'O', table->stateCounter + 1);
    ++table->stateCounter;
    insertTransition(table, table->stateCounter, 'T', table->stateCounter + 1);
    ++table->stateCounter;
    insertTransition(table, table->stateCounter, 'E', table->stateCounter + 1);
    ++table->stateCounter;
    insertTransition(table, table->stateCounter, ':', STATE_COMMENT);
    insertTransition(table, STATE_COMMENT, '\n', STATE_START);
    insertTransition(table, STATE_COMMENT, EOF,  STATE_START);

    /* All reserved keywords */
    for (i = 0; i < (int)(sizeof(keywordTable)/sizeof(keywordTable[0])); i++)
        addKeyword(table, keywordTable[i].keyword, keywordTable[i].type);

    /* Negative number literals: '-' followed by digits */
    insertTransition(table, STATE_START, '-', ++table->stateCounter);
    setToken(table, table->stateCounter, TOKEN_IDLE);
    ++table->stateCounter;
    for (i = '0'; i <= '9'; i++) {
        insertTransition(table, STATE_START, i, table->stateCounter);
        insertTransition(table, table->stateCounter - 1, i, table->stateCounter);
        insertTransition(table, table->stateCounter, i, table->stateCounter);
    }
    setToken(table, table->stateCounter, TOKEN_NUMBER);

    insertTransition(table, STATE_START, ',', ++table->stateCounter);
    setToken(table, table->stateCounter, TOKEN_COMMA);

    insertTransition(table, STATE_START, '\n', ++table->stateCounter);
    setToken(table, table->stateCounter, TOKEN_NEWLINE);

    insertTransition(table, STATE_START, '(', ++table->stateCounter);
    setToken(table, table->stateCounter, TOKEN_LPAREN);

    insertTransition(table, STATE_START, ')', ++table->stateCounter);
    setToken(table, table->stateCounter, TOKEN_RPAREN);

    /* String literals: everything between two double-quote characters */
    insertTransition(table, STATE_START, '"', STATE_TEXT);
    insertTransition(table, STATE_TEXT,  '"', ++table->stateCounter);
    setToken(table, table->stateCounter, TOKEN_STRING);

    insertTransition(table, STATE_START, ':', ++table->stateCounter);
    setToken(table, table->stateCounter, TOKEN_COLON);

    insertTransition(table, STATE_START, '.', ++table->stateCounter);
    setToken(table, table->stateCounter, TOKEN_EOS);

    insertTransition(table, STATE_START, EOF, ++table->stateCounter);
    setToken(table, table->stateCounter, TOKEN_EOF);

    /*
     * All letters that have not already been claimed by a keyword prefix
     * fall through to STATE_IDENT (generic identifier scanning).
     */
    for (i = 'A'; i <= 'Z'; i++) {
        if (getState(table, STATE_START, i) == STATE_ERROR)
            insertTransition(table, STATE_START, i, STATE_IDENT);
        if (getState(table, STATE_START, i | 0x20) == STATE_ERROR)
            insertTransition(table, STATE_START, i | 0x20, STATE_IDENT);
    }

    setToken(table, STATE_IDENT, TOKEN_IDENT);
}

LexerFSM* initLexerFSM() {
    LexerFSM* fsm = (LexerFSM*)malloc(sizeof(LexerFSM));

    if (!fsm)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for LexerFSM.");

    assert(fsm);
    fsm->currentState    = STATE_START;
    fsm->transitionTable = initTransitionTable();
    buildTransitionTable(fsm->transitionTable);

    return fsm;
}

/*
 * Advance the FSM by one character.
 *
 * Returns TOKEN_IDLE while the lexeme is still being built.
 * Returns the final TokenTypes value when a complete token is ready,
 * at which point the caller should unget the current character (it
 * belongs to the next token).
 *
 * When a delimiter or unknown character is encountered on a non-error
 * transition, the FSM moves to STATE_ACCEPT and emits the current token.
 * For identifiers that happen to share a prefix with a keyword, the FSM
 * continues into STATE_IDENT so the full word can be scanned.
 */
TokenTypes advance(LexerFSM* lexerFSM, char input) {
    unsigned short nextState;
    TokenTypes type;
    TokenTypes result;

    nextState = getState(lexerFSM->transitionTable, lexerFSM->currentState, input);

    if (nextState != STATE_ERROR) {
        lexerFSM->currentState = nextState;
        result = TOKEN_IDLE;
    } else if (lexerFSM->currentState == STATE_COMMENT ||
               lexerFSM->currentState == STATE_TEXT) {
        /* Inside a comment or string — consume everything until the terminator */
        result = TOKEN_IDLE;
    } else {
        type = getTokenType(lexerFSM->transitionTable, lexerFSM->currentState);

        if (!isDelimiter(input)) {
            if (!isValidCharacter(input))
                logError(ERROR_LEXICAL, g_currentLine,
                         "Invalid character encountered: '%c' (ASCII: %d).",
                         input, (int)input);

            if (lexerFSM->currentState == STATE_START && input == '_')
                logError(ERROR_LEXICAL, g_currentLine,
                         "Identifiers cannot start with an underscore.");

            if (type == TOKEN_NUMBER && !isdigit(input))
                reportError(ERROR_LEXICAL, g_currentLine,
                            "Identifiers cannot start with a number.");

            /* Punctuation and numbers always terminate cleanly */
            if (type == TOKEN_NUMBER || type > 300 || type == TOKEN_EOS) {
                lexerFSM->currentState = STATE_ACCEPT;
                result = type;
            } else {
                /* Keyword prefix followed by a non-delimiter — it's an identifier */
                lexerFSM->currentState = STATE_IDENT;
                result = TOKEN_IDLE;
            }
        } else {
            lexerFSM->currentState = STATE_ACCEPT;
            result = type;
        }
    }

    return result;
}

void freeLexerFSM(LexerFSM* lexerFSM) {
    freeTransitionTable(lexerFSM->transitionTable);
    free(lexerFSM);
}
