#ifndef AXIOME_INPUT_ABI_H
#define AXIOME_INPUT_ABI_H

/* Unified GUI input ABI for /Devices/input0.
   Freestanding (stdint only); included by kernel/input.c AND userspace GUI
   programs (axwm, axterm). Transport: read() returns a whole number of
   struct axinput_event (non-blocking, 0 = queue empty); write() of one
   uint32_t sets GUI takeover (1 = grab, 0 = release). While grabbed, the
   PS/2 + USB-HID keyboard paths feed this queue instead of the TTY line
   discipline, so keystrokes go to the compositor rather than the text
   console. Mouse motion is always queued in addition to the legacy
   mouse_read_event() buffer. */

#include <stdint.h>

#define AXINPUT_TYPE_KEY   1u
#define AXINPUT_TYPE_MOUSE 2u

/* Key codes: printable ASCII passes through as-is; special keys reuse the
   TTY key-code range so GUI programs and the console share one namespace. */
#define AXINPUT_KEY_UP     0x100u
#define AXINPUT_KEY_DOWN   0x101u
#define AXINPUT_KEY_LEFT   0x102u
#define AXINPUT_KEY_RIGHT  0x103u
#define AXINPUT_KEY_HOME   0x104u
#define AXINPUT_KEY_END    0x105u
#define AXINPUT_KEY_PGUP   0x106u
#define AXINPUT_KEY_PGDN   0x107u
#define AXINPUT_KEY_INSERT 0x108u
#define AXINPUT_KEY_DELETE 0x109u

/* Mouse buttons bitmask (PS/2 + HID unified). */
#define AXINPUT_BTN_LEFT   0x01u
#define AXINPUT_BTN_RIGHT  0x02u
#define AXINPUT_BTN_MIDDLE 0x04u

struct axinput_event {
    uint32_t type; /* AXINPUT_TYPE_* */
    uint32_t code; /* key: key code; mouse: button bitmask */
    int32_t dx;    /* mouse: relative motion (0 for keys) */
    int32_t dy;    /* mouse: relative motion, +down in screen coords */
};

#define AXINPUT_EVENT_SIZE 16u

#endif
