#define _CRT_SECURE_NO_WARNINGS
#include "Lexer.h"
#include "LexerFSM.h"
#include "ErrorHandler.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Read one character and keep the line counter in sync */
static int lexerGetc(Lexer* lexer) {
    int c = fgetc(lexer->fp);
    if (c == '\n') {
        lexer->lineNumber++;
        g_currentLine = lexer->lineNumber;
    }
    return c;
}

/* Push a character back and undo the line-count increment if needed */
static void lexerUngetc(int c, Lexer* lexer) {
    if (c == '\n') {
        lexer->lineNumber--;
        g_currentLine = lexer->lineNumber;
    }
    ungetc(c, lexer->fp);
}

/* Double the lexeme buffer when the current token is too long to fit */
static void lexemeSizeExpander(Token* token, int* capacity) {
    char* temp;

    if (*capacity << 1 > MAX_TOKEN_SIZE)
        reportError(ERROR_LEXICAL, g_currentLine,
                    "Maximum identifier name size reached.");

    (*capacity) <<= 1;

    temp = (char*)realloc(token->lexeme, *capacity * sizeof(char));
    if (!temp)
        reportError(ERROR_INTERNAL, 0,
                    "Failed to allocate memory for tokens lexeme.");

    token->lexeme = temp;
}

Token* createToken(const char* lexeme, TokenTypes type, unsigned line) {
    Token* token = (Token*)malloc(sizeof(Token));
    if (!token)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate new token");

    assert(token);
    token->type   = type;
    token->lexeme = _strdup(lexeme);
    token->line   = line;
    return token;
}

/*
 * Handle indentation at the start of a new line.
 *
 * Counts leading spaces/tabs, then compares against the top of the
 * indent stack:
 *   deeper   -> push and emit INDENT
 *   shallower -> pop one level per DEDENT needed; queue extras in pendingDedents
 *   same     -> no token emitted (just clears isStartOfLine)
 *
 * Returns the indent token to emit (INDENT or first DEDENT), or NULL
 * if indentation is unchanged.
 */
static Token* handleIndentation(Lexer* lexer, int* c) {
    int currentIndent = 0, prevIndent;
    Token* result = NULL;

    while (*c == ' ' || *c == '\t') {
        currentIndent += (*c == '\t') ? 4 : 1;
        *c = lexerGetc(lexer);
    }

    if (*c != '\n' && *c != '\r' && *c != EOF) {
        prevIndent = lexer->indentStack[lexer->indentTop];

        if (currentIndent > prevIndent) {
            if (lexer->indentTop + 1 >= MAX_INDENT_DEPTH)
                reportError(ERROR_LEXICAL, lexer->lineNumber,
                            "Max indentation depth reached!");

            lexer->indentStack[++lexer->indentTop] = currentIndent;
            lexer->isStartOfLine = 0;
            lexerUngetc(*c, lexer);
            result = createToken("INDENT", TOKEN_INDENT, lexer->lineNumber);

        } else if (currentIndent < prevIndent) {
            int dedents = 0;
            while (lexer->indentTop > 0 &&
                   lexer->indentStack[lexer->indentTop] > currentIndent) {
                lexer->indentTop--;
                dedents++;
            }

            if (lexer->indentStack[lexer->indentTop] != currentIndent)
                logError(ERROR_LEXICAL, lexer->lineNumber,
                         "Inconsistent indentation! Expected %d spaces "
                         "but found %d.",
                         lexer->indentStack[lexer->indentTop], currentIndent);

            lexer->isStartOfLine  = 0;
            lexerUngetc(*c, lexer);
            lexer->pendingDedents = dedents - 1; /* first DEDENT returned below */
            result = createToken("DEDENT", TOKEN_DEDENT, lexer->lineNumber);

        } else {
            lexer->isStartOfLine = 0;
        }
    }

    return result;
}

/*
 * Detect "leading zero" number formatting errors like "07" or "-07".
 * Resets the counter so those invalid digits are not accumulated.
 */
static void handleNumberFormatting(Token* token, int* counter, char c) {
    int isDigit = (c >= '0' && c <= '9');
    *counter =
        (*counter == 1 && token->lexeme[0] == '0' && isDigit) ? 0 :
        (*counter == 2 && token->lexeme[0] == '-' &&
         token->lexeme[1] == '0' && isDigit) ? 1 :
        *counter;
}

