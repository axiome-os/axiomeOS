# axmui — native HTML/CSS UI toolkit

axmui is the **de-facto standard GUI toolkit for axiomeOS**. Every built-in graphical
application (`axterm`, `axinfo`, `axlogin`, `axoobe`) is built on it. It replaces the
ad-hoc `axgui.h`/`axform.h` immediate-mode drawing with a declarative, beautiful,
retain-mode system that looks like the web but renders natively.

> **No WebKit, no browser engine.** HTML is parsed into a native DOM, CSS into
> a native stylesheet, flex/block layout is solved in C, and pixels are
> rasterized with `axgui_fill`/`axgui_text` directly into the window SHM dumb
> buffer. The guixd compositor presents the buffer. The mental model is web,
> the execution is native.

## Design goals

* **Beautiful out of the box** — rounded cards, subtle borders, coherent dark
  palette, spacing via `gap`/`padding`/`margin`, `:hover`/`:active`/`:focus`
  pseudo-states, focus rings, placeholder styling. No app has to reinvent
  metrics.
* **HTML+CSS native** — authors write `<div class="card"><button>OK</button></div>`
  and `.card{ background:#1E2A3A; border-radius:12px; padding:16px; gap:10px; }`
  and get real widgets. Selectors (`tag`, `.class`, `#id`, `A B`, `,*`, `:hover`
  etc.) and properties (`display:flex`, `flex-direction`, `justify-content`,
  `gap`, `width`/`height`/`margin`/`padding`, `background`, `border`,
  `border-radius`, `font-weight`, `text-align`, `opacity`…) are supported.
