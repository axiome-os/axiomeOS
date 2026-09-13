#ifndef AXIOME_WM_ABI_H
#define AXIOME_WM_ABI_H

/* Compositor <-> client ABI for the axiomeDE mini desktop.
   Freestanding (stdint only); shared by axwm (compositor), GUI clients
   (axterm --wm, axinfo) and host tests.

   Pixels: the compositor owns a fixed pool of SHM segments (one per window
   slot, WM_WIN_W x WM_WIN_H x 32bpp + header). A client is spawned as
   `prog --wm <shmid> <w> <h> <evfd> [-c <command...>]`, attaches the segment
   by id and draws its content there. The compositor blits the pixels under
   its own title bar / borders each frame.

   Events: the compositor creates a pipe per client and passes the read end
   down by INHERITANCE, with its fd number as the <evfd> argument (never a
   hardcoded number: dup2'ing onto a fixed fd can silently close one of the
   pipe's own ends if the number collides, leaving zero writers and instant
   EOF). It writes struct wm_event records (fixed 16 bytes; the reader must
   frame them, pipes are streams). Key codes reuse the input_abi namespace
   (ASCII + AXINPUT_KEY_*). */

#include <stdint.h>

#define WM_MAGIC 0x57494E44u /* "WIND" */

#define WM_EV_KEY   1u
#define WM_EV_MOUSE 2u

struct wm_event {
    uint32_t type; /* WM_EV_* */
    uint32_t code; /* key: key code; mouse: button bitmask */
    int32_t x;     /* mouse: window-content-local coords (keys: 0) */
    int32_t y;
};

#define WM_EVENT_SIZE 16u

/* SHM window segment layout: header followed by w*h u32 pixels. */
struct wm_win_hdr {
    uint32_t magic;    /* WM_MAGIC once the compositor assigned the slot */
    uint32_t w;
    uint32_t h;
    volatile uint32_t ready;  /* client sets 1 after the first full draw */
    volatile uint32_t closed; /* compositor sets 1 to ask for exit */
    volatile uint32_t seq;    /* client bumps per redraw (liveness) */
    uint32_t reserved;
    uint32_t pad; /* pixel area starts at byte 32 (see WM_HDR_SIZE) */
};

#define WM_HDR_SIZE 32u

/* Fixed client geometry: every window slot carries this many pixels, so the
   compositor can pre-allocate its SHM pool once (and a client buffer always
   fits one SHM_SLOT of 2 MiB). Windows are draggable but not resizable. */
#define WM_WIN_W 620u
#define WM_WIN_H 420u
#define WM_MAX_WIN 6u

#endif
