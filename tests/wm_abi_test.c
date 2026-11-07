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
    CHECK(sizeof(struct wm_win_hdr) == 56u);
    CHECK(WM_HDR_SIZE == 56u);
    CHECK(offsetof(struct wm_win_hdr, gfx_caps) == 32u);
    CHECK(offsetof(struct wm_win_hdr, gfx_detail) == 40u);
    CHECK(offsetof(struct wm_win_hdr, pitch) == 48u);
    CHECK(WM_MAGIC == 0x57494E44u);
    /* Fixed geometry must fit one SHM_SLOT (8 MiB) with room for header. */
    CHECK(WM_WIN_W == 620u && WM_WIN_H == 420u);
    CHECK(WM_HDR_SIZE + (uint64_t)WM_SHM_MAX_W * WM_SHM_MAX_H * 4u < 0x800000ULL);
    CHECK(WM_MAX_WIN == 6u);
    printf("PASS wm/abi (15 checks)\n");
    return 0;
}
