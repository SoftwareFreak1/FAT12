#include <stdio.h>
#include <stdarg.h>
#include "debug.h"

void debug_print(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printf("\033[36m");
    vprintf(format, args);
    printf("\033[0m");
    va_end(args);
}
