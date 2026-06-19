#ifndef DEBUG_H
#define DEBUG_H

#define DEBUG 0

#if DEBUG
    void debug_print(const char* format, ...);
    #define DBG_PRINT(...) debug_print(__VA_ARGS__)
#else
    #define DBG_PRINT(...)
#endif

#endif
