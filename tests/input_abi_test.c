/* Host test for the GUI input ABI (kernel/input_abi.h).
   Checks the event layout the kernel queue and the userspace GUI programs
   (axwm/axterm via axgui.h) agree on: 16-byte events, stable type codes and
   key namespace shared with the TTY special keys. */
#include <stdio.h>
#include <stddef.h>

#include "../kernel/input_abi.h"

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); return 1; } \
} while (0)

int main(void)
{
    CHECK(sizeof(struct axinput_event) == 16u);
    CHECK(AXINPUT_EVENT_SIZE == 16u);
    CHECK(offsetof(struct axinput_event, type) == 0);
    CHECK(offsetof(struct axinput_event, code) == 4);
    CHECK(offsetof(struct axinput_event, dx) == 8);
    CHECK(offsetof(struct axinput_event, dy) == 12);
    CHECK(AXINPUT_TYPE_KEY == 1u);
    CHECK(AXINPUT_TYPE_MOUSE == 2u);
    /* Key namespace mirrors kernel/tty.h TTY_KEY_* (0x100 base). */
    CHECK(AXINPUT_KEY_UP == 0x100u);
    CHECK(AXINPUT_KEY_DOWN == 0x101u);
    CHECK(AXINPUT_KEY_LEFT == 0x102u);
    CHECK(AXINPUT_KEY_RIGHT == 0x103u);
    CHECK(AXINPUT_KEY_DELETE == 0x109u);
    CHECK(AXINPUT_BTN_LEFT == 0x01u);
    CHECK(AXINPUT_BTN_RIGHT == 0x02u);
    CHECK(AXINPUT_BTN_MIDDLE == 0x04u);
    printf("PASS input/abi (14 checks)\n");
    return 0;
}
