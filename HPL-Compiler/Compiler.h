#pragma once
#include "Lexer.h"
#include "Parser.h"
#include "SemanticAnalyzer.h"
#include "CodeGenerator.h"

/*
 * Compiler — the top-level object that owns and coordinates every stage
 * of the compilation pipeline:
 *
 *   Lexer  ->  Parser  ->  SemanticAnalyzer  ->  CodeGenerator
 *
 * initCompiler() allocates and wires all four components together.
 * startCompiler() runs the full pipeline on the source file.
 * freeCompiler() tears everything down in one call.
 *
 * A pointer to the active Compiler is kept in ErrorHandler so that
 * reportError() can free it before exiting on a fatal error.
 */
typedef struct Compiler {
    Lexer* lexer;
    Parser* parser;
    SemanticAnalyzer* analyzer;
    CodeGenerator* generator;
} Compiler;

Compiler* initCompiler(const char* filePath);
void startCompiler(Compiler* compiler);
void freeCompiler(Compiler* compiler);
