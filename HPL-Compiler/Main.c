#define _CRT_SECURE_NO_WARNINGS
#include "ErrorHandler.h"
#include "Compiler.h"
#include "Global.h"
#include <string.h>

int main(int argc, char* argv[]) {
    const char* inputPath;
    size_t inputLen;
    size_t extensionLen;
    Compiler* compiler;

    if (argc != 2)
        reportError(ERROR_GLOBAL, 0,
                    "Expected exactly 1 argument (path to %s file).", EXTENSION);

    inputPath    = argv[1];
    inputLen     = strlen(inputPath);
    extensionLen = strlen(EXTENSION);

    /* Validate that the path ends with the expected file extension */
    if (inputLen <= extensionLen ||
        strcmp(inputPath + inputLen - extensionLen, EXTENSION))
        reportError(ERROR_GLOBAL, 0,
                    "Invalid file name: expected a non-empty name ending "
                    "with '%s'. Got '%s'.", EXTENSION, inputPath);

    compiler = initCompiler(inputPath);
    startCompiler(compiler);
    freeCompiler(compiler);

    return 0;
}
