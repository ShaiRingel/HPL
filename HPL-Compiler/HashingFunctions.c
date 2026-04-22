#include "HashingFunctions.h"

unsigned hashNumber(unsigned num, int capacity) {
    return (num * 2654435761u) % (unsigned)capacity;
}

unsigned hashChar(char c, int capacity) {
    return ((unsigned char)c * 31u) % (unsigned)capacity;
}

unsigned hashString(const char* str, int capacity) {
    unsigned hash = 5381;
    int c;

    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash % capacity;
}