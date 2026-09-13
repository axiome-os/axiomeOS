/* Host test for the compositor/client ABI (kernel/wm_abi.h).
   Checks the event + SHM-header layout axwm and its clients (axterm --wm,
   axinfo) agree on, and the fixed window geometry the SHM pool relies on. */
#include <stdio.h>
#include <stddef.h>

#include "../kernel/wm_abi.h"

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); return 1; } \
} while (0)

int main(void)
{
    CHECK(sizeof(struct wm_event) == 16u);
    CHECK(WM_EVENT_SIZE == 16u);
    CHECK(offsetof(struct wm_event, type) == 0);
    CHECK(offsetof(struct wm_event, code) == 4);
    CHECK(offsetof(struct wm_event, x) == 8);
    CHECK(offsetof(struct wm_event, y) == 12);
    CHECK(WM_EV_KEY == 1u);
    CHECK(WM_EV_MOUSE == 2u);
    CHECK(sizeof(struct wm_win_hdr) == 32u);
    CHECK(WM_HDR_SIZE == 32u);
    CHECK(WM_MAGIC == 0x57494E44u);
    /* Fixed geometry must fit one SHM_SLOT (2 MiB) with room for header. */
    CHECK(WM_WIN_W == 620u && WM_WIN_H == 420u);
    CHECK(WM_HDR_SIZE + (uint64_t)WM_WIN_W * WM_WIN_H * 4u < 0x200000ULL);
    CHECK(WM_MAX_WIN == 6u);
    printf("PASS wm/abi (14 checks)\n");
    return 0;
}
