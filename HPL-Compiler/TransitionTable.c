#include "HashingFunctions.h"
#include "TransitionTable.h"
#include "ErrorHandler.h"
#include "Global.h"
#include <stdlib.h>
#include <assert.h>

/* ----------------------------------------------------------------
 * Internal lookup helpers
 * ---------------------------------------------------------------- */

static StateBucket* findStateBucket(const TransitionTable* table,
                                    unsigned short state) {
    StateBucket* current;
    unsigned idx;

    idx = hashNumber(state, table->capacity);
    current = table->buckets[idx];

    while (current && current->key != state)
        current = current->next;

    return current;
}

static CharBucket* findCharBucket(const CharMap* map, char symbol) {
    CharBucket* current;
    unsigned idx;

    idx = hashChar(symbol, map->capacity);
    current = map->buckets[idx];

    while (current && current->key != symbol)
        current = current->next;

    return current;
}

/* ----------------------------------------------------------------
 * CharMap management
 * ---------------------------------------------------------------- */

static CharMap* createCharMap() {
    CharMap* map = (CharMap*)malloc(sizeof(CharMap));
    if (!map)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for CharMap");

    assert(map);
    map->capacity = INITIAL_CHAR_CAPACITY;
    map->buckets  = (CharBucket**)calloc(INITIAL_CHAR_CAPACITY, sizeof(CharBucket*));

    if (!map->buckets)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate buckets for CharMap");

    return map;
}

/*
 * Expand the CharMap from INITIAL_CHAR_CAPACITY to EXPANDED_CHAR_CAPACITY.
 * Called the first time a second entry is inserted into a state's map to
 * reduce collisions once the map starts to fill up.
 */
static void expandCharMap(CharMap* map) {
    unsigned i, newIdx;
    CharBucket** oldBuckets = map->buckets;
    CharBucket* current, *nextBucket;

    map->capacity = EXPANDED_CHAR_CAPACITY;
    map->buckets = (CharBucket**)calloc(EXPANDED_CHAR_CAPACITY, sizeof(CharBucket*));

    if (!map->buckets)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate buckets for expanded CharMap");

    assert(map->buckets);

    /* Rehash all existing entries into the new bucket array */
    for (i = 0; i < INITIAL_CHAR_CAPACITY; i++) {
        current = oldBuckets[i];
        while (current) {
            nextBucket = current->next;
            newIdx = hashChar(current->key, map->capacity);
            current->next = map->buckets[newIdx];
            map->buckets[newIdx] = current;
            current = nextBucket;
        }
    }
    free(oldBuckets);
}

/* ----------------------------------------------------------------
 * StateBucket management
 * ---------------------------------------------------------------- */

static StateBucket* createStateBucket(int state) {
    StateBucket* sb = (StateBucket*)malloc(sizeof(StateBucket));
    if (!sb)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for StateBucket");

    assert(sb);
    sb->key = state;
    sb->value = createCharMap();
    sb->token = 0;
    sb->next = NULL;
    return sb;
}

/* Return the existing bucket for `state`, or create and insert one */
static StateBucket* getOrCreateStateBucket(TransitionTable* table,
                                           unsigned short state) {
    StateBucket* sBucket;
    unsigned idx;

    sBucket = findStateBucket(table, state);
    if (!sBucket) {
        idx = hashNumber(state, table->capacity);
        sBucket = createStateBucket(state);
        sBucket->next = table->buckets[idx];
        table->buckets[idx] = sBucket;
    }
    return sBucket;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

TransitionTable* initTransitionTable() {
    TransitionTable* table = (TransitionTable*)malloc(sizeof(TransitionTable));
    if (!table)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for TransitionTable");

    assert(table);
    table->stateCounter = STATE_START;
    table->capacity = STATE_CAPACITY;
    table->buckets = (StateBucket**)calloc(STATE_CAPACITY, sizeof(StateBucket*));

    if (!table->buckets)
        reportError(ERROR_INTERNAL, 0,
                    "Failed to allocate state buckets for TransitionTable");

    /* Pre-create the start state so STATE_START always has an entry */
    getOrCreateStateBucket(table, 0);

    return table;
}

void insertTransition(TransitionTable* table, unsigned short state,
                      char symbol, unsigned short newState) {
    StateBucket* sBucket;
    CharBucket* cBucket;
    unsigned cIdx;
    CharMap* map;

    sBucket = getOrCreateStateBucket(table, state);
    map = sBucket->value;

    /* Expand the char map on first collision to reduce probe lengths */
    if (*map->buckets && map->capacity == INITIAL_CHAR_CAPACITY)
        expandCharMap(map);

    cBucket = (CharBucket*)malloc(sizeof(CharBucket));
    if (!cBucket)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate memory for CharBucket");

    assert(cBucket);
    cIdx = hashChar(symbol, map->capacity);
    cBucket->key = symbol;
    cBucket->value = newState;
    cBucket->next = map->buckets[cIdx];
    map->buckets[cIdx] = cBucket;
}

/* Returns STATE_ERROR if no transition exists for (state, symbol) */
unsigned short getState(const TransitionTable* table, unsigned short state,
                        char symbol) {
    StateBucket* sBucket;
    CharBucket* cBucket;

    sBucket = findStateBucket(table, state);
    if (!sBucket) return STATE_ERROR;

    cBucket = findCharBucket(sBucket->value, symbol);
    return cBucket ? cBucket->value : STATE_ERROR;
}

void setToken(TransitionTable* table, unsigned short state, TokenTypes token) {
    StateBucket* sBucket = getOrCreateStateBucket(table, state);
    sBucket->token = token;
}

/* Returns TOKEN_IDENT as the default when a state has no explicit token type */
TokenTypes getTokenType(const TransitionTable* table, unsigned short state) {
    StateBucket* sBucket = findStateBucket(table, state);
    return sBucket ? sBucket->token : TOKEN_IDENT;
}

static void freeCharMap(CharMap* map) {
    CharBucket* current, *temp;
    int i;

    if (!map) return;

    for (i = 0; i < map->capacity; i++) {
        current = map->buckets[i];
        while (current) {
            temp = current;
            current = current->next;
            free(temp);
        }
    }
    free(map->buckets);
    free(map);
}

void freeTransitionTable(TransitionTable* table) {
    StateBucket* current, *temp;
    int i;

    if (!table) return;

    for (i = 0; i < table->capacity; i++) {
        current = table->buckets[i];
        while (current) {
            temp = current;
            current = current->next;
            freeCharMap(temp->value);
            free(temp);
        }
    }
    free(table->buckets);
    free(table);
}
