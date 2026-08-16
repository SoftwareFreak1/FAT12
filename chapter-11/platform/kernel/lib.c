#include <stddef.h>
#include <stdint.h>

#define HEAP_SIZE (1024 * 1024)

static unsigned char g_heap[HEAP_SIZE];

typedef struct block_header {
    size_t size;
    int free;
    struct block_header* next;
} block_header_t;

static block_header_t* free_list = NULL;

static void heap_init(void)
{
    if (free_list != NULL) return;
    free_list = (block_header_t*)g_heap;
    free_list->size = HEAP_SIZE - sizeof(block_header_t);
    free_list->free = 1;
    free_list->next = NULL;
}

void* malloc(size_t size)
{
    if (size == 0) return NULL;
    heap_init();

    if (size < 16) size = 16;

    block_header_t* current = free_list;

    while (current != NULL)
    {
        if (current->free && current->size >= size)
        {
            if (current->size > size + sizeof(block_header_t) + 16)
            {
                block_header_t* new_block =
                    (block_header_t*)((unsigned char*)current +
                        sizeof(block_header_t) + size);
                new_block->size =
                    current->size - size - sizeof(block_header_t);
                new_block->free = 1;
                new_block->next = current->next;

                current->size = size;
                current->next = new_block;
            }

            current->free = 0;
            return (unsigned char*)current + sizeof(block_header_t);
        }

        current = current->next;
    }

    return NULL;
}

void free(void* ptr)
{
    if (ptr == NULL) return;

    block_header_t* block =
        (block_header_t*)((unsigned char*)ptr - sizeof(block_header_t));
    block->free = 1;

    block_header_t* current = free_list;
    while (current != NULL && current->next != NULL)
    {
        if (current->free && current->next->free)
        {
            current->size += sizeof(block_header_t) + current->next->size;
            current->next = current->next->next;
        }
        current = current->next;
    }
}

void* calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void* ptr = malloc(total);
    if (ptr == NULL) return NULL;

    unsigned char* p = (unsigned char*)ptr;
    for (size_t i = 0; i < total; i++)
        p[i] = 0;

    return ptr;
}

void* realloc(void* ptr, size_t size)
{
    if (ptr == NULL) return malloc(size);
    if (size == 0)
    {
        free(ptr);
        return NULL;
    }

    block_header_t* block =
        (block_header_t*)((unsigned char*)ptr - sizeof(block_header_t));
    size_t old_size = block->size;

    void* new_ptr = malloc(size);
    if (new_ptr == NULL) return NULL;

    size_t copy_size = old_size < size ? old_size : size;
    unsigned char* src = (unsigned char*)ptr;
    unsigned char* dst = (unsigned char*)new_ptr;

    for (size_t i = 0; i < copy_size; i++)
        dst[i] = src[i];

    free(ptr);
    return new_ptr;
}

void* memcpy(void* dest, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];

    return dest; /* byte-by-byte — correct for any alignment, slow */
}

void* memset(void* s, int c, size_t n)
{
    unsigned char* p = (unsigned char*)s;

    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)c;

    return s;
}

size_t strlen(const char* s)
{
    size_t len = 0;

    while (s[len] != '\0')
        len++;

    return len;
}

int strcmp(const char* s1, const char* s2)
{
    while (*s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
    }

    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n)
{
    while (n > 0 && *s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
        n--;
    }

    if (n == 0)
        return 0;

    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char* strncpy(char* dest, const char* src, size_t n)
{
    size_t i;

    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];

    for (; i < n; i++)
        dest[i] = '\0';

    return dest;
}

char* strchr(const char* s, int c)
{
    while (*s != '\0')
    {
        if (*s == (char)c)
            return (char*)s;
        s++;
    }

    if (c == '\0')
        return (char*)s;

    return NULL;
}

char* strrchr(const char* s, int c)
{
    const char* last = NULL;

    while (*s != '\0')
    {
        if (*s == (char)c)
            last = s;
        s++;
    }

    if (c == '\0')
        return (char*)s;

    return (char*)last;
}

char* strdup(const char* s)
{
    size_t len = strlen(s) + 1;
    char* copy = (char*)malloc(len);

    if (copy == NULL) return NULL;

    for (size_t i = 0; i < len; i++)
        copy[i] = s[i];

    return copy;
}


