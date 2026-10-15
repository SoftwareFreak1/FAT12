#include <stdint.h>
#include "vga.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_WHITE_ON_BLACK 0x0F

/* The VGA text buffer at its fixed address. volatile because the display
   hardware reads it, not our code: without it, the compiler could drop or
   merge writes it thinks nothing ever reads */
static volatile uint16_t* vga = (uint16_t*)0xB8000;

/* Where the next character goes: right after the last one we printed */
static int cursor_row = 0;
static int cursor_col = 0;

/* Packs a character and our fixed attribute into one 16-bit cell */
static uint16_t char_to_cell(char c)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)VGA_WHITE_ON_BLACK << 8);
}

void vga_clear(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = char_to_cell(' ');
}

void vga_print(const char* str)
{
    for (int i = 0; str[i] != '\0'; i++)
    {
        /* No scrolling: our kernel never prints more than one screen
           holds, so text past the last row is simply dropped */
        if (cursor_row >= VGA_HEIGHT)
            return;

        if (str[i] == '\n')
        {
            cursor_col = 0;
            cursor_row++;
            continue;
        }

        /* Our lines always fit on the screen, so anything past
           the last column is simply dropped */
        if (cursor_col >= VGA_WIDTH)
            continue;

        vga[cursor_row * VGA_WIDTH + cursor_col] = char_to_cell(str[i]);
        cursor_col++;
    }
}
