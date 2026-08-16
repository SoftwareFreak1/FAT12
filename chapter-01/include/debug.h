#ifndef DEBUG_H
#define DEBUG_H

#if DEBUG
#include <stdio.h>
#include <stdarg.h>
#define DBG_PRINT(...) \
    do { \
        printf("\033[36m"); \
        printf(__VA_ARGS__); \
        printf("\033[0m"); \
    } while (0)
#else
#define DBG_PRINT(...)
#endif

#endif
