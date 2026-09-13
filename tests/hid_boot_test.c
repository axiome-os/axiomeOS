#include "test_util.h"

#include <stdint.h>
#include <string.h>

#include "keyboard.h"
#include "tty.h"

/* Host stub for the kernel's TTY input hook: records emitted characters. */
static int emit[128];
static int emit_count;

void tty_input_char(int c)
{
    if (emit_count < (int)(sizeof(emit) / sizeof(emit[0])))
        emit[emit_count++] = c;
}

/* Host stubs for the GUI input queue (kernel/input.c): the HID path must
   mirror every key into the queue in addition to the TTY. */
static int in_emit[128];
static int in_emit_count;

void input_push_key(uint32_t code)
{
    if (in_emit_count < (int)(sizeof(in_emit) / sizeof(in_emit[0])))
        in_emit[in_emit_count++] = (int)code;
}

int input_gui_grabbed(void)
{
    return 0;
}

static void reset_emit(void)
{
    emit_count = 0;
    in_emit_count = 0;
}

/* Send an empty report so the previous-key tracking in hid_boot.c resets. */
static void release_all(void)
{
    uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
    keyboard_hid_boot_report(0, keys);
}

static int test_hid_boot(void)
{
    uint8_t keys[6];

    /* Single letter, unshifted. */
    release_all();
    reset_emit();
    memset(keys, 0, sizeof(keys));
    keys[0] = 0x04; /* 'a' */
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit_count, 1);
    CHECK_EQ(emit[0], 'a');
    /* GUI input queue mirrors the TTY while ungrabbed. */
    CHECK_EQ(in_emit_count, 1);
    CHECK_EQ(in_emit[0], 'a');

    /* Same key held -> no repeat (edge-triggered). */
    reset_emit();
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit_count, 0);

    /* Released then re-pressed -> new event. */
    release_all();
    reset_emit();
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit_count, 1);
    CHECK_EQ(emit[0], 'a');

    /* Shift modifier (0x22 = LShift | RShift) -> uppercase. */
    release_all();
    reset_emit();
    keyboard_hid_boot_report(0x22, keys);
    CHECK_EQ(emit_count, 1);
    CHECK_EQ(emit[0], 'A');

    /* Non-shift modifiers must not uppercase. */
    release_all();
    reset_emit();
    keyboard_hid_boot_report(0x01, keys); /* Ctrl */
    CHECK_EQ(emit_count, 1);
    CHECK_EQ(emit[0], 'a');

    /* Digits + shifted symbols. */
    release_all();
    reset_emit();
    memset(keys, 0, sizeof(keys));
    keys[0] = 0x1e; /* '1' */
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit[0], '1');

    release_all();
    reset_emit();
    keyboard_hid_boot_report(0x22, keys);
    CHECK_EQ(emit[0], '!');

    /* Symbol keys: '-' / '_'. */
    release_all();
    reset_emit();
    memset(keys, 0, sizeof(keys));
    keys[0] = 0x2d;
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit[0], '-');
    release_all();
    reset_emit();
    keyboard_hid_boot_report(0x22, keys);
    CHECK_EQ(emit[0], '_');

    /* Control characters. */
    release_all();
    reset_emit();
    memset(keys, 0, sizeof(keys));
    keys[0] = 0x28; /* Enter */
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit[0], '\n');

    release_all();
    reset_emit();
    keys[0] = 0x2a; /* Backspace */
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit[0], '\b');

    release_all();
    reset_emit();
    keys[0] = 0x2c; /* Space */
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit[0], ' ');

    /* Navigation / editing keys map to TTY key codes. */
    release_all();
    reset_emit();
    keys[0] = 0x52; /* Up */
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit[0], TTY_KEY_UP);

    release_all();
    reset_emit();
    keys[0] = 0x4a; /* Home */
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit[0], TTY_KEY_HOME);

    release_all();
    reset_emit();
    keys[0] = 0x49; /* Insert */
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit[0], TTY_KEY_INSERT);

    /* Unknown key codes are ignored (hid_key returns 0). */
    release_all();
    reset_emit();
    memset(keys, 0, sizeof(keys));
    keys[0] = 0x7f;
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit_count, 0);

    /* Boot-protocol modifier keys (1..3) must be rejected outright. */
    release_all();
    reset_emit();
    memset(keys, 0, sizeof(keys));
    keys[0] = 0x04;
    keys[1] = 0x01; /* mouse button */
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit_count, 0);

    /* Two distinct keys in one report -> two events, in order. */
    release_all();
    reset_emit();
    memset(keys, 0, sizeof(keys));
    keys[0] = 0x04; /* 'a' */
    keys[1] = 0x1e; /* '1' */
    keyboard_hid_boot_report(0, keys);
    CHECK_EQ(emit_count, 2);
    CHECK_EQ(emit[0], 'a');
    CHECK_EQ(emit[1], '1');

    /* Upper-case via shift applies to every key in the report. */
    release_all();
    reset_emit();
    keyboard_hid_boot_report(0x22, keys);
    CHECK_EQ(emit_count, 2);
    CHECK_EQ(emit[0], 'A');
    CHECK_EQ(emit[1], '!');

    TEST_REPORT("kernel/hid_boot");
}

int main(void)
{
    return test_hid_boot();
}
