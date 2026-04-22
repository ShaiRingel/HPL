#include "SymbolTable.h"
#include "ErrorHandler.h"
#include "HashingFunctions.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

SymbolTable* initSymbolTable() {
    SymbolTable* table = (SymbolTable*)malloc(sizeof(SymbolTable));
    if (!table)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate Symbol Table");

    assert(table);
    table->capacity = CAPACITY;
    table->buckets  = (SymbolEntry**)calloc(CAPACITY, sizeof(SymbolEntry*));

    if (!table->buckets) {
        free(table);
        reportError(ERROR_INTERNAL, 0, "Failed to allocate Buckets for symbol table");
    }

    return table;
}

/* Prepends a new entry to the bucket chain (most-recent declaration wins) */
void putSymbol(SymbolTable* table, char* key, SymbolData data) {
    int idx;
    SymbolEntry* newNode;

    if (!key)
        return;

    idx = hashString(key, CAPACITY);
    newNode = (SymbolEntry*)malloc(sizeof(SymbolEntry));
    if (!newNode)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for Symbol bucket");

    assert(newNode);
    newNode->key  = _strdup(key);
    newNode->data = data;

    newNode->next       = table->buckets[idx];
    table->buckets[idx] = newNode;
}

void removeSymbol(SymbolTable* table, const char* key) {
    unsigned idx;
    SymbolEntry** curr;
    SymbolEntry* temp;
    int found = 0;

    if (!table || !key) return;

    idx = hashChar(key, CAPACITY);
    curr = &table->buckets[idx];

    while (*curr && !found) {
        if (strcmp((*curr)->key, key) == 0) {
            temp  = *curr;
            *curr = (*curr)->next;
            free(temp->key);
            free(temp);
            found = 1;
        } else {
            curr = &(*curr)->next;
        }
    }
}

/* Returns a copy of the data; returns a zeroed SymbolData if not found */
SymbolData getSymbol(SymbolTable* table, char* key) {
    SymbolData result = { 0 };

    if (table && key) {
        unsigned idx = hashString(key, CAPACITY);
        SymbolEntry* node = table->buckets[idx];
        int found = 0;

        while (node && !found) {
            if (strcmp(node->key, key) == 0) {
                result = node->data;
                found  = 1;
            } else {
                node = node->next;
            }
        }
    }

    return result;
}

/*
 * Returns a direct pointer into the table's storage so the caller can
 * mutate the data in place (e.g. to update an inferred parameter type).
 * The pointer is invalidated if putSymbol() rehashes the table.
 */
SymbolData* getSymbolPointer(SymbolTable* table, char* key) {
    SymbolData* result = NULL;

    if (table) {
        unsigned int bucket = hashString(key, CAPACITY);
        SymbolEntry* entry  = table->buckets[bucket];
        int found = 0;

        while (entry && !found) {
            if (strcmp(entry->key, key) == 0) {
                result = &(entry->data);
                found  = 1;
            } else {
                entry = entry->next;
            }
        }
    }

    return result;
}

void freeSymbolTable(SymbolTable* table) {
    SymbolEntry* node, *next;
    int i;

    if (!table) return;

    for (i = 0; i < table->capacity; i++) {
        node = table->buckets[i];
        while (node) {
            next = node->next;
            free(node->key);
            free(node);
            node = next;
        }
    }

    free(table->buckets);
    free(table);
}
