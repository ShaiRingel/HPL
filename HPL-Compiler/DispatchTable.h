#pragma once

/*
 * DispatchTable — an open-addressing hash map from integer keys to
 * function pointers, used to route CST nodes to their handlers.
 *
 * Both the semantic analyser and the code generator register one handler
 * per NonTerminal value; analyzeNode / generateNode then look up and call
 * the right function without a long switch statement.
 *
 * Size the table at roughly 2× the number of entries you plan to register
 * to keep the load factor low.
 */
typedef void (*DispatchFn)();

typedef struct {
    int key;
    DispatchFn value;
} DispatchEntry;

typedef struct {
    DispatchEntry* entries;
    int capacity;
    int count;
} DispatchTable;

DispatchTable* initDispatchTable(int capacity);
void dispatchSet(DispatchTable* table, int key, DispatchFn fn);
DispatchFn dispatchGet(const DispatchTable* table, int key);
void freeDispatchTable(DispatchTable* table); /* does NOT free the function pointers */