/*
 * Feed characters into the FSM one at a time until it reaches STATE_ACCEPT.
 * Grows the lexeme buffer on demand.  The final character that triggered
 * acceptance is ungotten so the next call starts from it.
 */
static Token* runFSM(Lexer* lexer, int c) {
    Token* token = (Token*)malloc(sizeof(Token));
    int capacity = LONGEST_WORD_LENGTH + 1;
    LexerFSM* fsm = lexer->lexerFSM;
    int counter = 0;

    if (!token)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate new token");

    token->lexeme = (char*)malloc(capacity * sizeof(char));
    if (!token->lexeme) {
        free(token);
        reportError(ERROR_INTERNAL, 0, "Failed to allocate for tokens lexeme");
    }

    while (fsm->currentState != STATE_ACCEPT) {
        g_currentLine = lexer->lineNumber;

        if (getTokenType(fsm->transitionTable, fsm->currentState) == TOKEN_NUMBER)
            handleNumberFormatting(token, &counter, c);

        token->lexeme[counter++] = c;
        token->type = advance(fsm, c);

        /* advance() returning non-IDLE means the token is finished;
           the character that triggered it is NOT part of the lexeme */
        if (token->type != TOKEN_IDLE || fsm->currentState == STATE_COMMENT)
            if (counter > 0) counter--;

        /* FSM returned to start — discard whatever was accumulated */
        if (token->type == TOKEN_IDLE && !fsm->currentState)
            counter = 0;

        if (counter >= capacity)
            lexemeSizeExpander(token, &capacity);

        if (token->type == TOKEN_IDLE)
            c = lexerGetc(lexer);
    }

    lexerUngetc(c, lexer); /* unget the terminating character */

    if (token->type == TOKEN_NEWLINE)
        lexer->isStartOfLine = 1;

    token->lexeme[counter] = '\0';
    token->lexeme = (char*)realloc(token->lexeme, (counter + 1) * sizeof(char));
    token->line   = lexer->lineNumber;

    return token;
}

Lexer* initLexer(const char* path) {
    Lexer* lexer = (Lexer*)malloc(sizeof(Lexer));
    if (!lexer)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for Lexer.");

    assert(lexer);
    lexer->lexerFSM = initLexerFSM();
    lexer->fp       = fopen(path, "r");

    if (!lexer->fp)
        reportError(ERROR_GLOBAL, 0, "Failed to open file: %s", path);

    lexer->indentStack[0] = 0;
    lexer->indentTop      = 0;
    lexer->pendingDedents = 0;
    lexer->isStartOfLine  = 1;
    lexer->lineNumber     = 1;
    g_currentLine         = 1;

    return lexer;
}

/*
 * Return the next token from the source file.
 *
 * Priority order:
 *   1. Queued DEDENT tokens from a multi-level dedent
 *   2. Indentation token (INDENT or first DEDENT) at start of a line
 *   3. Normal FSM scan
 *   4. Synthetic DEDENT tokens to close any still-open blocks at EOF
 */
Token* nextToken(Lexer* lexer) {
    LexerFSM* fsm = lexer->lexerFSM;
    Token* result = NULL;
    int c;

    if (lexer->pendingDedents > 0) {
        lexer->pendingDedents--;
        result = createToken("DEDENT", TOKEN_DEDENT, lexer->lineNumber);
    } else {
        c = lexerGetc(lexer);

        while (lexer->isStartOfLine && result == NULL) {
            Token* indentToken = handleIndentation(lexer, &c);
            if (indentToken) {
                result = indentToken;
            } else if (c == EOF) {
                lexer->isStartOfLine = 0;
            } else if (lexer->isStartOfLine) {
                c = lexerGetc(lexer);
            }
        }

        if (!result) {
            if (c == EOF) {
                /* Close any remaining open blocks before signalling EOF */
                result = (lexer->indentTop-- > 0)
                    ? createToken("DEDENT", TOKEN_DEDENT, lexer->lineNumber)
                    : createToken("", TOKEN_EOF, lexer->lineNumber);
            } else {
                fsm->currentState = STATE_START;
                result = runFSM(lexer, c);

                if (result->type == TOKEN_EOF && lexer->indentTop-- > 0) {
                    free(result->lexeme);
                    free(result);
                    result = createToken("DEDENT", TOKEN_DEDENT, lexer->lineNumber);
                }
            }
        }
    }

    return result;
}

void freeLexer(Lexer* lexer) {
    freeLexerFSM(lexer->lexerFSM);
    fclose(lexer->fp);
    free(lexer);
}
