#include "keyboard.h"
#include "printk.h"
#include "tty.h"
#include "io.h"
#include "input.h"
#include "hal/cshim.h"

#define KEYBOARD_IRQ 1

static const char scancode_ansi[128] = {
    [0x00] = 0, [0x01] = 27,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
    [0x35] = '/',
    [0x37] = '*', [0x39] = ' ',
    [0x4A] = '-', [0x4E] = '+',
};

/* Escape sequence state machine */
#define ESC_STATE_IDLE  0
#define ESC_STATE_ESC   1
#define ESC_STATE_BRACK 2
#define ESC_STATE_CSI   3

static int esc_state = ESC_STATE_IDLE;
static int esc_buf[4];
static int esc_idx = 0;

/* PS/2 set-1 shift state + extended (0xE0) prefix. Releases arrive with
   bit 7 set; the GUI input queue only reports presses. */
static int kbd_shift;
static int kbd_e0;

/* Route one decoded key: always into the GUI input queue, and into the TTY
   line discipline only while no desktop holds the grab. */
static void kbd_emit(int c)
{
    input_push_key((uint32_t)c);
    if (!input_gui_grabbed())
        tty_input_char(c);
}

/* Shifted symbol for a scancode whose unshifted char is `base`. Letters are
   uppercased by the caller; this covers digits and punctuation. */
static char kbd_shifted(uint8_t sc, char base)
{
    switch (sc)
    {
        case 0x02: return '!';
        case 0x03: return '@';
        case 0x04: return '#';
        case 0x05: return '$';
        case 0x06: return '%';
        case 0x07: return '^';
        case 0x08: return '&';
        case 0x09: return '*';
        case 0x0A: return '(';
        case 0x0B: return ')';
        case 0x0C: return '_';
        case 0x0D: return '+';
        case 0x1A: return '{';
        case 0x1B: return '}';
        case 0x27: return ':';
        case 0x28: return '"';
        case 0x29: return '~';
        case 0x2B: return '|';
        case 0x33: return '<';
        case 0x34: return '>';
        case 0x35: return '?';
        default: break;
    }
    if (base >= 'a' && base <= 'z')
        return (char)(base - 'a' + 'A');
    return base;
}

static void handle_escape(int c)
{
    switch (esc_state) {
    case ESC_STATE_IDLE:
        if (c == 27) {
            esc_state = ESC_STATE_ESC;
            esc_idx = 0;
        } else {
            kbd_emit(c);
        }
        break;
        
    case ESC_STATE_ESC:
        if (c == '[') {
            esc_state = ESC_STATE_BRACK;
            esc_idx = 0;
        } else {
            esc_state = ESC_STATE_IDLE;
            kbd_emit(27);
            if (c >= 32 && c < 127) kbd_emit(c);
        }
        break;
        
    case ESC_STATE_BRACK:
        if (c >= '0' && c <= '9') {
            esc_buf[esc_idx++] = c;
            if (esc_idx >= 4) {
                esc_state = ESC_STATE_IDLE;
                kbd_emit(27);
            }
        } else if (c >= 'A' && c <= 'D') {
            /* Arrow keys: [A, [B, [C, [D */
            esc_state = ESC_STATE_IDLE;
            if (c == 'A') kbd_emit(TTY_KEY_UP);
            else if (c == 'B') kbd_emit(TTY_KEY_DOWN);
            else if (c == 'C') kbd_emit(TTY_KEY_RIGHT);
            else if (c == 'D') kbd_emit(TTY_KEY_LEFT);
        } else if (c == 'H') {
            esc_state = ESC_STATE_IDLE;
            kbd_emit(TTY_KEY_HOME);
        } else if (c == 'F') {
            esc_state = ESC_STATE_IDLE;
            kbd_emit(TTY_KEY_END);
        } else if (c == '~') {
            /* Handle [1~, [3~, [4~, etc */
            esc_state = ESC_STATE_IDLE;
            if (esc_idx == 1 && esc_buf[0] == '1') {
                kbd_emit(TTY_KEY_HOME);
            } else if (esc_idx == 1 && esc_buf[0] == '3') {
                kbd_emit(TTY_KEY_DELETE);
            } else if (esc_idx == 1 && esc_buf[0] == '4') {
                kbd_emit(TTY_KEY_END);
            } else if (esc_idx == 1 && esc_buf[0] == '5') {
                kbd_emit(TTY_KEY_PGUP);
            } else if (esc_idx == 1 && esc_buf[0] == '6') {
                kbd_emit(TTY_KEY_PGDN);
            }
        } else {
            esc_state = ESC_STATE_IDLE;
            kbd_emit(27);
            kbd_emit('[');
            for (int i = 0; i < esc_idx; i++)
                kbd_emit(esc_buf[i]);
            if (c >= 32 && c < 127) kbd_emit((char)c);
        }
        break;
    }
}

