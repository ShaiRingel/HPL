#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "CodeGenerator.h"
#include "DispatchTable.h"
#include "ErrorHandler.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <direct.h>

/*
 * CodeGenerator — emits 64-bit NASM assembly for Windows (x64 ABI).
 *
 * Compilation model:
 *   - Two-pass over the CST:
 *       Pass 1 (firstPassFunctions)  : emit all user-defined functions
 *       Pass 2 (secondPassMain)      : emit top-level statements as "Start"
 *   - A pre-pass (paramTypePrePass) infers parameter types from call sites
 *     before any function body is emitted, so frame sizes are correct.
 *
 * Calling convention (Windows x64):
 *   - First 4 integer args in rcx, rdx, r8, r9; remainder on the stack.
 *   - Caller allocates 32 bytes of shadow space before every call.
 *   - rbx, rsi, rdi, r12–r15 are callee-saved; saved/restored per function.
 *
 * Expression evaluation:
 *   - Results are always returned in rax.
 *   - Sethi-Ullman register allocation (Sethi & Ullman, 1970):
 *     A pre-pass (suLabel) annotates every expression node with the
 *     minimum number of registers needed to evaluate it spill-free.
 *     At each binary node the heavier subtree (higher label) is evaluated
 *     first so its result sits in one register while the lighter subtree
 *     runs — minimising total registers in flight simultaneously.
 *     Spills only occur when both children have equal labels and the pool
 *     is exhausted, which is the theoretically optimal spill point.
 *   - rdx is special-cased because idiv clobbers it: flushRdxIfBorrowed()
 *     spills it around every division and restores it afterward.
 *   - Call-clobbered registers (rcx, rdx, r8-r11) are spilled and restored
 *     around every function call via flushCallClobbered().
 */

 /* ----------------------------------------------------------------
  * GenState helpers
  * ---------------------------------------------------------------- */

  /* Return a new unique integer suitable for use in a label name */
static int newLabel(GenState* gs) { return gs->labelCounter++; }

