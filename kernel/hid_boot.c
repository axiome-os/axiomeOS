#include "keyboard.h"
#include "tty.h"
#include "input.h"

static uint8_t previous[6];

static int key_present(const uint8_t keys[6], uint8_t key)
{
    for (int i = 0; i < 6; i++)
        if (keys[i] == key)
            return 1;
    return 0;
}

static int hid_key(uint8_t key, int shift)
{
    static const char digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";

    if (key >= 0x04 && key <= 0x1d)
        return (shift ? 'A' : 'a') + key - 0x04;
    if (key >= 0x1e && key <= 0x27)
        return (shift ? shifted_digits : digits)[key - 0x1e];

    switch (key) {
    case 0x28: return '\n';
    case 0x29: return 27;
    case 0x2a: return '\b';
    case 0x2b: return '\t';
    case 0x2c: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x49: return TTY_KEY_INSERT;
    case 0x4a: return TTY_KEY_HOME;
    case 0x4b: return TTY_KEY_PGUP;
    case 0x4c: return TTY_KEY_DELETE;
    case 0x4d: return TTY_KEY_END;
    case 0x4e: return TTY_KEY_PGDN;
    case 0x4f: return TTY_KEY_RIGHT;
    case 0x50: return TTY_KEY_LEFT;
    case 0x51: return TTY_KEY_DOWN;
    case 0x52: return TTY_KEY_UP;
    default: return 0;
    }
}

void keyboard_hid_boot_report(uint8_t modifiers, const uint8_t keys[6])
{
    for (int i = 0; i < 6; i++)
        if (keys[i] >= 1 && keys[i] <= 3)
            return;

    int shift = (modifiers & 0x22) != 0;
    for (int i = 0; i < 6; i++) {
        uint8_t key = keys[i];
        if (key && !key_present(previous, key)) {
            int c = hid_key(key, shift);
            if (c)
            {
                input_push_key((uint32_t)c);
                if (!input_gui_grabbed())
                    tty_input_char(c);
            }
        }
    }
    for (int i = 0; i < 6; i++)
        previous[i] = keys[i];
}
