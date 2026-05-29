#include <stdint.h>
#include "vga.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static volatile uint16_t* vga = (uint16_t*)0xB8000;
static int cursor_row = 0;
static int cursor_col = 0;

void vga_clear(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = (uint16_t)(' ' | (0x0F << 8));
}

void vga_print(const char* str)
{
    for (int i = 0; str[i] != '\0'; i++)
    {
        if (str[i] == '\n')
        {
            cursor_col = 0;
            cursor_row++;
            continue;
        }
        vga[cursor_row * VGA_WIDTH + cursor_col] =
            (uint16_t)str[i] | ((uint16_t)0x0F << 8);
        cursor_col++;
    }
}