/* Search locals first, then globals; returns NULL if not found */
static VarEntry* findVar(GenState* gs, const char* name) {
    VarEntry* e;
    for (e = gs->locals; e; e = e->next) if (strcmp(e->name, name) == 0) return e;
    for (e = gs->globals; e; e = e->next) if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

/* Register a variable and assign it an rbp-relative offset (locals) or a global label */
static VarEntry* addVar(GenState* gs, const char* name, TokenTypes dataType) {
    VarEntry* e = (VarEntry*)malloc(sizeof(VarEntry));
    if (!e) reportError(ERROR_INTERNAL, 0, "OOM in addVar");
    e->name = _strdup(name);
    e->dataType = dataType;
    if (gs->inFunction) {
        gs->nextOffset += 8; /* all values are 8 bytes (qword) */
        e->offset = gs->nextOffset;
        e->isGlobal = 0;
        e->next = gs->locals;
        gs->locals = e;
    }
    else {
        e->offset = 0;
        e->isGlobal = 1;
        e->next = gs->globals;
        gs->globals = e;
    }
    return e;
}

/* Release all local VarEntries at the end of a function */
static void freeLocals(GenState* gs) {
    VarEntry* e = gs->locals;
    while (e) {
        VarEntry* n = e->next;
        free(e->name);
        free(e);
        e = n;
    }
    gs->locals = NULL;
    gs->nextOffset = 0;
}

/*
 * Emit a load of `name` into dstReg.
 * Pass NULL (or "rax") for dstReg to preserve the old behaviour;
 * all non-expression callers (assignment, bounds-check, etc.) pass NULL.
 */
static void emitLoad(GenState* gs, FILE* f, const char* name,
    const char* dstReg) {
    VarEntry* e = findVar(gs, name);
    if (!dstReg || !*dstReg) dstReg = "rax";
    if (!e) {
        fprintf(f, "\t;[codegen] unknown var: %s\n", name);
        return;
    }
    if (e->isGlobal)
        fprintf(f, "\tmov %s, [__g_%s]\n", dstReg, name);
    else
        fprintf(f, "\tmov %s, [rbp - %d]\n", dstReg, e->offset);
}

/* Emit the address of `name` into rax */
static void emitAddr(GenState* gs, FILE* f, const char* name) {
    VarEntry* e = findVar(gs, name);
    if (!e) {
        EMITI(f, ";[codegen] unknown var: %s", name);
        return;
    }
    if (e->isGlobal)
        EMITI(f, "lea rax, [__g_%s]", name);
    else
        EMITI(f, "lea rax, [rbp - %d]", e->offset);
}

/* Intern a string literal and return its integer ID for the __str_N label */
static int addString(GenState* gs, const char* value) {
    StrEntry* e = (StrEntry*)malloc(sizeof(StrEntry));
    if (!e) reportError(ERROR_INTERNAL, 0, "OOM in addString");
    e->id = gs->strCount++;
    e->value = _strdup(value);
    e->next = gs->strings;
    gs->strings = e;
    return e->id;
}

/* Record that a function has been defined (used later by the linker call) */
static void registerFunc(GenState* gs, const char* name) {
    FuncEntry* e = (FuncEntry*)malloc(sizeof(FuncEntry));
    if (!e) reportError(ERROR_INTERNAL, 0, "OOM in registerFunc");
    e->name = _strdup(name);
    e->next = gs->funcs;
    gs->funcs = e;
}

/*
 * Store the inferred type for parameter `paramIndex` of `funcName`.
 * TEXT wins over NUMBER if the same parameter is seen with both types
 * (TEXT is the conservative choice since pointer-sized qwords cover both).
 */
static void recordParamType(GenState* gs, const char* funcName,
    int paramIndex, TokenTypes type) {
    ParamTypeEntry* e;
    for (e = gs->paramTypes; e; e = e->next) {
        if (e->paramIndex == paramIndex &&
            strcmp(e->funcName, funcName) == 0) {
            if (type == TOKEN_TEXT || type == TOKEN_STRING)
                e->dataType = TOKEN_TEXT;
            return;
        }
    }
    e = (ParamTypeEntry*)malloc(sizeof(ParamTypeEntry));
    if (!e) reportError(ERROR_INTERNAL, 0, "OOM in recordParamType");
    e->funcName = _strdup(funcName);
    e->paramIndex = paramIndex;
    e->dataType = (type == TOKEN_TEXT || type == TOKEN_STRING)
        ? TOKEN_TEXT : TOKEN_NUMBER;
    e->next = gs->paramTypes;
    gs->paramTypes = e;
}

/* Returns the pre-pass inferred type, defaulting to NUMBER if unknown */
static TokenTypes lookupParamType(GenState* gs, const char* funcName,
    int paramIndex) {
    ParamTypeEntry* e;
    for (e = gs->paramTypes; e; e = e->next) {
        if (e->paramIndex == paramIndex &&
            strcmp(e->funcName, funcName) == 0)
            return e->dataType;
    }
    return TOKEN_NUMBER;
}

/* ----------------------------------------------------------------
 * Register allocator
 * ---------------------------------------------------------------- */

 /*
  * Initialise a fresh RegAlloc for one expression tree.
  * The pool excludes rax (always the result register) and rbp/rsp.
  */
static RegAlloc raMake(FILE* f) {
    int i;
    RegAlloc ra;
    ra.f = f;
    ra.regs[0] = "rbx";
    ra.regs[1] = "rcx";
    ra.regs[2] = "r10";
    ra.regs[3] = "r11";
    ra.regs[4] = "rdx";
    ra.regs[5] = "r8";
    ra.regs[6] = "r9";
    ra.regs[7] = "r12";
    ra.regs[8] = "r13";
    ra.regs[9] = "r14";
    ra.regs[10] = "r15";
    ra.regs[11] = "rsi";
    ra.regs[12] = "rdi";
    for (i = 0; i < RA_POOL_SIZE; i++) ra.inUse[i] = 0;
    return ra;
}

static const char* allocReg(RegAlloc* ra) {
    int i;
    for (i = 0; i < RA_POOL_SIZE; i++) {
        if (!ra->inUse[i]) {
            ra->inUse[i] = 1;
            return ra->regs[i];
        }
    }
    return NULL; /* pool exhausted — caller must spill to stack */
}

static void freeReg(RegAlloc* ra, const char* reg) {
    int i;
    if (!reg) return;
    for (i = 0; i < RA_POOL_SIZE; i++) {
        if (strcmp(ra->regs[i], reg) == 0) {
            ra->inUse[i] = 0;
            return;
        }
    }
}

/* ----------------------------------------------------------------
 * Division helpers
 *
 * idiv clobbers rdx (it reads rdx:rax as the dividend and writes the
 * remainder to rdx).  If rdx is currently in use by the register
 * allocator we must spill it around the division and restore it after.
 * ---------------------------------------------------------------- */

static const char* callClobbered[] = {
    "rcx", "rdx", "r8", "r9", "r10", "r11", NULL
};

static int isCallClobbered(const char* reg) {
    int i;
    for (i = 0; callClobbered[i]; i++)
        if (strcmp(callClobbered[i], reg) == 0) return 1;
    return 0;
}

static int poolIndex(RegAlloc* ra, const char* reg) {
    int i;
    for (i = 0; i < RA_POOL_SIZE; i++)
        if (strcmp(ra->regs[i], reg) == 0) return i;
    return -1;
}

/* Spill rdx if the allocator has it in use; returns 1 if it was spilled */
static int flushRdxIfBorrowed(RegAlloc* ra) {
    int idx = poolIndex(ra, "rdx");
    if (idx < 0 || !ra->inUse[idx]) return 0;
    fprintf(ra->f, "\tpush rdx\n");
    ra->inUse[idx] = 0;
    return 1;
}

/* Restore rdx after a division that spilled it */
static void unflushRdx(RegAlloc* ra) {
    int idx = poolIndex(ra, "rdx");
    if (idx < 0) return;
    fprintf(ra->f, "\tpop rdx\n");
    ra->inUse[idx] = 1;
}

/* Save all call-clobbered registers that are currently in use; returns a bitmask */
static unsigned flushCallClobbered(RegAlloc* ra) {
    int i;
    unsigned mask = 0;
    for (i = 0; i < RA_POOL_SIZE; i++) {
        if (ra->inUse[i] && isCallClobbered(ra->regs[i])) {
            fprintf(ra->f, "\tpush %s\n", ra->regs[i]);
            ra->inUse[i] = 0;
            mask |= (1u << i);
        }
    }
    return mask;
}

/* Restore registers saved by flushCallClobbered, in reverse order */
static void unflushCallClobbered(RegAlloc* ra, unsigned mask) {
    int i;
    for (i = RA_POOL_SIZE - 1; i >= 0; i--) {
        if (mask & (1u << i)) {
            fprintf(ra->f, "\tpop %s\n", ra->regs[i]);
            ra->inUse[i] = 1;
        }
    }
}

/* ----------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------- */
static void generateNode(CodeGenerator* gen, CSTNode* node);
static void generateBlock(CodeGenerator* gen, CSTNode* node);
static void generateFuncDecl(CodeGenerator* gen, CSTNode* node);
static TokenTypes generateExpr(CodeGenerator* gen, CSTNode* node);
static TokenTypes generateExprRA(CodeGenerator* gen, CSTNode* node,
    RegAlloc* ra, const char* targetReg);
static TokenTypes generateTermRA(CodeGenerator* gen, CSTNode* node,
    RegAlloc* ra, const char* targetReg);
static TokenTypes generateFactorRA(CodeGenerator* gen, CSTNode* node,
    RegAlloc* ra, const char* targetReg);
static TokenTypes generateLVal(CodeGenerator* gen, CSTNode* node);
static TokenTypes generateLValInto(CodeGenerator* gen, CSTNode* node,
    const char* dstReg);
static void generateLValAddr(CodeGenerator* gen, CSTNode* node);
static void generateFuncCall(CodeGenerator* gen, CSTNode* node, RegAlloc* ra);
static void generateCond(CodeGenerator* gen, CSTNode* node, const char* falseLbl);
static void generateBoolTerm(CodeGenerator* gen, CSTNode* node, const char* falseLbl);
static void generateBoolFact(CodeGenerator* gen, CSTNode* node, const char* falseLbl);
static TokenTypes inferArgType(GenState* gs, CSTNode* node);
static void paramTypePrePass(GenState* gs, CSTNode* node);

/* ----------------------------------------------------------------
 * Sethi-Ullman register allocation
 *
 * The Sethi-Ullman algorithm (Sethi & Ullman, 1970) minimises register
 * usage across expression trees in two phases:
 *
 *   Phase 1 — label pass (SemanticAnalyzer.c, getExpressionType):
 *     During semantic analysis every expression node is stamped with
 *     node->suLabel: the minimum registers needed to evaluate that
 *     subtree spill-free.  Labels are computed once and travel with
 *     the CST so no re-traversal is needed here.
 *
 *   Phase 2 — code generation (here):
 *     At each binary node, evaluate the heavier subtree (higher suLabel)
 *     first.  Its result occupies one register while the lighter subtree
 *     runs — minimising total registers in flight simultaneously.
 *     Spills occur only when both subtrees have equal labels and the pool
 *     is exhausted — the theoretically optimal spill point.
 *
 * Constraints specific to this target:
 *   - rax is always the result register; it is never in the pool.
 *   - rdx is clobbered by idiv/cqo; flushRdxIfBorrowed() handles it.
 *   - Function calls clobber rcx, rdx, r8-r11; flushCallClobbered() saves
 *     and restores them around every call site.
 * ---------------------------------------------------------------- */

 /*
  * Count the number of currently free registers in the pool.
  * Used to decide whether to spill when both subtrees have equal need.
  */
static int raFreeCount(RegAlloc* ra) {
    int i, count = 0;
    for (i = 0; i < RA_POOL_SIZE; i++)
        if (!ra->inUse[i]) count++;
    return count;
}

static TokenTypes generateExpr(CodeGenerator* gen, CSTNode* node) {
    RegAlloc ra = raMake(gen->file);
    return generateExprRA(gen, node, &ra, "rax");
}

/*
 * Emit code for a binary arithmetic operation using Sethi-Ullman ordering.
 *
 *   lhsNode / rhsNode : the two operand subtrees
 *   genLeft / genRight: generator functions for each side
 *   ra                : current register pool state
 *   isDivision        : non-zero if the op is idiv (needs rdx free)
 *   emitOp            : called with (f, lhsReg) to emit the final instruction
 *                       after both operands are ready in rax (rhs) and lhsReg
 *   targetReg         : register the caller wants the result in; the result
 *                       of this call is always placed in targetReg (which may
 *                       be rax when the caller has not allocated a scratch).
 *
 * After this call the result is in targetReg.
 *
 * Targeted-register strategy:
 *   Instead of always writing to rax and then having the parent emit
 *   "mov <scratchReg>, rax", we accept a targetReg from the caller.
 *   The heavy subtree is evaluated first: if the caller passed a real
 *   scratch register as targetReg we write the heavy result directly there,
 *   avoiding the extra mov entirely.  The light subtree is then evaluated
 *   and the final operation combines them.
 */
typedef void (*EmitOpFn)(FILE* f, const char* lhsReg);
static void emitAdd(FILE* f, const char* lhsReg);
static void emitSub(FILE* f, const char* lhsReg);
static void emitMul(FILE* f, const char* lhsReg);
static void emitDiv(FILE* f, const char* lhsReg);
static void emitMod(FILE* f, const char* lhsReg);

static void suBinaryOp(CodeGenerator* gen, RegAlloc* ra,
    CSTNode* lhsNode, CSTNode* rhsNode,
    TokenTypes(*genLeft)(CodeGenerator*, CSTNode*, RegAlloc*, const char*),
    TokenTypes(*genRight)(CodeGenerator*, CSTNode*, RegAlloc*, const char*),
    EmitOpFn emitOp, int isDivision, const char* targetReg) {
    FILE* f = gen->file;
    int leftNeed = lhsNode->suLabel;
    int rightNeed = rhsNode->suLabel;
    int spill = 0;
    const char* lhsReg;
    int s = 0;

    /*
     * targetReg is where the caller wants the final result.
     * If targetReg is already a scratch register allocated by the caller
     * we can write the heavy operand directly into it, skipping the
     * "mov <scratch>, rax" that the old code always emitted.
     *
     * For both orderings the invariant at the emitOp call is:
     *   rax     = the lighter (or equal) operand
     *   lhsReg  = the heavier operand (what was computed first)
     * emitOp(f, lhsReg) then does:  lhsReg op= rax  and moves into rax.
     * We finish with:  mov targetReg, rax  only when targetReg != rax.
     */

    if (rightNeed > leftNeed) {
        /*
         * Right is heavier: allocate a scratch for it (or reuse targetReg),
         * evaluate right DIRECTLY into lhsReg — no rax round-trip —
         * then evaluate left into rax, and combine.
         */
        lhsReg = (targetReg && strcmp(targetReg, "rax") != 0)
            ? targetReg : allocReg(ra);

        if (lhsReg) {
            /*
             * genRight honours lhsReg as targetReg all the way down, so
             * for leaves (suLabel == 1) this emits  mov lhsReg, [mem]
             * directly, and for complex subtrees it finishes with the
             * result already in lhsReg — no extra mov needed either way.
             */
            genRight(gen, rhsNode, ra, lhsReg); /* heavy right -> lhsReg directly */
            /* Pin lhsReg so nested spills inside genLeft cannot clobber rhs. */
            {
                int pin_ = poolIndex(ra, lhsReg);
                int wasLive_ = (pin_ >= 0) ? ra->inUse[pin_] : 1;
                if (pin_ >= 0) ra->inUse[pin_] = 1;
                genLeft(gen, lhsNode, ra, "rax");   /* light left  -> rax             */
                if (pin_ >= 0) ra->inUse[pin_] = wasLive_;
            }
            /* Now: rax = lhs, lhsReg = rhs */
            if (isDivision) s = flushRdxIfBorrowed(ra);
            emitOp(f, lhsReg);
            if (isDivision && s) unflushRdx(ra);
            if (!isDivision && lhsReg != targetReg) {
                fprintf(f, "\tmov rax, %s\n", lhsReg);
                freeReg(ra, lhsReg);
            }
            else if (isDivision && lhsReg != targetReg) {
                freeReg(ra, lhsReg);
            }
        }
        else {
            /* Pool exhausted — spill (right-heavy variant).
             * Pure stack-based, fully re-entrant.
             * Right is heavier so evaluate it first:
             *   genRight -> rax ; push rax     (rhs on stack)
             *   genLeft  -> rax               (lhs in rax)
             *   xchg rax, [rsp]               (rax=rhs, [rsp]=lhs)
             * Now we need  lhs op rhs  with lhs=[rsp] rhs=rax:
             *   xchg rax, [rsp] again         (rax=lhs, [rsp]=rhs)
             *   apply op rax, [rsp]
             *   add rsp, 8
             */
            genRight(gen, rhsNode, ra, "rax");
            EMITI(f, "push rax");              /* rhs -> stack       */
            genLeft(gen, lhsNode, ra, "rax");  /* lhs -> rax         */
            /* Now rax=lhs, [rsp]=rhs */
            if (isDivision) {
                s = flushRdxIfBorrowed(ra);
                EMITI(f, "cqo");
                EMITI(f, "idiv qword [rsp]");  /* rax = lhs / rhs    */
                if (s) unflushRdx(ra);
            }
            else if (emitOp == emitAdd) {
                EMITI(f, "add rax, [rsp]");    /* rax = lhs + rhs    */
            }
            else if (emitOp == emitSub) {
                EMITI(f, "sub rax, [rsp]");    /* rax = lhs - rhs    */
            }
            else {
                EMITI(f, "imul rax, [rsp]");   /* rax = lhs * rhs    */
            }
            EMITI(f, "add rsp, 8");            /* pop rhs slot       */
            if (targetReg && strcmp(targetReg, "rax") != 0)
                fprintf(f, "\tmov %s, rax\n", targetReg);
        }
    }
    else {
        /*
         * Left is heavier or equal: evaluate left first.
         * Write left directly into targetReg when possible, then evaluate
         * right into rax.
         */
        lhsReg = (targetReg && strcmp(targetReg, "rax") != 0)
            ? targetReg : allocReg(ra);

        if (!lhsReg || (leftNeed == rightNeed && raFreeCount(ra) == 0)) {
            /* Spill path — pool exhausted.
             * Pure stack-based: no scratch register other than rax.
             * Fully re-entrant because we never store anything in a
             * named register between genLeft and genRight.
             *
             *   genLeft  -> rax  ; push rax        (lhs on stack)
             *   genRight -> rax                    (rhs in rax)
             *   xchg rax, [rsp]                   (rax=lhs, [rsp]=rhs)
             *   apply op  rax, [rsp]  or  idiv [rsp]
             *   add rsp, 8                         (clean stack)
             *   mov targetReg, rax  if needed
             */
            if (lhsReg && lhsReg != targetReg) { freeReg(ra, lhsReg); lhsReg = NULL; }
            spill = 1; lhsReg = NULL;
            genLeft(gen, lhsNode, ra, "rax");
            EMITI(f, "push rax");              /* lhs -> stack       */
            genRight(gen, rhsNode, ra, "rax"); /* rhs -> rax         */
            EMITI(f, "xchg rax, [rsp]");       /* rax=lhs, [rsp]=rhs */
            if (isDivision) {
                s = flushRdxIfBorrowed(ra);
                EMITI(f, "cqo");
                EMITI(f, "idiv qword [rsp]");  /* rax = lhs / rhs    */
                if (s) unflushRdx(ra);
            }
            else if (emitOp == emitAdd) {
                EMITI(f, "add rax, [rsp]");    /* rax = lhs + rhs    */
            }
            else if (emitOp == emitSub) {
                EMITI(f, "sub rax, [rsp]");    /* rax = lhs - rhs    */
            }
            else {
                EMITI(f, "imul rax, [rsp]");   /* rax = lhs * rhs    */
            }
            EMITI(f, "add rsp, 8");            /* pop rhs slot       */
            if (targetReg && strcmp(targetReg, "rax") != 0)
                fprintf(f, "\tmov %s, rax\n", targetReg);
        }
        else {
            /*
             * Register path: evaluate left directly into lhsReg (which is
             * either the caller's targetReg or a freshly allocated scratch).
             * Then evaluate right into rax and combine.
             */
            genLeft(gen, lhsNode, ra, lhsReg);
            /*
             * genLeft honours lhsReg as targetReg all the way down through
             * generateExprRA/generateTermRA/generateFactorRA, so the result
             * is always in lhsReg after this call — for both plain leaves
             * (suLabel == 1) and complex subtrees (suLabel > 1).
             * No extra mov is needed here.
             *
             * Pin lhsReg in the allocator so that nested spill paths inside
             * genRight cannot allocate it and clobber the lhs value.
             * lhsReg is already marked inUse when it came from allocReg();
             * this only matters when lhsReg == targetReg (caller-owned reg).
             */
            {
                int pin_ = poolIndex(ra, lhsReg);
                int wasLive_ = (pin_ >= 0) ? ra->inUse[pin_] : 1;
                if (pin_ >= 0) ra->inUse[pin_] = 1;
                genRight(gen, rhsNode, ra, "rax");
                /* Restore previous lhsReg state so freeReg/allocReg work correctly */
                if (pin_ >= 0) ra->inUse[pin_] = wasLive_;
            }
            if (isDivision) s = flushRdxIfBorrowed(ra);
            emitOp(f, lhsReg);
            if (isDivision && s) unflushRdx(ra);
            /*
             * Move result to targetReg when they differ:
             *   add/sub/mul: result is in lhsReg; move to targetReg.
             *   div/mod:     idiv writes to rax; move to targetReg if needed.
             * When lhsReg == targetReg the result is already where it belongs.
             */
            if (lhsReg != targetReg) {
                const char* src_ = isDivision ? "rax" : lhsReg;
                if (targetReg && strcmp(targetReg, src_) != 0)
                    fprintf(f, "\tmov %s, %s\n", targetReg, src_);
                freeReg(ra, lhsReg);
            }
        }
    }
    (void)spill;
}

/* ----------------------------------------------------------------
 * EmitOpFn callbacks — one per operation
 * ---------------------------------------------------------------- */
static void emitAdd(FILE* f, const char* lhsReg) {
    /* add lhsReg, rax  — result stays in lhsReg (which is targetReg) */
    fprintf(f, "\tadd %s, rax\n", lhsReg);
}

static void emitSub(FILE* f, const char* lhsReg) {
    /* sub lhsReg, rax  — lhsReg = lhs - rhs, result stays in lhsReg */
    fprintf(f, "\tsub %s, rax\n", lhsReg);
}

static void emitMul(FILE* f, const char* lhsReg) {
    /* imul lhsReg, rax  — result stays in lhsReg */
    fprintf(f, "\timul %s, rax\n", lhsReg);
}

static void emitDiv(FILE* f, const char* lhsReg) {
    /*
     * idiv expects: rax = lhs (dividend), rbx = rhs (divisor)
     * rhs is currently in rax, lhs in lhsReg — swap then divide.
     */
    fprintf(f, "\txchg rax, %s\n", lhsReg); /* rax = lhs, lhsReg = rhs (divisor) */
    fprintf(f, "\tmov rbx, %s\n", lhsReg);
    fprintf(f, "\tcqo\n");
    fprintf(f, "\tidiv rbx\n");
    /* result (quotient) is already in rax */
}

static void emitMod(FILE* f, const char* lhsReg) {
    /* Same as div but take rdx (remainder) */
    fprintf(f, "\txchg rax, %s\n", lhsReg);
    fprintf(f, "\tmov rbx, %s\n", lhsReg);
    fprintf(f, "\tcqo\n");
    fprintf(f, "\tidiv rbx\n");
    fprintf(f, "\tmov rax, rdx\n");
}

/* ----------------------------------------------------------------
 * Expression generator — additive level (E -> T E_OP T ...)
 * ---------------------------------------------------------------- */
static TokenTypes generateExprRA(CodeGenerator* gen, CSTNode* node,
    RegAlloc* ra, const char* targetReg) {
    TokenTypes t = TOKEN_NUMBER;

    if (!node) {
        /* nothing */
    }
    else if (node->symbol != NON_TERMINAL_E) {
        t = generateTermRA(gen, node, ra, targetReg);
    }
    else {
        CSTNode* eop;
        CSTNode* lhsT;

        /*
         * Walk the E -> T (E_OP T)* chain using suBinaryOp for every step.
         * suBinaryOp honours targetReg directly — no rax round-trip needed.
         *
         * For a single term (no E_OP siblings) just pass through to
         * generateTermRA with the caller's targetReg unchanged.
         *
         * For a multi-step chain (a + b + c + ...) we fold left:
         *   step 1: suBinaryOp(lhs=T0, rhs=T1) -> targetReg
         *   step 2: suBinaryOp(lhs=result-so-far, rhs=T2) -> targetReg
         * Between steps the running result lives in targetReg (or rax when
         * targetReg IS rax), so we synthesise a fake "already-evaluated"
         * lhs node whose suLabel is 1 (one register needed to hold it).
         */
        lhsT = node->firstChild;
        eop = lhsT->nextSibling;

        if (!eop) {
            /* Single-term passthrough */
            t = generateTermRA(gen, lhsT, ra, targetReg);
        }
        else {
            /* First binary step: real lhs vs real rhs */
            CSTNode* opTok = eop->firstChild;
            CSTNode* rhsT = eop->nextSibling;
            EmitOpFn emitOp = (opTok->token->type == TOKEN_ADD) ? emitAdd : emitSub;

            suBinaryOp(gen, ra, lhsT, rhsT,
                generateTermRA, generateTermRA,
                emitOp, 0, targetReg);
            t = TOKEN_NUMBER;

            eop = rhsT ? rhsT->nextSibling : NULL;
            while (eop && eop->symbol == NON_TERMINAL_E_OP) {
                /*
                 * Subsequent steps: the accumulator is already in targetReg.
                 * Treat it as a weight-1 lhs by reusing suBinaryOp's
                 * right-is-heavier branch: evaluate rhs into a scratch reg,
                 * then apply the op against targetReg directly.
                 */
                FILE* f = gen->file;
                const char* accReg = targetReg ? targetReg : "rax";
                opTok = eop->firstChild;
                rhsT = eop->nextSibling;
                emitOp = (opTok->token->type == TOKEN_ADD) ? emitAdd : emitSub;

                /* Eval rhs into rax, then combine with accReg */
                generateTermRA(gen, rhsT, ra, "rax");
                emitOp(f, accReg);   /* accReg op= rax */

                eop = rhsT ? rhsT->nextSibling : NULL;
            }
        }
    }
    return t;
}

/* ----------------------------------------------------------------
 * Expression generator — multiplicative level
 * (T -> F T_OP F ... | remainderOf F dividedBy F)
 * ---------------------------------------------------------------- */
static TokenTypes generateTermRA(CodeGenerator* gen, CSTNode* node,
    RegAlloc* ra, const char* targetReg) {
    FILE* f = gen->file;
    TokenTypes t;

    if (!node) return TOKEN_NUMBER;

    if (node->symbol == NON_TERMINAL_E) {
        t = generateExprRA(gen, node, ra, targetReg);
    }
    else if (node->symbol != NON_TERMINAL_T) {
        t = generateFactorRA(gen, node, ra, targetReg);
    }
    else {
        CSTNode* first = node->firstChild;

        if (first->token && first->token->type == TOKEN_REMAINDEROF) {
            /*
             * remainderOf lhs dividedBy rhs
             * Use Sethi-Ullman to order the two operands.
             */
            CSTNode* lhsOperand = first->nextSibling;
            CSTNode* rhsOperand = first->nextSibling->nextSibling->nextSibling;
            suBinaryOp(gen, ra, lhsOperand, rhsOperand,
                generateFactorRA, generateFactorRA,
                emitMod, 1, targetReg);
            t = TOKEN_NUMBER;
        }
        else {
            CSTNode* top;
            CSTNode* lhsF;

            lhsF = first;
            top = lhsF->nextSibling;

            if (!top) {
                /* Single-factor passthrough */
                t = generateFactorRA(gen, lhsF, ra, targetReg);
            }
            else {
                /* First binary step */
                CSTNode* opTok = top->firstChild;
                CSTNode* rhsF = top->nextSibling;
                int isDiv = (opTok->token->type != TOKEN_MUL);
                EmitOpFn emitOp = isDiv ? emitDiv : emitMul;

                suBinaryOp(gen, ra, lhsF, rhsF,
                    generateFactorRA, generateFactorRA,
                    emitOp, isDiv, targetReg);
                t = TOKEN_NUMBER;

                top = rhsF ? rhsF->nextSibling : NULL;
                while (top && top->symbol == NON_TERMINAL_T_OP) {
                    /*
                     * Subsequent steps: accumulator is already in targetReg.
                     * Eval rhs into rax and combine.
                     */
                    FILE* f = gen->file;
                    const char* accReg = targetReg ? targetReg : "rax";
                    int s;
                    opTok = top->firstChild;
                    rhsF = top->nextSibling;
                    isDiv = (opTok->token->type != TOKEN_MUL);
                    emitOp = isDiv ? emitDiv : emitMul;

                    generateFactorRA(gen, rhsF, ra, "rax");
                    if (isDiv) {
                        s = flushRdxIfBorrowed(ra);
                        emitOp(f, accReg);
                        if (s) unflushRdx(ra);
                    }
                    else {
                        emitOp(f, accReg);   /* accReg *= rax */
                    }

                    top = rhsF ? rhsF->nextSibling : NULL;
                }

                /* Result is already in targetReg — nothing more to do. */
            }
        }
    }
    return t;
}

/* ----------------------------------------------------------------
 * Expression generator — atom level (F -> literal | lval | call | '(' E ')')
 * ---------------------------------------------------------------- */
static TokenTypes generateFactorRA(CodeGenerator* gen, CSTNode* node,
    RegAlloc* ra, const char* targetReg) {
    FILE* f = gen->file;
    TokenTypes result = TOKEN_NUMBER;
    CSTNode* inner;

    if (!node) return result;

    /* Unwrap an F node to its single child */
    inner = (node->symbol == NON_TERMINAL_F) ? node->firstChild : node;
    if (!inner) return result;

    /*
     * targetReg is where the caller wants the result.  For leaf nodes
     * (literals and variable loads) we emit directly into targetReg,
     * eliminating the "mov rax, X ; mov <scratch>, rax" pattern the old
     * code always produced.
     *
     * For complex sub-expressions we recurse normally; the sub-expression
     * always lands in rax, and the caller is responsible for the mov if
     * it wanted a different destination.
     */

     /* Effective destination: use targetReg when given, else "rax" */
    const char* dst = (targetReg && *targetReg) ? targetReg : "rax";

    if (inner->token && inner->token->type == TOKEN_LPAREN) {
        /* Parenthesised expression — recurse; result in dst */
        result = generateExprRA(gen, inner->nextSibling, ra, dst);
    }
    else if (inner->symbol == NON_TERMINAL_E) {
        result = generateExprRA(gen, inner, ra, dst);
    }
    else if (inner->symbol == NON_TERMINAL_T) {
        result = generateTermRA(gen, inner, ra, dst);
    }
    else if (inner->symbol == NON_TERMINAL_LVAL) {
        /*
         * Variable load directly into dst — no rax round-trip.
         * generateLValInto emits  mov dst, [var]  for plain scalars.
         * For atPosition the index logic uses rax internally, but
         * generateLValInto still finishes with the value in dst.
         */
        result = generateLValInto(gen, inner, dst);
    }
    else if (inner->symbol == NON_TERMINAL_FUNC_CALL) {
        VarEntry* ve;
        generateFuncCall(gen, inner, ra);
        ve = findVar(&gen->gs, inner->firstChild->token->lexeme);
        result = ve ? ve->dataType : TOKEN_NUMBER;
        /* Function result is always in rax; move if caller wants elsewhere */
        if (strcmp(dst, "rax") != 0)
            fprintf(f, "\tmov %s, rax\n", dst);
    }
    else if (inner->token && inner->token->type == TOKEN_NUMBER) {
        /*
         * Numeric literal: load directly into dst.
         * Old code: mov rax, N  (parent then did: mov <scratch>, rax)
         * New code: mov dst,  N  (no extra mov needed)
         */
        fprintf(f, "\tmov %s, %s\n", dst, inner->token->lexeme);
        result = TOKEN_NUMBER;
    }
    else if (inner->token && inner->token->type == TOKEN_STRING) {
        /* Strip the surrounding quotes and intern the content */
        const char* raw = inner->token->lexeme;
        int len = (int)strlen(raw);
        char* strContent = (char*)malloc(len + 1);
        int sid;
        if (!strContent)
            reportError(ERROR_INTERNAL, 0, "OOM in generateFactorRA");
        strncpy(strContent, raw + 1, len - 2);
        strContent[len - 2] = '\0';
        sid = addString(&gen->gs, strContent);
        free(strContent);
        /*
         * String literal address: lea directly into dst.
         * Old code: lea rax, [__str_N]  (parent then did: mov <scratch>, rax)
         * New code: lea dst,  [__str_N]
         */
        fprintf(f, "\tlea %s, [__str_%d]\n", dst, sid);
        result = TOKEN_TEXT;
    }

    return result;
}

/*
 * Load the value of an lval.
 *
 * generateLValInto(dstReg) — used by the expression generators.
 *   For a plain variable it emits a single  mov dstReg, [var]  with no
 *   intermediate rax touch.  For atPosition the index logic forces rax
 *   as a staging register, so dstReg is honoured only at the end.
 *
 * generateLVal() — thin wrapper that calls generateLValInto("rax"),
 *   preserving the old contract for all non-expression callers.
 */
static TokenTypes generateLValInto(CodeGenerator* gen, CSTNode* node,
    const char* dstReg) {
    FILE* f = gen->file;
    CSTNode* identNode = node->firstChild;
    const char* name = identNode->token->lexeme;
    TokenTypes result;

    if (!dstReg || !*dstReg) dstReg = "rax";

    if (identNode->nextSibling &&
        identNode->nextSibling->token &&
        identNode->nextSibling->token->type == TOKEN_ATPOSITION) {
        /* atPosition: index arithmetic requires rax as a staging register.
         * We do the full sequence into rax, then move to dstReg at the end
         * only if dstReg is not rax. */
        CSTNode* idxExpr = identNode->nextSibling->nextSibling;
        generateExpr(gen, idxExpr);
        EMITI(f, "dec rax");
        EMITI(f, "push rax");
        emitLoad(&gen->gs, f, name, NULL);   /* must land in rax for the mov rdx below */
        EMITI(f, "mov rdx, rax");
        EMITI(f, "mov rcx, [rsp]");
        EMITI(f, "sub rsp, 32");
        EMITI(f, "call __bounds_check");
        EMITI(f, "add rsp, 32");
        emitLoad(&gen->gs, f, name, NULL);
        EMITI(f, "pop rbx");
        EMITI(f, "movzx rax, byte [rax + rbx]");
        EMITI(f, "mov [__char_buf], al");
        EMITI(f, "mov byte [__char_buf + 1], 0");
        EMITI(f, "lea rax, [__char_buf]");
        if (strcmp(dstReg, "rax") != 0)
            fprintf(f, "\tmov %s, rax\n", dstReg);
        result = TOKEN_TEXT;
    }
    else {
        /* Plain scalar: load directly into dstReg — zero extra instructions. */
        emitLoad(&gen->gs, f, name, dstReg);
        VarEntry* ve = findVar(&gen->gs, name);
        result = ve ? ve->dataType : TOKEN_NUMBER;
    }
    return result;
}

static TokenTypes generateLVal(CodeGenerator* gen, CSTNode* node) {
    return generateLValInto(gen, node, "rax");
}

/* Compute the address of an lval into rax (used for assignment targets) */
static void generateLValAddr(CodeGenerator* gen, CSTNode* node) {
    FILE* f = gen->file;
    CSTNode* identNode = node->firstChild;
    const char* name = identNode->token->lexeme;

    if (identNode->nextSibling &&
        identNode->nextSibling->token &&
        identNode->nextSibling->token->type == TOKEN_ATPOSITION) {
        CSTNode* idxExpr = identNode->nextSibling->nextSibling;
        generateExpr(gen, idxExpr);
        EMITI(f, "dec rax");
        EMITI(f, "push rax");
        emitLoad(&gen->gs, f, name, NULL);
        EMITI(f, "mov rdx, rax");
        EMITI(f, "mov rcx, [rsp]");
        EMITI(f, "sub rsp, 32");
        EMITI(f, "call __bounds_check");
        EMITI(f, "add rsp, 32");
        emitLoad(&gen->gs, f, name, NULL);
        EMITI(f, "pop rbx");
        EMITI(f, "lea rax, [rax + rbx]");
    }
    else {
        emitAddr(&gen->gs, f, name);
    }
}

/* ----------------------------------------------------------------
 * Condition generators
 *
 * Conditions use a "fall-through on true, jump on false" strategy.
 * generateCond() is called with a label to jump to when the condition
 * is false; it falls through naturally when the condition is true.
 *
 * OR is short-circuit: each term gets a "next" label; on success it
 * jumps past the final "jmp falseLbl" to __or_end.
 * AND is short-circuit: each factor jumps directly to falseLbl on failure.
 * NOT inverts by swapping the true/false labels for the inner factor.
 * ---------------------------------------------------------------- */

 /*
  * The relOpDispatch table maps each relational TokenTypes to the
  * conditional jump mnemonic that jumps when the condition is FALSE
  * (i.e. the inverse, because we jump to falseLbl).
  */
static void buildRelOpDispatch(CodeGenerator* gen) {
    if (gen->relOpDispatch) return;
    gen->relOpDispatch = initDispatchTable(16);
    dispatchSet(gen->relOpDispatch, TOKEN_EQTO, (DispatchFn)(size_t)"jne");
    dispatchSet(gen->relOpDispatch, TOKEN_NOTEQ, (DispatchFn)(size_t)"je ");
    dispatchSet(gen->relOpDispatch, TOKEN_LT, (DispatchFn)(size_t)"jge");
    dispatchSet(gen->relOpDispatch, TOKEN_ATMOST, (DispatchFn)(size_t)"jg ");
    dispatchSet(gen->relOpDispatch, TOKEN_GT, (DispatchFn)(size_t)"jle");
    dispatchSet(gen->relOpDispatch, TOKEN_ATLEAST, (DispatchFn)(size_t)"jl ");
}

static void generateRelOp(CodeGenerator* gen, CSTNode* relOpNode,
    const char* falseLbl) {
    FILE* f = gen->file;
    TokenTypes op = relOpNode->firstChild->token->type;
    const char* mnemonic;

    buildRelOpDispatch(gen);
    mnemonic = (const char*)(size_t)dispatchGet(gen->relOpDispatch, op);
    if (!mnemonic) mnemonic = "jne";
    EMITI(f, "%s %s", mnemonic, falseLbl);
}

static void generateBoolFact(CodeGenerator* gen, CSTNode* node,
    const char* falseLbl) {
    FILE* f = gen->file;
    CSTNode* first = node->firstChild;

    if (!first) {
        /* nothing */
    }
    else if (first->token && first->token->type == TOKEN_LPAREN) {
        generateCond(gen, first->nextSibling, falseLbl);
    }
    else if (first->token && first->token->type == TOKEN_NOT) {
        /* Invert: jump to a temporary "true" label, then fall to falseLbl */
        int tid = newLabel(&gen->gs);
        char trueLbl[40];
        sprintf(trueLbl, "__not_true_%d", tid);
        generateBoolFact(gen, first->nextSibling, trueLbl);
        EMITI(f, "jmp %s", falseLbl);
        LBL(f, trueLbl);
    }
    else {
        /*
         * Simple comparison: evaluate lhs and rhs, cmp, conditional jump.
         * Apply Sethi-Ullman ordering: evaluate the heavier operand first.
         * generateBoolFact has no RegAlloc parameter, so we create a local
         * one just for the one scratch register needed here.
         */
        CSTNode* lhsNode = first;
        CSTNode* rhsNode = first->nextSibling->nextSibling;
        int leftNeed = lhsNode->suLabel;
        int rightNeed = rhsNode->suLabel;
        RegAlloc localRa = raMake(f);
        const char* lhsReg = allocReg(&localRa);

        if (rightNeed > leftNeed && lhsReg) {
            /* Evaluate heavier rhs first, save it, evaluate lhs, then cmp */
            generateExpr(gen, rhsNode);
            fprintf(f, "\tmov %s, rax\n", lhsReg);
            generateExpr(gen, lhsNode);
            fprintf(f, "\tcmp rax, %s\n", lhsReg); /* lhs cmp rhs */
        }
        else if (lhsReg) {
            /* Normal left-first: save lhs, evaluate rhs, then cmp */
            generateExpr(gen, lhsNode);
            fprintf(f, "\tmov %s, rax\n", lhsReg);
            generateExpr(gen, rhsNode);
            fprintf(f, "\tcmp %s, rax\n", lhsReg); /* lhs cmp rhs */
        }
        else {
            /* Pool exhausted — spill */
            generateExpr(gen, lhsNode);
            EMITI(f, "push rax");
            generateExpr(gen, rhsNode);
            EMITI(f, "mov rbx, rax");
            EMITI(f, "pop rax");
            EMITI(f, "cmp rax, rbx");
        }
        generateRelOp(gen, first->nextSibling, falseLbl);
    }
}

/*
 * AND term: collect all BOOL_F factors, emit them in order.
 * Each factor jumps to falseLbl on failure (short-circuit AND).
 * Uses an iterative stack to avoid recursion on deeply nested conditions.
 */
static void generateBoolTerm(CodeGenerator* gen, CSTNode* node,
    const char* falseLbl) {
    CSTNode* factors[64]; int nFactors = 0;
    CSTNode* stk[64];     int top = 0;
    int i;

    stk[top++] = node;
    while (top > 0) {
        CSTNode* curr = stk[--top];
        if (!curr) continue;
        if (curr->symbol == NON_TERMINAL_BOOL_F) {
            if (nFactors < 64) factors[nFactors++] = curr;
        }
        else if (curr->symbol == NON_TERMINAL_BOOL_T) {
            CSTNode* child = curr->firstChild;
            int base = top;
            int lo, hi;
            while (child && top < 64) {
                stk[top++] = child;
                child = child->nextSibling;
            }
            lo = base; hi = top - 1;
            while (lo < hi) {
                CSTNode* tmp = stk[lo]; stk[lo] = stk[hi]; stk[hi] = tmp;
                lo++; hi--;
            }
        }
    }

    for (i = 0; i < nFactors; i++)
        generateBoolFact(gen, factors[i], falseLbl);
}

/*
 * OR condition: collect all BOOL_T terms.
 * If only one term, emit it directly.
 * For multiple terms: each term gets a "next" label; success jumps to
 * __or_end, failure falls through to the next term's test, and after
 * all terms "jmp falseLbl" is emitted.
 */
static void generateCond(CodeGenerator* gen, CSTNode* node,
    const char* falseLbl) {
    FILE* f = gen->file;
    CSTNode* terms[64]; int nTerms = 0;
    CSTNode* stk[64];   int top = 0;
    int i;

    if (!node) return;

    stk[top++] = node;
    while (top > 0) {
        CSTNode* curr = stk[--top];
        if (!curr) continue;
        if (curr->symbol == NON_TERMINAL_BOOL_T) {
            if (nTerms < 64) terms[nTerms++] = curr;
        }
        else if (curr->symbol == NON_TERMINAL_COND) {
            CSTNode* child = curr->firstChild;
            int base = top;
            int lo, hi;
            while (child && top < 64) {
                stk[top++] = child;
                child = child->nextSibling;
            }
            lo = base; hi = top - 1;
            while (lo < hi) {
                CSTNode* tmp = stk[lo]; stk[lo] = stk[hi]; stk[hi] = tmp;
                lo++; hi--;
            }
        }
    }

    if (nTerms <= 1) {
        if (nTerms == 1) generateBoolTerm(gen, terms[0], falseLbl);
        return;
    }

    {
        int endId = newLabel(&gen->gs);
        char endLbl[40];
        sprintf(endLbl, "__or_end_%d", endId);

        for (i = 0; i < nTerms; i++) {
            int nid = newLabel(&gen->gs);
            char nLbl[40];
            sprintf(nLbl, "__or_next_%d", nid);
            generateBoolTerm(gen, terms[i], nLbl);
            EMITI(f, "jmp %s", endLbl); /* this term succeeded */
            LBL(f, nLbl);               /* next term starts here */
        }
        EMITI(f, "jmp %s", falseLbl);   /* all terms failed */
        LBL(f, endLbl);
    }
}

/* ----------------------------------------------------------------
 * Statement generators
 * ---------------------------------------------------------------- */

static void generateVarDecl(CodeGenerator* gen, CSTNode* node) {
    FILE* f = gen->file;
    CSTNode* identNode = node->firstChild->nextSibling;
    CSTNode* typeToken = identNode->nextSibling->nextSibling->firstChild;
    TokenTypes dataType = typeToken ? typeToken->token->type : TOKEN_NUMBER;
    VarEntry* ve = addVar(&gen->gs, identNode->token->lexeme, dataType);

    /* Initialise local variables to zero / empty string */
    if (!ve->isGlobal) {
        if (dataType == TOKEN_TEXT || dataType == TOKEN_STRING)
            EMITI(f, "lea rax, [__empty_str]");
        else
            EMITI(f, "xor rax, rax");
        EMITI(f, "mov [rbp - %d], rax", ve->offset);
    }
    /* Global variables are zero-initialised by the .bss / .data section */
}

static void generateAssignment(CodeGenerator* gen, CSTNode* node) {
    FILE* f = gen->file;
    CSTNode* keyword = node->firstChild;
    CSTNode* lvalNode = keyword->nextSibling;
    CSTNode* exprNode = lvalNode->nextSibling->nextSibling;
    TokenTypes op = keyword->token->type;
    CSTNode* identNode = lvalNode->firstChild;
    int isCharStore = (identNode->nextSibling &&
        identNode->nextSibling->token &&
        identNode->nextSibling->token->type == TOKEN_ATPOSITION);

    generateExpr(gen, exprNode); /* result in rax */

    if (op == TOKEN_SET) {
        if (isCharStore) {
            /* Copy the low byte of the rhs string pointer into the target slot */
            EMITI(f, "push rax");
            generateLValAddr(gen, lvalNode);
            EMITI(f, "pop rbx");
            EMITI(f, "movzx rbx, byte [rbx]");
            EMITI(f, "mov byte [rax], bl");
        }
        else {
            EMITI(f, "push rax");
            generateLValAddr(gen, lvalNode);
            EMITI(f, "pop rbx");
            EMITI(f, "mov [rax], rbx");
        }
    }
    else {
        /* Compound assignment: load current value, apply op, store back */
        EMITI(f, "push rax");
        generateLValAddr(gen, lvalNode);
        EMITI(f, "mov rbx, [rax]"); /* current value */
        EMITI(f, "mov rcx, rax");   /* save address */
        EMITI(f, "pop rax");        /* rhs */

        switch (op) {
        case TOKEN_INCREASE: EMITI(f, "add rbx, rax"); break;
        case TOKEN_DECREASE: EMITI(f, "sub rbx, rax"); break;
        case TOKEN_MULTIPLY: EMITI(f, "imul rbx, rax"); break;
        case TOKEN_DIVIDE:
            EMITI(f, "xchg rax, rbx"); /* rax = dividend, rbx = divisor */
            EMITI(f, "cqo");
            EMITI(f, "idiv rbx");
            EMITI(f, "mov rbx, rax");
            break;
        default: break;
        }
        EMITI(f, "mov [rcx], rbx");
    }
}

static void generateShow(CodeGenerator* gen, CSTNode* exprNode) {
    FILE* f = gen->file;
    TokenTypes t = generateExpr(gen, exprNode);
    EMITI(f, "mov rcx, rax");
    EMITI(f, "sub rsp, 32");
    if (t == TOKEN_TEXT || t == TOKEN_STRING)
        EMITI(f, "call __print_str");
    else
        EMITI(f, "call __print_num");
    EMITI(f, "add rsp, 32");
}

static void generateGet(CodeGenerator* gen, CSTNode* node) {
    FILE* f = gen->file;
    CSTNode* typeNode = node->firstChild->nextSibling;
    CSTNode* prompt = typeNode->nextSibling->nextSibling;
    CSTNode* lvalNode = prompt->nextSibling->nextSibling;
    TokenTypes varType = TOKEN_NUMBER;
    VarEntry* ve;

    /* Print the prompt string */
    generateExpr(gen, prompt);
    EMITI(f, "mov rcx, rax");
    EMITI(f, "sub rsp, 32");
    EMITI(f, "call __print_str");
    EMITI(f, "add rsp, 32");

    /* Read input using the appropriate runtime helper */
    ve = findVar(&gen->gs, lvalNode->firstChild->token->lexeme);
    if (ve) varType = ve->dataType;

    EMITI(f, "sub rsp, 32");
    if (varType == TOKEN_TEXT || varType == TOKEN_STRING)
        EMITI(f, "call __read_str");
    else
        EMITI(f, "call __read_num");
    EMITI(f, "add rsp, 32");

    /* Store result into the target variable */
    EMITI(f, "push rax");
    generateLValAddr(gen, lvalNode);
    EMITI(f, "pop rbx");
    EMITI(f, "mov [rax], rbx");
}

static void generateIO(CodeGenerator* gen, CSTNode* node) {
    if (node->firstChild->token->type == TOKEN_SHOW)
        generateShow(gen, node->firstChild->nextSibling);
    else
        generateGet(gen, node);
}

static void generateIfStmt(CodeGenerator* gen, CSTNode* ifNode) {
    FILE* f = gen->file;
    int endId = newLabel(&gen->gs);
    int elsId = newLabel(&gen->gs);
    char endLbl[40], elsLbl[40];
    CSTNode* condNode, * blockNode, * elifNode;
    int elifDone = 0;

    sprintf(endLbl, "__if_end_%d", endId);
    sprintf(elsLbl, "__if_else_%d", elsId);

    condNode = ifNode->firstChild->nextSibling;
    blockNode = condNode;
    while (blockNode && blockNode->symbol != NON_TERMINAL_BLOCK)
        blockNode = blockNode->nextSibling;
    elifNode = blockNode ? blockNode->nextSibling : NULL;

    generateCond(gen, condNode, elsLbl);
    generateBlock(gen, blockNode);
    EMITI(f, "jmp %s", endLbl);
    LBL(f, elsLbl);

    /* Walk the elif/else chain */
    while (elifNode && elifNode->symbol == NON_TERMINAL_ELIF_LIST && !elifDone) {
        CSTNode* ec = elifNode->firstChild;
        if (!ec) {
            elifDone = 1;
        }
        else if (ec->token && ec->token->type == TOKEN_OTHERWISE) {
            CSTNode* next = ec->nextSibling;
            if (next && next->token && next->token->type == TOKEN_OTHERWISE_IF) {
                /* "Otherwise if ..." — another conditional branch */
                CSTNode* ic = next->nextSibling;
                CSTNode* ib = ic;
                int nid = newLabel(&gen->gs);
                char nlbl[40];
                sprintf(nlbl, "__elif_%d", nid);
                while (ib && ib->symbol != NON_TERMINAL_BLOCK)
                    ib = ib->nextSibling;
                generateCond(gen, ic, nlbl);
                generateBlock(gen, ib);
                EMITI(f, "jmp %s", endLbl);
                LBL(f, nlbl);
                elifNode = ib ? ib->nextSibling : NULL;
            }
            else {
                /* "Otherwise:" — plain else */
                CSTNode* eb = next;
                while (eb && eb->symbol != NON_TERMINAL_BLOCK)
                    eb = eb->nextSibling;
                if (eb) generateBlock(gen, eb);
                elifDone = 1;
            }
        }
        else if (ec->symbol == NON_TERMINAL_ELSE_PART) {
            CSTNode* eb = ec->firstChild;
            while (eb && eb->symbol != NON_TERMINAL_BLOCK)
                eb = eb->nextSibling;
            if (eb) generateBlock(gen, eb);
            elifDone = 1;
        }
        else {
            elifDone = 1;
        }
    }

    LBL(f, endLbl);
}

/*
 * Repeat N iterations:
 *   Push N onto the stack; decrement and test at the top of each iteration.
 *   This avoids using a dedicated register for the counter.
 */
static void generateRepeat(CodeGenerator* gen, CSTNode* loopNode) {
    FILE* f = gen->file;
    CSTNode* exprNode = loopNode->firstChild->nextSibling;
    CSTNode* blockNode = exprNode;
    int sid, eid;
    char sLbl[40], eLbl[40];

    while (blockNode && blockNode->symbol != NON_TERMINAL_BLOCK)
        blockNode = blockNode->nextSibling;

    sid = newLabel(&gen->gs);
    eid = newLabel(&gen->gs);
    sprintf(sLbl, "__repeat_s_%d", sid);
    sprintf(eLbl, "__repeat_e_%d", eid);

    generateExpr(gen, exprNode);
    EMITI(f, "push rax");       /* counter lives on the stack */
    LBL(f, sLbl);
    EMITI(f, "mov rax, [rsp]");
    EMITI(f, "test rax, rax");
    EMITI(f, "jle %s", eLbl);
    generateBlock(gen, blockNode);
    EMITI(f, "dec qword [rsp]");
    EMITI(f, "jmp %s", sLbl);
    LBL(f, eLbl);
    EMITI(f, "add rsp, 8");     /* pop counter */
}

static void generateWhile(CodeGenerator* gen, CSTNode* loopNode) {
    FILE* f = gen->file;
    CSTNode* condNode = loopNode->firstChild->nextSibling;
    CSTNode* blockNode = condNode;
    int sid, eid;
    char sLbl[40], eLbl[40];

    while (blockNode && blockNode->symbol != NON_TERMINAL_BLOCK)
        blockNode = blockNode->nextSibling;

    sid = newLabel(&gen->gs);
    eid = newLabel(&gen->gs);
    sprintf(sLbl, "__while_s_%d", sid);
    sprintf(eLbl, "__while_e_%d", eid);

    LBL(f, sLbl);
    generateCond(gen, condNode, eLbl);
    generateBlock(gen, blockNode);
    EMITI(f, "jmp %s", sLbl);
    LBL(f, eLbl);
}

static void generateCtrlFlow(CodeGenerator* gen, CSTNode* node) {
    CSTNode* inner = node->firstChild;
    if (!inner) return;
    if (inner->symbol == NON_TERMINAL_IF_STMT) {
        generateIfStmt(gen, inner);
        return;
    }
    if (inner->symbol == NON_TERMINAL_LOOP_STMT) {
        CSTNode* first = inner->firstChild;
        if (!first || !first->token) return;
        switch (first->token->type) {
        case TOKEN_REPEAT: generateRepeat(gen, inner); break;
        case TOKEN_WHILE:  generateWhile(gen, inner);  break;
        default: break;
        }
    }
}

/* Windows x64 ABI: first 4 integer arguments in rcx, rdx, r8, r9 */
static const char* paramRegs[] = { "rcx", "rdx", "r8", "r9" };

/*
 * Emit an entire function, including prologue, parameter spill, body,
 * and epilogue.  The frame size is computed conservatively so the stack
 * stays 16-byte aligned per the ABI.
 *
 * The outer GenState (locals/offset/inFunction) is saved and restored so
 * nested function declarations work correctly.
 */
static void generateFuncDecl(CodeGenerator* gen, CSTNode* node) {
    FILE* f = gen->file;
    CSTNode* identNode = node->firstChild->nextSibling;
    CSTNode* paramsNode = identNode->nextSibling->nextSibling;
    CSTNode* blockNode = paramsNode;
    const char* funcName;
    VarEntry* outerLocals;
    int outerOffset, outerInFunc;
    int nParams = 0;
    int paramCount = 0;
    char* paramNames[64];
    int frameBytes;

    while (blockNode && blockNode->symbol != NON_TERMINAL_BLOCK)
        blockNode = blockNode->nextSibling;

    funcName = identNode->token->lexeme;
    registerFunc(&gen->gs, funcName);

    /* Save outer function state */
    outerLocals = gen->gs.locals;
    outerOffset = gen->gs.nextOffset;
    outerInFunc = gen->gs.inFunction;
    gen->gs.locals = NULL;
    gen->gs.nextOffset = 0;
    gen->gs.inFunction = 1;

    /* Collect parameter names in order using an iterative stack */
    if (paramsNode->symbol == NON_TERMINAL_PARAM_LIST) {
        CSTNode* stk[64]; int top = 0;
        stk[top++] = paramsNode;
        while (top > 0) {
            CSTNode* curr = stk[--top];
            if (!curr) continue;
            if (curr->token && curr->token->type == TOKEN_IDENT) {
                if (paramCount < 64) paramNames[paramCount++] = curr->token->lexeme;
            }
            else if (curr->symbol == NON_TERMINAL_PARAM_LIST) {
                CSTNode* child = curr->firstChild;
                int base = top;
                int lo, hi;
                while (child && top < 64) {
                    stk[top++] = child;
                    child = child->nextSibling;
                }
                lo = base; hi = top - 1;
                while (lo < hi) {
                    CSTNode* tmp = stk[lo]; stk[lo] = stk[hi]; stk[hi] = tmp;
                    lo++; hi--;
                }
            }
        }
        {
            int i;
            for (i = 0; i < paramCount; i++) {
                addVar(&gen->gs, paramNames[i],
                    lookupParamType(&gen->gs, funcName, i));
                nParams++;
            }
        }
    }

    /*
     * Frame layout (growing downwards from rbp):
     *   [rbp - 8*1 .. rbp - 8*nParams] : parameter home slots
     *   [rbp - frameBytes - 56]        : callee-saved register save area
     *   56 = 7 registers * 8 bytes     (rbx, rsi, rdi, r12-r15)
     */
    frameBytes = (((nParams + 16) * 8 + 47) / 16) * 16 + 24;
    gen->gs.currentFrameBytes = frameBytes;

    EMIT(f, "");
    LBLF(f, "__%s", funcName);
    EMITI(f, "push rbp");
    EMITI(f, "mov rbp, rsp");
    EMITI(f, "sub rsp, %d", frameBytes);
    /* Save callee-saved registers */
    EMITI(f, "push rbx"); EMITI(f, "push rsi"); EMITI(f, "push rdi");
    EMITI(f, "push r12"); EMITI(f, "push r13"); EMITI(f, "push r14");
    EMITI(f, "push r15");

    /* Spill incoming register arguments to their home slots */
    if (paramsNode->symbol == NON_TERMINAL_PARAM_LIST) {
        int i;
        for (i = 0; i < paramCount && i < 4; i++) {
            VarEntry* ve = findVar(&gen->gs, paramNames[i]);
            if (ve && !ve->isGlobal)
                EMITI(f, "mov [rbp - %d], %s", ve->offset, paramRegs[i]);
        }
    }

    /* Emit the function body */
    if (blockNode) {
        CSTNode* s = blockNode->firstChild;
        while (s) {
            generateNode(gen, s);
            s = s->nextSibling;
        }
    }

    /* Epilogue — also used as the target of explicit Return statements */
    {
        int lastIsReturn = 0;
        if (blockNode) {
            CSTNode* s = blockNode->firstChild;
            CSTNode* last = NULL;
            while (s) { last = s; s = s->nextSibling; }
            if (last && last->symbol == NON_TERMINAL_FUNC_RET)
                lastIsReturn = 1;
        }
        if (!lastIsReturn) EMITI(f, "xor rax, rax"); /* implicit return 0 */
        LBLF(f, "__%s_ret", funcName);
        EMITI(f, "lea rsp, [rbp - %d]", frameBytes + 56);
        EMITI(f, "pop r15"); EMITI(f, "pop r14"); EMITI(f, "pop r13");
        EMITI(f, "pop r12"); EMITI(f, "pop rdi"); EMITI(f, "pop rsi");
        EMITI(f, "pop rbx");
        EMITI(f, "add rsp, %d", frameBytes);
        EMITI(f, "pop rbp");
        EMITI(f, "ret");
    }

    /* Restore outer function state */
    freeLocals(&gen->gs);
    gen->gs.locals = outerLocals;
    gen->gs.nextOffset = outerOffset;
    gen->gs.inFunction = outerInFunc;
}

/* Emit an explicit Return: evaluate the expression, then jump to the epilogue */
static void generateFuncRet(CodeGenerator* gen, CSTNode* node) {
    FILE* f = gen->file;
    generateExpr(gen, node->firstChild->nextSibling);
    EMITI(f, "lea rsp, [rbp - %d]", gen->gs.currentFrameBytes + 56);
    EMITI(f, "pop r15"); EMITI(f, "pop r14"); EMITI(f, "pop r13");
    EMITI(f, "pop r12"); EMITI(f, "pop rdi"); EMITI(f, "pop rsi");
    EMITI(f, "pop rbx");
    EMITI(f, "add rsp, %d", gen->gs.currentFrameBytes);
    EMITI(f, "pop rbp");
    EMITI(f, "ret");
}

/*
 * Emit a function call.
 *   1. Evaluate arguments right-to-left and push them all.
 *   2. Pop the first ≤4 into the register argument slots.
 *   3. Flush any call-clobbered registers the allocator holds.
 *   4. Allocate shadow space, call, clean up stack.
 *   5. Restore flushed registers.
 */
static void generateFuncCall(CodeGenerator* gen, CSTNode* node, RegAlloc* ra) {
    FILE* f = gen->file;
    CSTNode* identNode = node->firstChild;
    const char* name = identNode->token->lexeme;
    CSTNode* argList = identNode->nextSibling;
    CSTNode* argExprs[16];
    int nArgs = 0;
    int i, regArgs;
    unsigned spillMask;

    while (argList && argList->symbol != NON_TERMINAL_ARG_LIST)
        argList = argList->nextSibling;

    if (argList) {
        CSTNode* stk[64]; int top = 0;
        stk[top++] = argList;
        while (top > 0 && nArgs < 16) {
            CSTNode* curr = stk[--top];
            if (!curr) continue;
            if (curr->symbol == NON_TERMINAL_ARG_LIST) {
                CSTNode* child = curr->firstChild;
                int base = top;
                int lo, hi;
                while (child && top < 64) {
                    stk[top++] = child;
                    child = child->nextSibling;
                }
                lo = base; hi = top - 1;
                while (lo < hi) {
                    CSTNode* tmp = stk[lo]; stk[lo] = stk[hi]; stk[hi] = tmp;
                    lo++; hi--;
                }
            }
            else if (curr->token && curr->token->type == TOKEN_COMMA) {
                /* skip */
            }
            else if (curr->token && curr->token->type == TOKEN_NAN) {
                /* skip — NAN is a placeholder for an empty argument list */
            }
            else {
                argExprs[nArgs++] = curr;
            }
        }
    }

    /* Push all arguments (right-to-left so arg[0] ends up at top after pops).
     * Use the caller's RegAlloc (ra) when available so that registers already
     * live in the outer expression (e.g. the lhs of a plus across two calls)
     * are seen as in-use and not allocated as scratch by argument expressions.
     * Fall back to a fresh RegAlloc when called from a statement context (ra==NULL). */
    for (i = nArgs - 1; i >= 0; i--) {
        if (ra)
            generateExprRA(gen, argExprs[i], ra, "rax");
        else
            generateExpr(gen, argExprs[i]);
        EMITI(f, "push rax");
    }

    /* Move first ≤4 arguments into register slots */
    regArgs = nArgs < 4 ? nArgs : 4;
    for (i = 0; i < regArgs; i++)
        EMITI(f, "pop %s", paramRegs[i]);

    spillMask = ra ? flushCallClobbered(ra) : 0;
    EMITI(f, "sub rsp, 32");   /* shadow space */
    EMITI(f, "call __%s", name);
    EMITI(f, "add rsp, 32");
    if (nArgs > 4) EMITI(f, "add rsp, %d", (nArgs - 4) * 8); /* clean stack args */
    if (spillMask) unflushCallClobbered(ra, spillMask);
}

/* Wrapper used when a function call appears as a statement (result discarded) */
static void generateFuncCallStmt(CodeGenerator* gen, CSTNode* node) {
    generateFuncCall(gen, node->firstChild, NULL);
}

static void generateBlock(CodeGenerator* gen, CSTNode* node) {
    CSTNode* s;
    if (!node) return;
    s = node->firstChild;
    while (s) {
        generateNode(gen, s);
        s = s->nextSibling;
    }
}

/*
 * No-op handler registered for FUNC_DECL in the main dispatch table.
 * Function declarations are handled entirely in the first pass;
 * the second pass must skip them to avoid double-emission.
 */
static void generateFuncDeclNoop(CodeGenerator* gen, CSTNode* node) {
    (void)gen; (void)node;
}

typedef void (*GenNodeHandler)(CodeGenerator*, CSTNode*);

/* Build the dispatch table once; maps NonTerminal -> generator function */
static void buildGenNodeDispatch(CodeGenerator* gen) {
    if (gen->genNodeDispatch) return;
    gen->genNodeDispatch = initDispatchTable(20);
    dispatchSet(gen->genNodeDispatch, NON_TERMINAL_BLOCK,
        (DispatchFn)generateBlock);
    dispatchSet(gen->genNodeDispatch, NON_TERMINAL_VAR_DECL,
        (DispatchFn)generateVarDecl);
    dispatchSet(gen->genNodeDispatch, NON_TERMINAL_FUNC_DECL,
        (DispatchFn)generateFuncDeclNoop);
    dispatchSet(gen->genNodeDispatch, NON_TERMINAL_ASSIGN_STMT,
        (DispatchFn)generateAssignment);
    dispatchSet(gen->genNodeDispatch, NON_TERMINAL_IO_STMT,
        (DispatchFn)generateIO);
    dispatchSet(gen->genNodeDispatch, NON_TERMINAL_CTRL_FLOW,
        (DispatchFn)generateCtrlFlow);
    dispatchSet(gen->genNodeDispatch, NON_TERMINAL_FUNC_CALL_STMT,
        (DispatchFn)generateFuncCallStmt);
    dispatchSet(gen->genNodeDispatch, NON_TERMINAL_FUNC_RET,
        (DispatchFn)generateFuncRet);
}

static void generateNode(CodeGenerator* gen, CSTNode* node) {
    GenNodeHandler handler;
    CSTNode* c;

    if (!node) return;
    buildGenNodeDispatch(gen);
    handler = (GenNodeHandler)dispatchGet(gen->genNodeDispatch, node->symbol);
    if (handler) {
        handler(gen, node);
    }
    else {
        /* No handler registered — recurse into children */
        c = node->firstChild;
        while (c) {
            generateNode(gen, c);
            c = c->nextSibling;
        }
    }
}

/* ----------------------------------------------------------------
 * Runtime helper library
 * These are small assembly routines emitted at the top of every output
 * file.  They implement the standard-library functions the language needs:
 * string/number I/O, bounds checking, and heap-based string duplication.
 * ---------------------------------------------------------------- */
static void emitHelpers(FILE* f) {
    EMIT(f, "; ================================================================");
    EMIT(f, "; Runtime helpers");
    EMIT(f, "; ================================================================");
    EMIT(f, "");

    LBL(f, "__strlen");
    EMITI(f, "xor rax, rax");
    LBLF(f, "__strlen_loop");
    EMITI(f, "cmp byte [rcx + rax], 0");
    EMITI(f, "je  __strlen_done");
    EMITI(f, "inc rax");
    EMITI(f, "jmp __strlen_loop");
    LBL(f, "__strlen_done");
    EMITI(f, "ret");
    EMIT(f, "");

    LBL(f, "__print_str");
    EMITI(f, "push rbp"); EMITI(f, "mov rbp, rsp"); EMITI(f, "sub rsp, 48");
    EMITI(f, "mov [rbp - 8],  rcx");
    EMITI(f, "mov rcx, %s", STD_OUTPUT_HANDLE);
    EMITI(f, "call GetStdHandle");
    EMITI(f, "mov [rbp - 16], rax");
    EMITI(f, "mov rcx, [rbp - 8]");
    EMITI(f, "call __strlen");
    EMITI(f, "mov rcx, [rbp - 16]"); EMITI(f, "mov rdx, [rbp - 8]");
    EMITI(f, "mov r8,  rax");        EMITI(f, "lea r9,  [rbp - 24]");
    EMITI(f, "mov qword [rsp + 32], 0");
    EMITI(f, "call WriteConsoleA");
    EMITI(f, "mov rsp, rbp"); EMITI(f, "pop rbp"); EMITI(f, "ret");
    EMIT(f, "");

    LBL(f, "__print_num");
    EMITI(f, "push rbp"); EMITI(f, "mov rbp, rsp"); EMITI(f, "sub rsp, 64");
    EMITI(f, "mov [rbp - 8],  rbx"); EMITI(f, "mov [rbp - 16], rdi");
    EMITI(f, "mov rax, rcx");        EMITI(f, "test rax, rax");
    EMITI(f, "jge __pnum_positive");
    EMITI(f, "neg rax");
    EMITI(f, "mov byte [rbp - 32], '-'"); EMITI(f, "mov byte [rbp - 31], 0");
    EMITI(f, "jmp __pnum_convert");
    LBLF(f, "__pnum_positive");
    EMITI(f, "mov byte [rbp - 32], 0");
    LBLF(f, "__pnum_convert");
    EMITI(f, "lea rdi, [rbp - 56]");
    EMITI(f, "mov byte [rdi + 20], 0"); EMITI(f, "mov rcx, 20");
    EMITI(f, "mov rbx, 10");
    LBLF(f, "__pnum_loop");
    EMITI(f, "xor rdx, rdx"); EMITI(f, "div rbx");
    EMITI(f, "add dl, '0'");  EMITI(f, "dec rcx");
    EMITI(f, "mov [rdi + rcx], dl");
    EMITI(f, "test rax, rax"); EMITI(f, "jnz __pnum_loop");
    EMITI(f, "mov [rbp - 24], rcx");
    EMITI(f, "cmp byte [rbp - 32], '-'"); EMITI(f, "jne __pnum_noSign");
    EMITI(f, "lea rcx, [rbp - 32]");      EMITI(f, "sub rsp, 32");
    EMITI(f, "call __print_str");         EMITI(f, "add rsp, 32");
    LBLF(f, "__pnum_noSign");
    EMITI(f, "mov rcx, [rbp - 24]");
    EMITI(f, "lea rcx, [rdi + rcx]");
    EMITI(f, "sub rsp, 32"); EMITI(f, "call __print_str"); EMITI(f, "add rsp, 32");
    EMITI(f, "mov rbx, [rbp - 8]"); EMITI(f, "mov rdi, [rbp - 16]");
    EMITI(f, "mov rsp, rbp");        EMITI(f, "pop rbp"); EMITI(f, "ret");
    EMIT(f, "");

    LBL(f, "__read_str");
    EMITI(f, "push rbp"); EMITI(f, "mov rbp, rsp"); EMITI(f, "sub rsp, 48");
    EMITI(f, "mov rcx, %s", STD_INPUT_HANDLE);
    EMITI(f, "call GetStdHandle");       EMITI(f, "mov [rbp - 8], rax");
    EMITI(f, "mov rcx, [rbp - 8]");     EMITI(f, "lea rdx, [__input_buf]");
    EMITI(f, "mov r8,  255");            EMITI(f, "lea r9,  [rbp - 16]");
    EMITI(f, "mov qword [rsp + 32], 0"); EMITI(f, "call ReadConsoleA");
    EMITI(f, "mov rax, [rbp - 16]");    EMITI(f, "lea rcx, [__input_buf]");
    EMITI(f, "test rax, rax");           EMITI(f, "jz  __read_str_done");
    EMITI(f, "dec rax");
    EMITI(f, "cmp byte [rcx + rax], 10"); EMITI(f, "jne __read_str_chk_cr");
    EMITI(f, "mov byte [rcx + rax], 0");
    EMITI(f, "test rax, rax");            EMITI(f, "jz  __read_str_done");
    EMITI(f, "dec rax");
    LBLF(f, "__read_str_chk_cr");
    EMITI(f, "cmp byte [rcx + rax], 13"); EMITI(f, "jne __read_str_done");
    EMITI(f, "mov byte [rcx + rax], 0");
    LBL(f, "__read_str_done");
    EMITI(f, "lea rcx, [__input_buf]"); EMITI(f, "call __strdup");
    EMITI(f, "mov rsp, rbp");            EMITI(f, "pop rbp"); EMITI(f, "ret");
    EMIT(f, "");

    LBL(f, "__rnum_invalid");
    EMITI(f, "sub rsp, 40");
    EMITI(f, "lea rcx, [__rnum_invalid_msg]");
    EMITI(f, "call __print_str"); EMITI(f, "mov rcx, 1");
    EMITI(f, "call ExitProcess");
    EMIT(f, "");

    LBL(f, "__read_num");
    EMITI(f, "push rbp"); EMITI(f, "mov rbp, rsp"); EMITI(f, "sub rsp, 64");
    EMITI(f, "mov [rbp - 8],  rbx"); EMITI(f, "mov [rbp - 16], rsi");
    EMITI(f, "call __read_str");      EMITI(f, "mov rsi, rax");
    EMITI(f, "xor rbx, rbx");
    EMITI(f, "cmp byte [rsi], '-'"); EMITI(f, "jne __rnum_digits");
    EMITI(f, "mov rbx, 1");          EMITI(f, "inc rsi");
    EMITI(f, "cmp byte [rsi], 0");   EMITI(f, "je  __rnum_call_invalid");
    LBLF(f, "__rnum_digits");
    EMITI(f, "cmp byte [rsi], 0"); EMITI(f, "je  __rnum_call_invalid");
    EMITI(f, "xor rax, rax");      EMITI(f, "mov rcx, 10");
    LBLF(f, "__rnum_loop");
    EMITI(f, "movzx rdx, byte [rsi]"); EMITI(f, "test rdx, rdx");
    EMITI(f, "jz __rnum_done");        EMITI(f, "sub rdx, '0'");
    EMITI(f, "cmp rdx, 9");            EMITI(f, "ja  __rnum_call_invalid");
    EMITI(f, "imul rax, rcx");         EMITI(f, "add rax, rdx");
    EMITI(f, "inc rsi");               EMITI(f, "jmp __rnum_loop");
    LBLF(f, "__rnum_done");
    EMITI(f, "test rbx, rbx"); EMITI(f, "jz  __rnum_ret"); EMITI(f, "neg rax");
    LBLF(f, "__rnum_ret");
    EMITI(f, "mov rbx, [rbp - 8]"); EMITI(f, "mov rsi, [rbp - 16]");
    EMITI(f, "mov rsp, rbp");        EMITI(f, "pop rbp"); EMITI(f, "ret");
    LBLF(f, "__rnum_call_invalid");
    EMITI(f, "mov rsp, rbp"); EMITI(f, "call __rnum_invalid");
    EMIT(f, "");

    LBL(f, "__print_char");
    EMITI(f, "push rbp"); EMITI(f, "mov rbp, rsp"); EMITI(f, "sub rsp, 64");
    EMITI(f, "mov [rbp - 8], rcx");
    EMITI(f, "mov rcx, %s", STD_OUTPUT_HANDLE);
    EMITI(f, "call GetStdHandle"); EMITI(f, "mov rcx, rax");
    EMITI(f, "lea rdx, [rbp - 8]"); EMITI(f, "mov r8,  1");
    EMITI(f, "lea r9,  [rbp - 16]"); EMITI(f, "mov qword [rsp + 32], 0");
    EMITI(f, "call WriteConsoleA");
    EMITI(f, "mov rsp, rbp"); EMITI(f, "pop rbp"); EMITI(f, "ret");
    EMIT(f, "");

    LBL(f, "__bounds_check");
    EMITI(f, "push rbp"); EMITI(f, "mov rbp, rsp"); EMITI(f, "sub rsp, 48");
    EMITI(f, "mov [rbp - 8],  rcx"); EMITI(f, "mov [rbp - 16], rdx");
    EMITI(f, "mov rcx, rdx");         EMITI(f, "call __strlen");
    EMITI(f, "mov rcx, [rbp - 8]");   EMITI(f, "cmp rcx, rax");
    EMITI(f, "jl  __bounds_ok");
    EMITI(f, "lea rcx, [__bounds_msg]");
    EMITI(f, "sub rsp, 32"); EMITI(f, "call __print_str"); EMITI(f, "add rsp, 32");
    EMITI(f, "mov rcx, 1");   EMITI(f, "call ExitProcess");
    LBLF(f, "__bounds_ok");
    EMITI(f, "mov rsp, rbp"); EMITI(f, "pop rbp"); EMITI(f, "ret");
    EMIT(f, "");

    LBL(f, "__strdup");
    EMITI(f, "push rbp"); EMITI(f, "mov rbp, rsp"); EMITI(f, "sub rsp, 64");
    EMITI(f, "mov [rbp - 8], rcx");
    EMITI(f, "call __strlen"); EMITI(f, "inc rax"); EMITI(f, "mov [rbp - 16], rax");
    EMITI(f, "call GetProcessHeap");
    EMITI(f, "mov rcx, rax");  EMITI(f, "xor rdx, rdx");
    EMITI(f, "mov r8, [rbp - 16]"); EMITI(f, "call HeapAlloc");
    EMITI(f, "test rax, rax"); EMITI(f, "jz __strdup_fail");
    EMITI(f, "mov [rbp - 24], rax");
    EMITI(f, "mov rcx, rax");  EMITI(f, "mov rdx, [rbp - 8]");
    LBLF(f, "__strdup_copy");
    EMITI(f, "mov al, [rdx]"); EMITI(f, "mov [rcx], al");
    EMITI(f, "inc rcx");       EMITI(f, "inc rdx");
    EMITI(f, "test al, al");   EMITI(f, "jnz __strdup_copy");
    EMITI(f, "mov rax, [rbp - 24]");
    LBLF(f, "__strdup_fail");
    EMITI(f, "mov rsp, rbp"); EMITI(f, "pop rbp"); EMITI(f, "ret");
    EMIT(f, "");
}

/* ----------------------------------------------------------------
 * Data and BSS section emitters
 * ---------------------------------------------------------------- */

 /*
  * Emit string literals as NASM byte arrays, with \n escape sequences
  * converted to the literal value 10.  Text global variables get a
  * pointer initialised to __empty_str; numeric globals go in .bss.
  */
static void emitDataSection(CodeGenerator* gen) {
    FILE* f = gen->file;
    StrEntry* se;
    VarEntry* ve;

    EMIT(f, ""); EMIT(f, "section .data");
    EMITI(f, "__empty_str db 0");
    EMITI(f, "__bounds_msg db '[Runtime Error] atPosition index out of bounds (1-based).', 10, 0");
    EMITI(f, "__rnum_invalid_msg db '[Runtime Error] Expected a number but received invalid input.', 10, 0");

    for (se = gen->gs.strings; se; se = se->next) {
        const char* p = se->value;
        int needComma = 0;
        fprintf(f, "\t__str_%d db ", se->id);
        if (!*p) {
            fprintf(f, "0\n");
            continue;
        }
        while (*p) {
            if (needComma) fprintf(f, ", ");
            needComma = 1;
            if (*p == '\\' && *(p + 1) == 'n') {
                fprintf(f, "10");
                p += 2;
            }
            else {
                fprintf(f, "%d", *p);
                p++;
            }
        }
        fprintf(f, ", 0\n");
    }

    for (ve = gen->gs.globals; ve; ve = ve->next) {
        if (ve->dataType == TOKEN_TEXT || ve->dataType == TOKEN_STRING)
            EMITI(f, "__g_%s dq __empty_str", ve->name);
    }

    EMIT(f, "");
}

static void emitBSSSection(CodeGenerator* gen) {
    FILE* f = gen->file;
    VarEntry* ve;

    EMIT(f, "section .bss");
    EMITI(f, "__input_buf  resb 256"); /* shared buffer for all Get statements */
    EMITI(f, "__char_buf   resb 2");   /* single-character atPosition result    */

    for (ve = gen->gs.globals; ve; ve = ve->next) {
        if (ve->dataType != TOKEN_TEXT && ve->dataType != TOKEN_STRING)
            EMITI(f, "__g_%s resq 1", ve->name);
    }

    EMIT(f, "");
}

/* ----------------------------------------------------------------
 * Parameter-type pre-pass
 *
 * Walks the entire CST looking for FUNC_CALL nodes.  For each call site
 * it records the type of each argument so that when generateFuncDecl()
 * runs it can allocate frame slots with the correct size.
 * ---------------------------------------------------------------- */

static TokenTypes inferArgType(GenState* gs, CSTNode* node) {
    if (!node) return TOKEN_NUMBER;

    /* Unwrap single-child wrapper nodes to reach the actual value */
    if ((node->symbol == NON_TERMINAL_E || node->symbol == NON_TERMINAL_T ||
        node->symbol == NON_TERMINAL_F) &&
        node->firstChild && node->firstChild->nextSibling == NULL)
        return inferArgType(gs, node->firstChild);

    /* Multi-child arithmetic expression — must be numeric */
    if (node->symbol == NON_TERMINAL_E || node->symbol == NON_TERMINAL_T)
        return TOKEN_NUMBER;

    if (node->symbol == NON_TERMINAL_LVAL && node->firstChild &&
        node->firstChild->token) {
        VarEntry* ve = findVar(gs, node->firstChild->token->lexeme);
        return ve ? ve->dataType : TOKEN_NUMBER;
    }
    if (node->token && node->token->type == TOKEN_STRING) return TOKEN_TEXT;
    return TOKEN_NUMBER;
}

static void paramTypePrePass(GenState* gs, CSTNode* node) {
    CSTNode* c;
    if (!node) return;

    if (node->symbol == NON_TERMINAL_FUNC_CALL) {
        CSTNode* identNode = node->firstChild;
        if (identNode && identNode->token) {
            const char* funcName = identNode->token->lexeme;
            CSTNode* argList = identNode->nextSibling;
            while (argList && argList->symbol != NON_TERMINAL_ARG_LIST)
                argList = argList->nextSibling;

            if (argList) {
                CSTNode* stk[64];      int top = 0;
                CSTNode* argExprs[16]; int nArgs = 0;
                int i;

                stk[top++] = argList;
                while (top > 0 && nArgs < 16) {
                    CSTNode* curr = stk[--top];
                    if (!curr) continue;
                    if (curr->symbol == NON_TERMINAL_ARG_LIST) {
                        CSTNode* child = curr->firstChild;
                        int base = top;
                        int lo, hi;
                        while (child && top < 64) {
                            stk[top++] = child;
                            child = child->nextSibling;
                        }
                        lo = base; hi = top - 1;
                        while (lo < hi) {
                            CSTNode* tmp = stk[lo]; stk[lo] = stk[hi]; stk[hi] = tmp;
                            lo++; hi--;
                        }
                    }
                    else if (curr->token && curr->token->type == TOKEN_COMMA) {
                        /* skip */
                    }
                    else if (curr->token && curr->token->type == TOKEN_NAN) {
                        /* skip */
                    }
                    else {
                        argExprs[nArgs++] = curr;
                    }
                }
                for (i = 0; i < nArgs; i++)
                    recordParamType(gs, funcName, i,
                        inferArgType(gs, argExprs[i]));
            }
        }
    }

    c = node->firstChild;
    while (c) {
        paramTypePrePass(gs, c);
        c = c->nextSibling;
    }
}

/* ----------------------------------------------------------------
 * Two-pass top-level generation
 * ---------------------------------------------------------------- */

 /* Pass 1: recurse until a FUNC_DECL is found, then emit it */
static void firstPassFunctions(CodeGenerator* gen, CSTNode* node) {
    CSTNode* c;
    if (!node) return;
    if (node->symbol == NON_TERMINAL_FUNC_DECL) {
        generateFuncDecl(gen, node);
        return;
    }
    c = node->firstChild;
    while (c) {
        firstPassFunctions(gen, c);
        c = c->nextSibling;
    }
}

/* Pass 2: emit every top-level node that is NOT a function declaration */
static void secondPassMain(CodeGenerator* gen, CSTNode* root) {
    CSTNode* c = root->firstChild;
    while (c) {
        if (c->symbol != NON_TERMINAL_FUNC_DECL)
            generateNode(gen, c);
        c = c->nextSibling;
    }
}

static void generateCodeInternal(CodeGenerator* gen) {
    FILE* f = gen->file;

    /* NASM file header */
    EMIT(f, "default rel"); EMIT(f, "bits 64"); EMIT(f, "");
    EMIT(f, "section .text");
    EMITI(f, "global Start");
    EMITI(f, "extern GetStdHandle");   EMITI(f, "extern WriteConsoleA");
    EMITI(f, "extern ReadConsoleA");   EMITI(f, "extern ExitProcess");
    EMITI(f, "extern GetProcessHeap"); EMITI(f, "extern HeapAlloc");
    EMIT(f, "");

    emitHelpers(f);

    /* Pre-pass: infer parameter types from all call sites */
    paramTypePrePass(&gen->gs, gen->root);

    /* Pass 1: all user-defined functions */
    firstPassFunctions(gen, gen->root);

    /* Pass 2: top-level statements become the "Start" entry point */
    EMIT(f, ""); LBL(f, "Start");
    EMITI(f, "push rbp"); EMITI(f, "mov rbp, rsp");
    EMITI(f, "sub rsp, 120");
    EMITI(f, "push rbx"); EMITI(f, "push rsi"); EMITI(f, "push rdi");
    EMITI(f, "push r12"); EMITI(f, "push r13"); EMITI(f, "push r14");
    EMITI(f, "push r15");

    secondPassMain(gen, gen->root);

    LBL(f, "Exit");
    EMITI(f, "xor rcx, rcx"); /* exit code 0 */
    EMITI(f, "pop r15"); EMITI(f, "pop r14"); EMITI(f, "pop r13");
    EMITI(f, "pop r12"); EMITI(f, "pop rdi"); EMITI(f, "pop rsi");
    EMITI(f, "pop rbx");
    EMITI(f, "add rsp, 120"); EMITI(f, "pop rbp");
    EMITI(f, "call ExitProcess");

    emitDataSection(gen);
    emitBSSSection(gen);
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

 /* Strip the path prefix and the file extension to get the bare name */
static void extractBaseName(CodeGenerator* gen, const char* path) {
    const char* base = path;
    const char* s;
    int extLen, nameLen;

    if ((s = strrchr(path, '\\'))) base = s + 1;
    extLen = (int)strlen(EXTENSION);
    nameLen = (int)strlen(base) - extLen;

    gen->fileName = _strdup(base);
    gen->fileName[nameLen] = '\0';
}

/*
 * Walk three directory levels up from the executable to find the project
 * root (compiler install directory).  The expected layout is:
 *   <project_root>/x64/Debug/<compiler>.exe
 */
static void getProjectRoot(char* rootOut, size_t rootSize) {
    char exePath[512];
    char* p;
    int i;

    if (!GetModuleFileNameA(NULL, exePath, sizeof(exePath)))
        reportError(ERROR_INTERNAL, 0, "Failed to get executable path");

    for (i = 0; i < 3; i++) {
        p = strrchr(exePath, '\\');
        if (!p) p = strrchr(exePath, '/');
        if (p) *p = '\0';
    }

    strncpy(rootOut, exePath, rootSize - 1);
    rootOut[rootSize - 1] = '\0';
}

/*
 * Create Build/<name>/ in the working directory and set outPath to the
 * .asm file that will be written there.
 */
static void buildOutputPath(CodeGenerator* gen, const char* srcPath,
    char* outPath) {
    char cwd[512];
    char* tmp;

    extractBaseName(gen, srcPath);

    if (!_getcwd(cwd, sizeof(cwd)))
        reportError(ERROR_INTERNAL, 0, "Failed to get current working directory");

    {
        size_t allocSize = strlen(cwd) + strlen(gen->fileName) * 2 + 64;
        tmp = (char*)malloc(allocSize);
        if (!tmp) reportError(ERROR_INTERNAL, 0, "OOM in buildOutputPath");

        sprintf(tmp, "mkdir \"%s\\Build\" >nul 2>nul", cwd);
        system(tmp);
        sprintf(tmp, "mkdir \"%s\\Build\\%s\" >nul 2>nul", cwd, gen->fileName);
        system(tmp);

        sprintf(outPath, "%s\\Build\\%s\\%s.asm", cwd, gen->fileName, gen->fileName);

        gen->buildRoot = _strdup(cwd);

        free(tmp);
    }
}

CodeGenerator* initCodeGenerator(const char* targetPath) {
    char projRootBuf[512];
    char outPath[512];
    CodeGenerator* gen = (CodeGenerator*)malloc(sizeof(CodeGenerator));

    if (!gen)
        reportError(ERROR_INTERNAL, 0, "Failed to allocate CodeGenerator");

    gen->fileName = NULL;
    gen->projRoot = NULL;
    gen->buildRoot = NULL;
    gen->outPath = NULL;
    gen->root = NULL;
    gen->relOpDispatch = NULL;
    gen->genNodeDispatch = NULL;

    getProjectRoot(projRootBuf, sizeof(projRootBuf));
    gen->projRoot = _strdup(projRootBuf);

    buildOutputPath(gen, targetPath, outPath);
    gen->outPath = _strdup(outPath);
    gen->file = fopen(outPath, "w");
    if (!gen->file)
        reportError(ERROR_INTERNAL, 0, "Cannot open output file: %s", outPath);

    return gen;
}

void generateAndLinkCode(CodeGenerator* gen, CSTNode* root) {
    VarEntry* e, * n;
    StrEntry* se;
    FuncEntry* fe;
    ParamTypeEntry* pe;

    memset(&gen->gs, 0, sizeof(gen->gs));
    gen->root = root;
    generateCodeInternal(gen);

    fclose(gen->file);
    gen->file = NULL;

    /* Free all GenState entries accumulated during generation */
    for (e = gen->gs.globals; e; e = n) { n = e->next; free(e->name); free(e); }
    for (e = gen->gs.locals; e; e = n) { n = e->next; free(e->name); free(e); }

    se = gen->gs.strings;
    while (se) { StrEntry* sn = se->next; free(se->value); free(se); se = sn; }

    fe = gen->gs.funcs;
    while (fe) { FuncEntry* fn = fe->next; free(fe->name); free(fe); fe = fn; }

    pe = gen->gs.paramTypes;
    while (pe) { ParamTypeEntry* pn = pe->next; free(pe->funcName); free(pe); pe = pn; }

    /* Invoke the linker script to assemble and link the output */
    {
        size_t cmdSize = strlen(gen->projRoot) + strlen(gen->buildRoot) +
            strlen(gen->fileName) * 2 + 64;
        char* cmd = (char*)malloc(cmdSize);
        if (!cmd) reportError(ERROR_INTERNAL, 0, "OOM in linker");
        assert(cmd);
        sprintf(cmd, "%s\\Scripts\\Link.bat \"%s\\Build\\%s\" \"%s\"",
            gen->projRoot, gen->buildRoot, gen->fileName, gen->fileName);
        printf("\n--- Linking Assembly ---\n");
        system(cmd);
        free(cmd);
    }
}

void freeCodeGenerator(CodeGenerator* gen) {
    if (!gen) return;

    if (gen->file)
        fclose(gen->file);

    if (errorCount > 0 && gen->outPath)
        remove(gen->outPath);

    if (gen->fileName)
        free(gen->fileName);

    if (gen->projRoot)
        free(gen->projRoot);

    if (gen->buildRoot)
        free(gen->buildRoot);

    if (gen->outPath)
        free(gen->outPath);

    if (gen->relOpDispatch)
        freeDispatchTable(gen->relOpDispatch);

    if (gen->genNodeDispatch)
        freeDispatchTable(gen->genNodeDispatch);


    free(gen);
}