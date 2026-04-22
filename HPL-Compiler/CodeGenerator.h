#pragma once
#include <stdio.h>
#include "CST.h"
#include "Global.h"
#include "DispatchTable.h"

/* Windows console handle IDs used in the runtime helpers */
#define STD_INPUT_HANDLE  "-10"
#define STD_OUTPUT_HANDLE "-11"

/* Emit one line of NASM assembly to file f, with and without a leading tab */
#define EMIT(f,  fmt, ...)  fprintf(f, fmt "\n",      ##__VA_ARGS__)
#define EMITI(f, fmt, ...)  fprintf(f, "\t" fmt "\n", ##__VA_ARGS__)

/* Emit a label definition */
#define LBL(f,  name)       fprintf(f, "%s:\n",  name)
#define LBLF(f, fmt, ...)   fprintf(f, fmt ":\n", ##__VA_ARGS__)

/* ----------------------------------------------------------------
 * Variable tracking
 * ---------------------------------------------------------------- */

/*
 * VarEntry — tracks one declared variable across code generation.
 * Locals are addressed as [rbp - offset]; globals by their __g_<name> label.
 */
typedef struct VarEntry {
    char* name;
    TokenTypes dataType;
    int offset;   /* rbp-relative byte offset for locals; 0 for globals */
    int isGlobal;
    struct VarEntry* next;
} VarEntry;

/* StrEntry — an interned string literal assigned a unique integer ID */
typedef struct StrEntry {
    int id;
    char* value;
    struct StrEntry* next;
} StrEntry;

/* FuncEntry — records which functions have been defined */
typedef struct FuncEntry {
    char* name;
    struct FuncEntry* next;
} FuncEntry;

/*
 * ParamTypeEntry — caches the inferred type of each function parameter.
 * Built during the pre-pass so that parameter frame slots can be sized
 * correctly before the function body is emitted.
 */
typedef struct ParamTypeEntry {
    char* funcName;
    int paramIndex;
    TokenTypes dataType;
    struct ParamTypeEntry* next;
} ParamTypeEntry;

/* ----------------------------------------------------------------
 * Generator state
 * ---------------------------------------------------------------- */

/*
 * GenState — mutable state threaded through all generator functions.
 *
 *   locals / globals   : linked lists of known variables
 *   nextOffset         : next available [rbp - N] slot inside a function
 *   strings            : interned string literals emitted into .data
 *   funcs              : functions seen so far (used by the linker call)
 *   inFunction         : non-zero while generating inside a function body
 *   labelCounter       : monotonically increasing integer for unique labels
 *   currentFrameBytes  : frame size of the function currently being emitted
 *   paramTypes         : pre-pass results for parameter type inference
 */
typedef struct {
    VarEntry* locals;
    int nextOffset;
    VarEntry* globals;
    StrEntry* strings;
    int strCount;
    FuncEntry* funcs;
    int inFunction;
    int labelCounter;
    int currentFrameBytes;
    ParamTypeEntry* paramTypes;
} GenState;

/* ----------------------------------------------------------------
 * Register allocator
 * ---------------------------------------------------------------- */

#define RA_POOL_SIZE 13

/*
 * RegAlloc — a trivial linear-scan register pool.
 * Used to keep intermediate expression values in registers instead of
 * always pushing/popping rax, reducing stack traffic inside expressions.
 */
typedef struct {
    FILE* f;
    const char* regs[RA_POOL_SIZE];
    int inUse[RA_POOL_SIZE];
} RegAlloc;

/* ----------------------------------------------------------------
 * Top-level code generator
 * ---------------------------------------------------------------- */

typedef struct {
    char* fileName;   /* base name of the source file (no extension)       */
    char* projRoot;   /* absolute path to the compiler's install directory  */
    char* buildRoot;  /* directory from which the compiler was invoked      */
    char* outPath;
    FILE* file;       /* open handle to the output .asm file                */
    CSTNode* root;    /* root of the CST being compiled                     */
    GenState gs;
    DispatchTable* relOpDispatch;  /* maps relational TokenTypes to jump mnemonics */
    DispatchTable* genNodeDispatch;/* maps NonTerminal values to generator functions */
} CodeGenerator;

CodeGenerator* initCodeGenerator(const char* targetPath);
void generateAndLinkCode(CodeGenerator* generator, CSTNode* node);
void freeCodeGenerator(CodeGenerator* generator);
