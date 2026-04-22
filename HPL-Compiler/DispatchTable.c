#include "HashingFunctions.h"
#include "DispatchTable.h"
#include "ErrorHandler.h"
#include <stdlib.h>
#include <assert.h>

/*
 * Open-addressing hash table with linear probing.
 *
 * An entry whose value is NULL is treated as empty, so NULL is not a
 * valid handler — this is fine because all real handlers are non-NULL
 * function pointers.
 *
 * The table never grows, so pick a capacity of ~2× the number of entries
 * you intend to register to keep clustering low.
 */

DispatchTable* initDispatchTable(int capacity) {
    int i;
    DispatchTable* table = (DispatchTable*)malloc(sizeof(DispatchTable));

    if (!table)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate DispatchTable");

    assert(table);

    if (capacity < 4) capacity = 4;
    table->capacity = capacity;
    table->count = 0;
    table->entries = (DispatchEntry*)malloc(capacity * sizeof(DispatchEntry));

    if (!table->entries)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate DispatchTable entries");

    for (i = 0; i < capacity; i++) {
        table->entries[i].key   = 0;
        table->entries[i].value = NULL;
    }

    return table;
}

void dispatchSet(DispatchTable* table, int key, DispatchFn fn) {
    unsigned idx = hashNumber(key, table->capacity);
    int i;

    /* Linear probe to find an empty slot or an existing entry for this key */
    for (i = 0; i < table->capacity; i++) {
        unsigned pos = (idx + i) % (unsigned)table->capacity;
        DispatchEntry* e = &table->entries[pos];

        if (e->value == NULL || e->key == key) {
            if (e->value == NULL) table->count++;
            e->key = key;
            e->value = fn;
            return;
        }
    }

    reportError(ERROR_INTERNAL, 0,
                "DispatchTable full (capacity %d, count %d)",
                table->capacity, table->count);
}

DispatchFn dispatchGet(const DispatchTable* table, int key) {
    unsigned idx = hashNumber(key, table->capacity);
    int i;

    for (i = 0; i < table->capacity; i++) {
        unsigned pos = (idx + i) % (unsigned)table->capacity;
        const DispatchEntry* e = &table->entries[pos];

        if (e->value == NULL)
            return NULL; /* hit an empty slot — key is not present */
        if (e->key == key)
            return e->value;
    }

    return NULL;
}

void freeDispatchTable(DispatchTable* table) {
    if (!table) return;
    free(table->entries);
    free(table);
}