* **Cocoa/GTK-like C API** — `axmui_app_t` / `axmui_window_t` / `axmui_view_t`,
  imperative helpers (`axmui_button_create`, `axmui_view_append`, …) plus
  declarative `axmui_window_set_html(html,css)`. Callbacks via
  `axmui_view_on_click(view, fn, userdata)`. Custom drawing via
  `axmui_view_set_draw(view, fn, data)` (used by `axterm`'s terminal grid).
* **Window-system native** — wraps `axclient_init`/`axgui_win_attach`/seqlock
  (`axclient_begin`→draw→`axclient_commit`). Handles `hover`/`active`/`focused`,
  hit-testing, Tab focus cycling, mouse→`WM_EV_MOUSE` and key→`WM_EV_KEY`
  dispatch. Compositor close (`hdr->closed` + EOF) and generation staleness are
  honoured.

## Layout

```
kernel/userspace/axmui/axmui.h   ← single header (header-only, like axgui.h)
```

Header-only so every `USER_PROGS` links it with no Makefile churn. Internals:

* **CSS sheet** — `axmui_sheet` with `axmui_rule[NRULES]`; `axmui_css_parse`
  splits `selector{ decl; }`, expands comma groups, detects `:hover`/`:active`/`:focus`.
* **HTML parser** — tiny iterative parser (`<tag attr="…">`/`</tag>`/`text` and void
  elements `input/br`). Builds an `axmui_view_t` tree in a bump pool
  (`AXMUI_MAX_NODES` 128, no `malloc` per node).
* **Style resolution** — UA defaults per tag (`button`/`input`/`h1`…), then sheet
  rules in order (`axmui_match_selector`), then `style="…"` inline (highest).
  Pseudo-states re-tint (`:hover` brightens ~10 %, `:active` darkens ~15 %).
* **Layout** — integer-px flexbox (`row`/`column`, `gap`, `justify`, `align`,
  `flex-grow`) + block vertical stack. `width/height: px/%/auto`,
  `margin`/`padding`/`border` box model. `axmui_layout` writes `x/y/w/h/abs_x/abs_y`.
* **Render** — `axmui_render_node` draws `background`/`border`/`border-radius`
  (quarter-circle fill), then `text`/`value`/`placeholder` (secure `*` for
  passwords), caret for focused inputs, then children. Custom draw bypasses this
  for `axterm`.

All coordinates are `int` px; the window is fixed `WM_WIN_W×WM_WIN_H` (620×420).

## API

```c
#include "axmui/axmui.h"

axmui_app_t    *app = axmui_app_create();
axmui_window_t *win = axmui_window_create(app, "Title", 620, 420);
axmui_view_t   *root = axmui_window_root(win);

/* declarative */
axmui_window_set_html(win,
  "<div class='col'><div class='card'><h1>Hi</h1><button id='ok'>OK</button></div></div>",
  ".card{ background:#1E2A3A; border-radius:12px; padding:16px; }");

/* imperative (same tree) */
axmui_view_t *btn = axmui_button_create(win, "OK");
axmui_view_set_id(btn, "ok2");
axmui_view_set_class(btn, "btn-primary");
axmui_view_append(root, btn);

/* events */
axmui_view_on_click(btn, on_ok, userdata);
axmui_window_on_key(win, on_any_key, userdata);

/* input */
axmui_view_t *inp = axmui_input_create(win, "username", 0);
axmui_view_t *found = axmui_view_find(root, "ok");
axmui_view_get_value(inp);            /* current value */
axmui_view_set_draw(found, custom, data); /* e.g. terminal */

/* run (blocks until the compositor closes the window) */
return axmui_app_run(app, argc, argv, "myapp");
```

`axmui_app_run` hides `axclient_init`/`axgui_win_attach`/`seqlock`/`WM_EV_*`
dispatch, hover/active tracking, `Tab` focus cycling, and re-renders on every
state change. For custom multiplexing (e.g. `axterm`'s shell pipe) the app can
drive `axmui_window_attach` + `axmui_window_render` + `axclient_begin/commit`
manually — still fully axmui.

## Built-ins that use it

| Binary | What it shows | axmui surface |
|---|---|---|
| `axinfo` | `uname`, clock, window geometry, help | HTML card + `Close` button, `any-key-to-close` via `axmui_window_on_key` |
| `axlogin` | username / password form | two `input` (secure), `Login` button, `status` label, `Tab` cycle, secure scrubbing |
| `axoobe` | first-boot username / password / confirm + admin toggle | three `input`, `[ ] Administrator` toggle button, `Create` button, `status` |
| `axterm` | VT100 terminal on `/bin/sh` | HTML chrome (`header` + `card #term` + footer) around a `custom_draw` terminal grid; shell pipes, fork/helper/ `kill_process_tree` preserved |

`guixd`/`axwm` (the compositor itself), `gfx_test` and `evtest` remain low-level
by design — they own the scanout or are test harnesses.

## Extending

* Add a new selector/property: teach `axmui_apply_decl` and `axmui_match_simple`.
* Add a new widget: create a `tag` with UA defaults in `axmui_compute_style_for`,
  handle its `text`/`value`/`placeholder` in `axmui_render_node`, expose a helper
  like `axmui_label_create`.
* Custom pixels: `axmui_view_set_draw(view, fn, data)` — `fn` receives the SHM
  `axgui_fb` and the view's `abs_x/abs_y/w/h` and can draw anything with
  `axgui_*` primitives.

## Why not WebKit?

AxiomeOS is a tiny freestanding kernel with a synchronous `write(DRM)` → `PRESENT`
scanout and a per-window SHM seqlock. WebKit would bring a multi-process browser,
JS GC, GPU shaders and megabytes of font shaping for zero gain. axmui is ~1500
lines of C, no dependencies beyond `libc.sl`, and paints 60 fps in software via
the same path Gallium `softpipe` would use.

## Further reading

* `kernel/userspace/axmui/axmui.h` — implementation (search `axmui_`).
* `kernel/wm_abi.h` — SHM header + `WM_EV_*` ABI.
* `kernel/userspace/axclient.h` — attach + seqlock helpers.
* `ports/mesa-axiome/WINSYS.md` — Mesa softpipe transport that will replace
  the current software `axgui` raster in a future DRI accelerator.
