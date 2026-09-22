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

/* Capability bits (mirrors gfx::GfxCap) for GUI gating */
#define WM_GFX_CAP_HW_FILL    (1ull << 0)
#define WM_GFX_CAP_HW_BLIT    (1ull << 1)
#define WM_GFX_CAP_HW_FLIP    (1ull << 2)
#define WM_GFX_CAP_3D         (1ull << 3)
#define WM_GFX_CAP_ALPHA      (1ull << 4)
#define WM_GFX_CAP_COMPOSITOR (1ull << 5)
#define WM_GFX_CAP_BLUR       (1ull << 6)
#define WM_GFX_CAP_SHADOWS    (1ull << 7)
#define WM_GFX_CAP_SHADERS    (1ull << 8)
#define WM_GFX_CAP_VSYNC      (1ull << 9)
#define WM_GFX_CAP_HW_CURSOR  (1ull << 10)
#define WM_GFX_CAP_SCALE      (1ull << 11)
#define WM_GFX_CAP_YUV        (1ull << 12)

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
    /* Extended caps: compositor publishes gfx caps here at window creation
       (and on hot-update). Clients read without extra IPC. */
    volatile uint64_t gfx_caps;   /* GfxCap bitmask */
    volatile uint32_t gfx_detail; /* 0=simplified 1=detailed */
    uint32_t pad2;
};

#define WM_HDR_SIZE 48u

static inline int wm_has_cap(const struct wm_win_hdr *hdr, uint64_t cap)
{
    return hdr && (hdr->gfx_caps & cap);
}
static inline const char *wm_detail_mode(const struct wm_win_hdr *hdr)
{
    if (!hdr) return "simplified";
    return hdr->gfx_detail ? "detailed" : "simplified";
}

/* Fixed client geometry: every window slot carries this many pixels, so the
   compositor can pre-allocate its SHM pool once (and a client buffer always
   fits one SHM_SLOT of 2 MiB). Windows are draggable but not resizable. */
#define WM_WIN_W 620u
#define WM_WIN_H 420u
#define WM_MAX_WIN 6u

#endif
