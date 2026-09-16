#ifndef AXIOME_WM_ABI_H
#define AXIOME_WM_ABI_H

/* Compositor <-> client ABI for the axiomeDE mini desktop.
   Freestanding (stdint only); shared by guixd (compositor), GUI clients
   (axterm --wm, axinfo, axlogin, axoobe) and host tests.

   Pixels: the compositor owns a fixed pool of SHM segments (one per window
   slot, WM_WIN_W x WM_WIN_H x 32bpp + header). A client is spawned as
   `prog --wm <shmid> <w> <h> <evfd> <gen> [...]`, attaches the segment
   by id and draws its content there. The compositor blits the pixels under
   its own title bar / borders each frame.

   Startup: the compositor initialises the whole header (magic, geometry,
   gen, closed=0, seq=0, ready=0) BEFORE spawning, so attach can never race
   the header setup. <gen> is a per-open generation counter; the client
   must refuse to run when hdr->gen disagrees (stale mapping from a
   previous occupant), and must keep re-checking it: a mismatch at any
   time means the slot was reused and the client must exit at once.

   Frame publication is a seqlock over hdr->seq (even = stable):
     client: seq++ (odd: drawing), barrier, draw pixels, [ready=1 the first
             time], barrier, seq++ (even: stable).
     server: s0 = seq; if odd, skip this frame; copy pixels; barrier;
             s1 = seq; if s0 != s1, discard the copy and retry next frame.
   The server therefore never presents a half-drawn client frame.

   Events: the compositor creates a pipe per client and passes the read end
   down by INHERITANCE, with its fd number as the <evfd> argument (never a
   hardcoded number: dup2'ing onto a fixed fd can silently close one of the
   pipe's own ends if the number collides, leaving zero writers and instant
   EOF). It writes struct wm_event records (fixed 16 bytes; the reader must
   frame them, pipes are streams). Key codes reuse the input_abi namespace
   (ASCII + AXINPUT_KEY_*). The compositor asks for exit by setting
   closed=1 AND closing its write end (EOF); clients must honour whichever
   they observe first. */

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
    volatile uint32_t seq;    /* seqlock: even = stable, odd = writer active */
    volatile uint32_t gen;    /* slot generation, bumped per open */
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
