#ifndef AXIOME_INPUT_H
#define AXIOME_INPUT_H

#include <stdint.h>

/* Unified GUI input queue (kernel/input.c). IRQ-safe producers, consumed
   from userspace through /Devices/input0. */
void input_init(void);
void input_push_key(uint32_t code);
void input_push_mouse(int dx, int dy, uint32_t buttons);

/* Non-zero while a GUI program holds the input grab (keyboard bypasses the
   TTY). Set/cleared by writing 1/0 to /Devices/input0. */
int input_gui_grabbed(void);

#endif
