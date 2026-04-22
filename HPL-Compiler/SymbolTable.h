#pragma once
#include "Global.h"

#define CAPACITY 100 /* number of hash buckets in every symbol table */

/* Classifies what kind of symbol an entry represents */
typedef enum {
    TYPE_NONE  = 0, /* uninitialised / not found                    */
    TYPE_VAR,       /* a declared variable                          */
    TYPE_FUNC,      /* a declared function                          */
    TYPE_PARAM      /* a function parameter (type may be inferred)  */
} VarType;

/*
 * SymbolData — the payload stored for each symbol.
 *
 * Fields are used selectively depending on the VarType:
 *   TYPE_VAR   : type, dataType, isArray
 *   TYPE_FUNC  : type, dataType (return type), numParams, paramTypes
 *   TYPE_PARAM : type, dataType (may be 0 until inferred)
 */
typedef struct {
    VarType type;
    TokenTypes dataType;       /* TOKEN_INTEGER / TOKEN_TEXT / 0 if unknown */
    int isArray;               /* non-zero if this is a text (string) variable */
    int numParams;             /* for TYPE_FUNC: number of parameters          */
    TokenTypes* paramTypes;    /* for TYPE_FUNC: per-parameter type array      */
    int value;                 /* 1 once the symbol has been fully declared    */
} SymbolData;

/* One bucket entry in the hash table */
typedef struct SymbolEntry {
    char* key;
    SymbolData data;
    struct SymbolEntry* next;
} SymbolEntry;

typedef struct {
    SymbolEntry** buckets;
    int capacity;
} SymbolTable;

SymbolTable* initSymbolTable();
void putSymbol(SymbolTable* table, char* key, SymbolData data);
void removeSymbol(SymbolTable* table, const char* key);
SymbolData getSymbol(SymbolTable* table, char* key);
SymbolData* getSymbolPointer(SymbolTable* table, char* key); /* direct pointer into storage — do not hold across putSymbol calls */
void freeSymbolTable(SymbolTable* table);
