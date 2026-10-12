#include <stddef.h>
#include <time.h>

/* --- Heap allocator --- */

#define HEAP_SIZE (8 * 1024 * 1024)

static unsigned char heap[HEAP_SIZE];
static size_t heap_used = 0;

void* malloc(size_t size)
{
    /* Round up so every block stays size_t-aligned */
    size = (size + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
    if (heap_used + sizeof(size_t) + size > HEAP_SIZE) return NULL;

    /* Each block starts with its size, so realloc knows how much to copy */
    size_t* block = (size_t*)(heap + heap_used);
    *block = size;
    heap_used += sizeof(size_t) + size;

    return block + 1;
}

void free(void* ptr)
{
    /* A bump allocator never reclaims memory */
    (void)ptr;
}

void* memcpy(void* dest, const void* src, size_t n)
{
    unsigned char* d = dest;
    const unsigned char* s = src;

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];

    return dest;
}

void* memset(void* s, int c, size_t n)
{
    unsigned char* p = s;

    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)c;

    return s;
}

void* calloc(size_t count, size_t size)
{
    void* ptr = malloc(count * size);
    if (ptr == NULL) return NULL;

    return memset(ptr, 0, count * size);
}

void* realloc(void* ptr, size_t size)
{
    void* new_ptr = malloc(size);
    if (ptr == NULL || new_ptr == NULL) return new_ptr;

    /* The old block's size sits right in front of it */
    size_t old_size = ((size_t*)ptr)[-1];
    return memcpy(new_ptr, ptr, old_size < size ? old_size : size);
}

/* --- String functions --- */

size_t strlen(const char* s)
{
    size_t len = 0;

    while (s[len] != '\0')
        len++;

    return len;
}

int strcmp(const char* s1, const char* s2)
{
    while (*s1 != '\0' && *s1 == *s2)
    {
        s1++;
        s2++;
    }

    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char* strcpy(char* dest, const char* src)
{
    char* d = dest;

    while ((*d++ = *src++) != '\0')
        ;

    return dest;
}

char* strcat(char* dest, const char* src)
{
    strcpy(dest + strlen(dest), src);
    return dest;
}

char* strchr(const char* s, int c)
{
    for (; *s != '\0'; s++)
        if (*s == (char)c)
            return (char*)s;

    return NULL;
}

char* strrchr(const char* s, int c)
{
    const char* last = NULL;

    for (; *s != '\0'; s++)
        if (*s == (char)c)
            last = s;

    return (char*)last;
}

int toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');

    return c;
}

/* --- Time functions --- */

/* No real-time clock driver: the clock always reads 2024-01-01 12:00:00 */

time_t time(time_t* out)
{
    if (out != NULL) *out = 0;
    return 0;
}

struct tm* localtime(const time_t* seconds)
{
    (void)seconds;

    /* struct tm counts years from 1900 and months from 0 */
    static struct tm now = { .tm_year = 2024 - 1900, .tm_mon = 0, .tm_mday = 1,
                             .tm_hour = 12 };
    return &now;
}