void keyboard_irq_handler(void)
{
    uint8_t status = inb(0x64);
    if (status & 1)
    {
        uint8_t scancode = inb(0x60);
        /* Extended prefix: the next byte is an E0 code. */
        if (scancode == 0xE0)
        {
            kbd_e0 = 1;
            return;
        }
        /* Releases only update modifier state (GUI queue is press-only). */
        if (scancode & 0x80)
        {
            uint8_t rel = (uint8_t)(scancode & 0x7F);
            if (!kbd_e0 && (rel == 0x2A || rel == 0x36))
                kbd_shift = 0;
            kbd_e0 = 0;
            return;
        }
        if (kbd_e0)
        {
            kbd_e0 = 0;
            switch (scancode)
            {
                case 0x48: kbd_emit(TTY_KEY_UP); break;
                case 0x50: kbd_emit(TTY_KEY_DOWN); break;
                case 0x4B: kbd_emit(TTY_KEY_LEFT); break;
                case 0x4D: kbd_emit(TTY_KEY_RIGHT); break;
                case 0x47: kbd_emit(TTY_KEY_HOME); break;
                case 0x4F: kbd_emit(TTY_KEY_END); break;
                case 0x49: kbd_emit(TTY_KEY_PGUP); break;
                case 0x51: kbd_emit(TTY_KEY_PGDN); break;
                case 0x52: kbd_emit(TTY_KEY_INSERT); break;
                case 0x53: kbd_emit(TTY_KEY_DELETE); break;
                default: break;
            }
            return;
        }
        /* Modifier presses. */
        if (scancode == 0x2A || scancode == 0x36)
        {
            kbd_shift = 1;
            return;
        }
        if (scancode < 0x80)
        {
            char c = scancode_ansi[scancode];
            if (c && kbd_shift)
                c = kbd_shifted(scancode, c);
            if (c) {
                /* Handle escape sequences in the keyboard driver */
                if (c == 27) {
                    esc_state = ESC_STATE_ESC;
                    esc_idx = 0;
                } else if (esc_state != ESC_STATE_IDLE) {
                    handle_escape(c);
                } else {
                    kbd_emit(c);
                }
            }
        }
    }
}

static int keyboard_self_test(void)
{
    outb(0x64, 0xAA);
    for (int i = 0; i < 10000; i++)
    {
        if (inb(0x64) & 1)
            return inb(0x60) == 0x55;
    }
    return 0;
}

void keyboard_init(void)
{
    printk("Keyboard: probing PS/2 controller...\n");

    if (!keyboard_self_test())
    {
        printk("Keyboard: self-test failed (no PS/2 controller?)\n");
        return;
    }

    outb(0x64, 0x60);
    outb(0x60, 0x47);

    outb(0x60, 0xF4);
    for (int i = 0; i < 1000; i++)
    {
        if (inb(0x64) & 1)
            break;
    }
    if (inb(0x64) & 1)
        inb(0x60);

    hal_irq_mask(KEYBOARD_IRQ, 0);
    printk("Keyboard: ready\n");
}
