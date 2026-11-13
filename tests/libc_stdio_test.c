#include "test_util.h"

#include <stddef.h>
#include <stdint.h>

/* Declare only the functions under test so the host <stdio.h> pulled in by
   test_util.h keeps providing printf/fprintf for the test harness. macOS
   turns sprintf/snprintf into fortify macros, so undo that first. */
#undef sprintf
#undef snprintf
extern int sprintf(char *str, const char *fmt, ...);
extern int snprintf(char *str, size_t size, const char *fmt, ...);

static int test_libc_stdio(void)
{
    char buf[128];

    /* Simple strings. */
    snprintf(buf, sizeof(buf), "hello");
    CHECK_STR(buf, "hello");
    CHECK_EQ(snprintf(buf, sizeof(buf), ""), 0);
    CHECK_STR(buf, "");

    /* Integers. */
    snprintf(buf, sizeof(buf), "%d", 42);
    CHECK_STR(buf, "42");
    snprintf(buf, sizeof(buf), "%d", -7);
    CHECK_STR(buf, "-7");
    snprintf(buf, sizeof(buf), "%i", 123);
    CHECK_STR(buf, "123");
    snprintf(buf, sizeof(buf), "%u", 4000000000u);
    CHECK_STR(buf, "4000000000");
    snprintf(buf, sizeof(buf), "%x", 0xdeadbeefu);
    CHECK_STR(buf, "deadbeef");
    snprintf(buf, sizeof(buf), "%X", 0xdeadbeefu);
    CHECK_STR(buf, "DEADBEEF");
    snprintf(buf, sizeof(buf), "%ld", 9000000000L);
    CHECK_STR(buf, "9000000000");
    snprintf(buf, sizeof(buf), "%d", 0);
    CHECK_STR(buf, "0");

    /* Strings, chars, percent. */
    snprintf(buf, sizeof(buf), "%s", "world");
    CHECK_STR(buf, "world");
    snprintf(buf, sizeof(buf), "[%c]", 'Q');
    CHECK_STR(buf, "[Q]");
    snprintf(buf, sizeof(buf), "100%%");
    CHECK_STR(buf, "100%");
    snprintf(buf, sizeof(buf), "a%sb", "x");
    CHECK_STR(buf, "axb");

    /* NULL string renders as "(null)". */
    snprintf(buf, sizeof(buf), "%s", (const char *)NULL);
    CHECK_STR(buf, "(null)");

    /* Width, padding, alignment. */
    snprintf(buf, sizeof(buf), "[%5d]", 42);
    CHECK_STR(buf, "[   42]");
    snprintf(buf, sizeof(buf), "[%-5d]", 42);
    CHECK_STR(buf, "[42   ]");
    snprintf(buf, sizeof(buf), "[%05d]", 42);
    CHECK_STR(buf, "[00042]");
    snprintf(buf, sizeof(buf), "[%5s]", "hi");
    CHECK_STR(buf, "[   hi]");
    snprintf(buf, sizeof(buf), "[%-5s]", "hi");
    CHECK_STR(buf, "[hi   ]");

    /* Pointer formatting. */
    snprintf(buf, sizeof(buf), "%p", (void *)0x1234);
    CHECK_STR(buf, "0x1234");

    /* Multiple conversions in one format. */
    snprintf(buf, sizeof(buf), "%d/%d/%d", 1, 2, 3);
    CHECK_STR(buf, "1/2/3");
    snprintf(buf, sizeof(buf), "%s %d %x", "val", -1, 0xff);
    CHECK_STR(buf, "val -1 ff");

    /* Truncation: snprintf returns the length it would have written, and the
       buffer is always NUL-terminated within `size`. */
    CHECK_EQ(snprintf(buf, 5, "hello world"), 11);
    CHECK_MEM(buf, "hell", 4);
    CHECK_EQ(buf[4], '\0');
    CHECK_EQ(snprintf(buf, 1, "hello"), 5);
    CHECK_EQ(buf[0], '\0');

    /* sprintf has no size bound. */
    sprintf(buf, "%d-%s", 7, "seven");
    CHECK_STR(buf, "7-seven");

    TEST_REPORT("libc/stdio");
}

int main(void)
{
    return test_libc_stdio();
}
