# Lua plugins

The meson build embeds LuaJIT (`-Dlua=auto`, on whenever LuaJIT and PCRE2 are
found) as a lightweight replacement for the perl extensions of the autoconf
build.

## Startup configuration

`$XDG_CONFIG_HOME/nrxvt/init.lua` (default `~/.config/nrxvt/init.lua`) is
loaded automatically if it exists, before plugins and before fonts, window
geometry, visual selection, and the child process are initialized. An
`init.lua` works without any plugins. Syntax and execution errors report a
traceback where available and abort startup rather than silently applying
an incomplete configuration.

Use `nrxvt -c /path/to/config.lua` to load a different startup file instead.
Relative paths are resolved from the current working directory. The selected
file must exist and load successfully; the default `init.lua` is not loaded.
Plugin and helper-module lookup still uses the usual configuration directory.

The implicit `nrxvt` global provides typed startup options using the
existing X resource names:

```lua
-- Font fallback order and scrollback capacity.
nrxvt.opt.font = "xft:monospace:size=11"
nrxvt.opt.saveLines = 50000
nrxvt.opt.scrollBar = false
nrxvt.opt.background = "#272822"
nrxvt.opt.color16 = "#fd971f"

-- Resource names containing punctuation use bracket notation.
nrxvt.opt["print-pipe"] = "cat > /tmp/nrxvt-screen.txt"

-- Key bindings call the runtime API directly.
nrxvt.api.bind ("C-S-v", function (t)
  nrxvt.api.selection_request (t, true)
end)
```

`nrxvt.opt` accepts every built-in resource available in the build, plus
all supported indexed palette colors. Boolean resources require Lua
booleans, numeric resources require finite Lua numbers, and other
resources require strings. Unknown or unavailable option names raise an
error. Assigning `nil` removes a Lua default. Reads return the configured
Lua default, or `nil` if none was assigned, rather than the effective
terminal state.

Precedence is built-in defaults, Xresources, Lua configuration, then
explicit command-line options (including `-xrm`). Lua defaults are copied
to each terminal without modifying the shared X display's resource
database. `nrxvt.opt` only accepts built-in terminal settings. Runtime
functions, hook registration, and constants are exposed by `nrxvt.api`.

Configuration and plugins are loaded once per interpreter. Startup option
assignments are allowed during that load; later writes raise an error.
Live reconfiguration is not provided by this interface. Bind callbacks
with `nrxvt.api.bind` and register hooks with `nrxvt.api.on_*` from `init.lua`,
just as a plugin does. Helper modules can live in `nrxvt/lua/?.lua` or
`nrxvt/lua/?/init.lua` below the configuration directory and be loaded with
`require`.

## Loading plugins

Every `*.lua` file in `$XDG_CONFIG_HOME/nrxvt/plugins` (by default
`~/.config/nrxvt/plugins`) is loaded once at startup, in alphabetical order.
Files starting with a dot are skipped. Helper modules that should not be
loaded as plugins can live in `plugins/lib/` and be loaded with `require`.

Without an `init.lua` or any plugin file no Lua state is created. Only
registered hooks are called, so unused hooks cost nothing.

Four example plugins are installed to `share/nrxvt/lua` (see `src/lua/`):

- `keyboard-select.lua` selects and copies text with vi-like keys (`M-Escape`),
  a port of the keyboard-select perl extension
- `links.lua` underlines URLs, opens them with Ctrl + click and lets you
  select them from the keyboard (`M-u`)
- `search.lua` searches incrementally through the whole scrollback (`M-s`)
- `scrollbar.lua` provides a 4px scrollbar with a draggable proportional handle, anchored left or right according to the window's position on its monitor

Enable them by copying or linking them into the plugin directory, and change
their settings (keys, opener, ...) at the top of the files.

The scrollbar compares the decorated window's center with its monitor's center, including i3's advertised border extents. Windows to the right get a right scrollbar; left and centered windows keep a left scrollbar. Placement updates on moves, resizes, mapping, and window property changes. This detects monitor-relative position, not the window's ancestry in a nested i3 split. With multiple monitors it uses the active RandR CRTC with the largest frame overlap; without RandR it uses the whole X screen.

## Writing plugins

The implicit `nrxvt` global is available before plugins are loaded. No
`require` call is needed. Register callbacks with the `nrxvt.api.on_*` functions:

```lua
nrxvt.api.on_start (function (t)
  nrxvt.api.warn ("terminal with " .. nrxvt.api.nrow (t) .. " rows started")
end)

-- typing "!" rings the bell instead of sending it
nrxvt.api.on_key_press (function (t, ev, keysym, str)
  if str == "!" then
    nrxvt.api.feed (t, "\a")
    return true
  end
end)
```

Each registration appends a callback. Callbacks run in file loading order,
then registration order within each file. Register existing functions
directly, e.g. `nrxvt.api.on_refresh_begin (sync)`. Registration is allowed only
while loading `init.lua`, plugins, and their required modules, before any
terminal hooks run. The registrations apply to every terminal using the
interpreter; keep per-terminal state on `t` rather than in shared locals.

If a file fails to load, its hook registrations are discarded. An
`init.lua` failure aborts startup; a plugin failure is reported and loading
continues with the next file. Returned hook tables are no longer supported:
startup files must return no value (or `nil`), otherwise their hook
registrations are discarded and an error is reported. Helper modules loaded
with `require` can still return values as usual.

Hooks are called with the terminal object `t` followed by the hook
arguments. If a hook returns a true value, the event counts as consumed:
the plugins after it are skipped, and for most hooks urxvt skips its own
handling too, e.g. the key is not sent to the program.

A hook that raises an error is reported on stderr once, with a traceback,
and is then disabled.

The terminal object is a table, so plugins can keep per-terminal state in
it. Prefix the fields with the plugin name to avoid clashes, e.g.
`t.search = {...}`. Pass this table as the first argument of terminal API
functions, for example `nrxvt.api.nrow (t)`. The table has no terminal
methods. Once the terminal is destroyed, API calls using it raise an error.

### Rows and columns

Rows are numbered relative to the top of the screen. Row 0 is the first row
of the screen, and `nrxvt.api.nrow (t) - 1` is the last one. Negative rows are in the
scrollback, down to `nrxvt.api.top_row (t)`. The window shows the rows from
`nrxvt.api.view_start (t)` (which is 0 or negative) to `nrxvt.api.view_start (t) + nrxvt.api.nrow (t) - 1`.

Columns start at 0. The text of a row is UTF-8, so byte offsets and columns
differ for non-ASCII text and wide characters. `nrxvt.api.col (t, row, byte)` maps a
byte offset to a column.

### Renditions

Renditions (colors and attributes) are 32-bit masks. `nrxvt.api.style` builds a
pair of masks, and the functions that take one apply
`rend = (rend & and_mask) | or_mask`:

```lua
local URL = { nrxvt.api.style { underline = true, fg = 4 } }
nrxvt.api.rend (t, row, c0, c1, unpack (URL))
```

Build styles once at load time, not in hooks.

## Hooks

The hooks are the ones of the perl interface (see `urxvtperl(3)`). Their
arguments are listed after `t`. X events (`ev`) are tables with `type`,
`window` and `send_event`. Key, button and motion events also have `x`, `y`,
`x_root`, `y_root`, `row` (absolute, like `nrxvt.api.line`), `col`, `state`, `time`,
`shift`, `ctrl` and `meta`, plus `keycode` or `button`.

| hook | arguments | notes |
|---|---|---|
| `on_init` | | before the window exists |
| `on_start` | | the window and the program are running |
| `on_destroy` | | the terminal is about to go away |
| `on_reset` | | the terminal was reset |
| `on_child_start` | pid | |
| `on_child_exit` | status | |
| `on_key_press` | ev, keysym, str | `str` is what would be sent to the program |
| `on_key_release` | ev, keysym | |
| `on_button_press` / `on_button_release` | ev | |
| `on_motion_notify` | ev | enables pointer motion events without pressed buttons |
| `on_focus_in` / `on_focus_out` | | |
| `on_line_update` | row | a changed logical line (starting at row, possibly wrapped) is about to be shown, the place for `nrxvt.api.rend` |
| `on_refresh_begin` / `on_refresh_end` | | around every redraw, `nrxvt.api.highlight` works in `on_refresh_begin` |
| `on_view_change` | view_start | the view was scrolled |
| `on_screen_change` | screen | after switching screens: 0 for primary, 1 for alternate; also fires when a hard reset returns to primary. Repeated requests for the same screen do not fire it. This is a notification and cannot cancel the switch. |
| `on_scroll_back` | lines, top_row | lines were moved into the scrollback |
| `on_add_lines` | text | text output by the program, return true to drop it |
| `on_tt_write` | data | data sent to the program, return true to drop it |
| `on_tt_paste` | data | data pasted into the program, return true to drop it |
| `on_osc_seq` | op, args | operating system commands, return true to consume |
| `on_bell` | | |
| `on_size_change` | width, height | in pixels |
| `on_sel_make` / `on_sel_grab` | time | a selection was made / is about to be claimed |
| `on_sel_extend` | | a selection is extended by clicks, return true to suppress it |
| `on_configure_notify` / `on_map_notify` / `on_unmap_notify` / `on_property_notify` / `on_client_message` / `on_x_event` | ev | |

## Terminal functions

| function | description |
|---|---|
| `nrxvt.api.nrow (t)`, `nrxvt.api.ncol (t)` | screen size in characters |
| `nrxvt.api.top_row (t)` | row of the oldest scrollback line |
| `nrxvt.api.view_start (t [, row])` | first row in the window, scrolls there first if `row` is given |
| `nrxvt.api.current_screen (t)` | 0 for the primary screen, 1 for the alternate screen; reads urxvt's existing state directly, without polling or system calls |
| `nrxvt.api.prompt_marker (t, row [, marked])` | whether a row has a prompt marker; pass a boolean to set or clear it |
| `nrxvt.api.prompt_markers (t)` | table of marked row numbers, oldest first, including visible rows |
| `nrxvt.api.prompt_jump (t, direction)` | jumps to the previous marker for negative directions, the next for positive directions, or does nothing for zero; returns `view_start`. After the newest marker it returns to the live screen. Navigation does nothing on the alternate screen. |
| `nrxvt.api.cursor (t [, row, col])` | cursor `row, col`, moves the cursor first if `row` and `col` are given. Rows in the scrollback are allowed (like perl's `screen_cur`), pause the program with `nrxvt.api.pty_events` while the cursor is there, as its output would go there too |
| `nrxvt.api.focus (t)` | whether the window has the focus |
| `nrxvt.api.font_size (t)` | `width, height` of a character cell in pixels |
| `nrxvt.api.window (t)` | X window id |
| `nrxvt.api.window_geometry (t)` | `x, y, width, height, monitor_x, monitor_y, monitor_width, monitor_height` in root pixels, or no values before the window exists. The frame includes WM decorations from `_NET_FRAME_EXTENTS` (otherwise the X border). The monitor is the active RandR CRTC with the greatest frame overlap, falling back to the X screen. This queries X; use it in window-event hooks rather than refresh hooks. |
| `nrxvt.api.line (t, row)` | `text, wrapped`, where `wrapped` tells whether the line continues on the next row |
| `nrxvt.api.col (t, row, byte)` | column of the character at a 1-based byte offset of `nrxvt.api.line (t, row)` |
| `nrxvt.api.line_bounds (t, row)` | `beg, end, len` of the logical line that `row` is part of (it may be wrapped over several rows), `len` in cells. Offsets into it are `(row - beg) * nrxvt.api.ncol (t) + col`, like perl's line objects |
| `nrxvt.api.cells (t, row)` | table with the UTF-8 text of each cell of a row, column `col` at index `col + 1`, `""` for the second column of wide characters. Rows that continue on the next row have `nrxvt.api.ncol (t)` cells, so the cells of the rows `beg` to `end` together are the `len` cells of the logical line |
| `nrxvt.api.rend (t, row, c0, c1, and_mask, or_mask)` | changes the rendition of columns `[c0, c1)` until the line changes again |
| `nrxvt.api.highlight (t, row, c0, c1, and_mask, or_mask)` | like `nrxvt.api.rend`, but only for the current redraw (only allowed in `on_refresh_begin`) |
| `nrxvt.api.xor_span (t, r0, c0, r1, c1 [, mask])` | XORs `mask` (default `nrxvt.api.RS_RVid`) into the renditions from `(r0, c0)` to `(r1, c1)` (exclusive), the rows in between completely, like perl's `scr_xor_span`. Like `nrxvt.api.highlight`, only allowed in `on_refresh_begin` and undone after the redraw |
| `nrxvt.api.xor_rect (t, r0, c0, r1, c1 [, mask1 [, mask2]])` | the same for the columns `[c0, c1)` of the rows `r0` to `r1`, `mask2` (default `RS_RVid + RS_Uline`) goes to the trailing blanks but the first, like rectangular selections and perl's `scr_xor_rect` |
| `nrxvt.api.overlay (t, x, y, w, h)` | opens the overlay box (there is one per terminal), negative `x`/`y` count from the right/bottom, the border adds one cell on each side |
| `nrxvt.api.overlay_text (t, x, y, text [, and_mask, or_mask])` | writes into the overlay |
| `nrxvt.api.overlay_off (t)` | closes the overlay |
| `nrxvt.api.new_overlay (t, x, y, w, h [, border [, and_mask, or_mask]])` | a new overlay object, see below |
| `nrxvt.api.new_bar (t, options)` | reserves an edge strip and returns a retained bar object, see below |
| `nrxvt.api.write (t, data)` | sends data to the program, as if typed |
| `nrxvt.api.feed (t, data)` | processes data as if the program had output it |
| `nrxvt.api.refresh (t)` | schedules a redraw, needed after changes outside of hooks, e.g. in timers |
| `nrxvt.api.pty_events (t [, events])` | the events (`nrxvt.api.EV_READ`, `nrxvt.api.EV_WRITE`) urxvt waits for on the pty, changed first if given. `nrxvt.api.EV_NONE` pauses the program output until the returned old value is restored. Writes to the program that can't be sent at once (keys that are not consumed, `nrxvt.api.write`) resume it, consume them with `on_tt_write` while paused |
| `nrxvt.api.selection (t [, clipboard])` | the PRIMARY (or CLIPBOARD) selection text, or nil |
| `nrxvt.api.set_selection (t, text [, clipboard])` | sets and claims the selection |
| `nrxvt.api.selection_beg (t [, row, col])`, `nrxvt.api.selection_end (t, ...)`, `nrxvt.api.selection_mark (t, ...)` | the start, the (exclusive) end and the mark of the selection, set first if given. Like in perl, an end column 0 means the end of the previous row, and columns beyond the text of the row the whole row including its newline |
| `nrxvt.api.selection_make (t [, rect [, time]])` | selects the text between `selection_beg` and `selection_end` (a rectangle if `rect` is true) like the mouse does, and claims PRIMARY |
| `nrxvt.api.selection_grab (t [, clipboard [, time]])` | claims PRIMARY (or CLIPBOARD) with the current text, returns whether that worked |
| `nrxvt.api.selection_clear (t [, clipboard])` | forgets the PRIMARY text and removes the selection highlight (or forgets the CLIPBOARD text) |
| `nrxvt.api.selection_request (t [, clipboard [, time]])` | pastes PRIMARY (or CLIPBOARD) into the program |
| `nrxvt.api.selection_fetch (t, clipboard, fn [, time])` | requests PRIMARY (or CLIPBOARD) and calls `fn (text)` with it instead of pasting it, or `fn (nil)` if there is none. `fn` runs once the owner has answered, which may be right away. Returns `false` without requesting anything while another fetch of the terminal is pending. Pass the key or button event's `ev.time` as `time` where there is one. |
| `nrxvt.api.resource (t, name)` | the X resource `NRxvt.<name>` by default, or nil |
| `nrxvt.api.resource_boolean (t, name)` | the X resource as a boolean: true for `true`, `yes`, `on` and `1` (in any case), false otherwise, nil if it is not set |

The default X resource name and class are `nrxvt` and `NRxvt`. Use `NRxvt.<name>` for shared resource settings, or `nrxvt.<name>` for an instance named `nrxvt`. Builds may override these defaults with the Meson `res-name` and `res-class` options.

The optional `time` arguments are X event times, e.g. `ev.time` of a key
event, and default to `CurrentTime`.

### Overlays

`nrxvt.api.overlay` shows a single box with a border, like urxvt's own popups.
`nrxvt.api.new_overlay` creates overlay objects like the perl `overlay` method: any
number of them, with or without border, and with their own colors. They
start hidden, `x`/`y` are window columns/rows, negative ones count from the
right/bottom (`-1, -1` is the bottom right corner), and the position follows
size changes. The border takes another cell on each side.

| method | description |
|---|---|
| `ov:set (x, y, text [, and_mask, or_mask])` | writes text at a cell inside the border, the masks change the default rendition of the overlay, without them the cells keep their rendition |
| `ov:show ()` | shows it on top of the other overlays |
| `ov:hide ()` | |
| `ov:close ()` | hides it and frees it, it can't be used afterwards |

Shown overlays stay visible until they are hidden or closed, even without
references. Hidden ones are freed when they are garbage collected, and all
of them when the terminal goes away.

```lua
-- a one line status box in the bottom right corner, without border
local status = nrxvt.api.new_overlay (t, -1, -1, 12, 1, false)
status:set (0, 0, "-- SELECT --")
status:show ()
```

### Bars

Bars reserve pixels outside the text viewport. Create them in `on_init` so their reservations are included in initial character geometry. Runtime creation or closure keeps the current outer window size and adjusts text rows/columns as necessary on the next scheduled frame. Hidden bars keep their reservation and show terminal background. Bars remain registered even if their Lua references are garbage collected; `close` explicitly releases the reservation. All bars close when their terminal is destroyed.

```lua
nrxvt.api.on_init(function (t)
  -- Horizontal bars own corners; nested parts inherit the bar's flow axis.
  t.status_bar = nrxvt.api.new_bar(t, {
    edge = "bottom", thickness = 24, priority = 0,
    on_event = function(term, bar, ev)
      if ev.type == "button_press" and ev.button == 1 then
        nrxvt.api.warn("clicked " .. (ev.id or "the bar background"))
      end
    end,
  })
  t.status_bar:submit {
    bg = "background",
    children = {
      { id = "mode", size = { px = 80 }, text = "NORMAL" },
      { id = "title", size = { ratio = 3 }, text = "Terminal" },
      { id = "position", size = { ratio = 1 }, text = "Ready" },
    },
  }
end)
```

`edge`, `thickness`, and `priority` are required and fixed for the bar's lifetime. Edges are `"top"`, `"bottom"`, `"left"`, and `"right"`. Thickness is an integer from 1 through 32767 pixels; total reservations must fit X11 geometry limits. Priority is a signed 32-bit integer: smaller numbers are closer to the outside, with creation order breaking ties. `visible` is an optional boolean, defaulting to true; `on_event` is an optional function. Top/bottom bars span the client width and own the corners. Left/right bars occupy the height between them. The independent native scrollbar, if enabled, occupies its own gutter between the corresponding side's bars and the text viewport.

The root of a submitted tree covers the bar. Each child requires a unique nonempty string `id` and `size = { px = n }` or `size = { ratio = r }`. Arrays determine order. Children may have nested `children` arrays; all descendants flow left-to-right in horizontal bars and top-to-bottom in vertical bars. Pixel sizes are nonnegative integers through 32767. Ratios are finite nonnegative numbers. Allocate pixels first, then divide the remainder by ratio. For example, in a 100px container, `4px`, `ratio=1`, and `ratio=3` allocate 4px, 24px, and 72px. Oversized pixel allocations retain their requested lengths and clip at their ancestors. Zero total ratio weight leaves the remainder as container background.

Nodes and the root accept `bg`, `fg`, and `text`. Colors are `"foreground"`, `"background"`, or terminal palette indices. Defaults are terminal background for rectangles and foreground for text. Text is single-line UTF-8, horizontally and vertically centered, unrotated, clipped, and rendered with terminal fonts. Parents paint before their children. No padding, wrapping, or images are provided. Text is limited to 65535 UTF-8 bytes per node; nesting is limited to 64 levels. Unknown fields, invalid numbers/text/colors, duplicate IDs, mixed sizing units, and sparse child arrays raise errors without changing the accepted scene.

`submit` always describes a complete replacement tree: omitted nodes disappear, omitted optional fields reset to defaults, and stable IDs preserve identity through reordering/reparenting. Mutating a table after submission has no effect until the next submission. C++ retains its own values and diffs each full tree in O(N); only differences schedule layout/paint. Reuse tables and avoid submitting when your relevant state has not changed. Drawing is deferred and coalesced by the existing refresh scheduler. A submission in `on_refresh_begin` appears in the same presentation. A bar-only update does not redraw terminal cells.

| Method | Description |
|---|---|
| `bar:submit(tree)` | Accept a complete tree and schedule incremental work. |
| `bar:visible([value])` | Return visibility, changing it first if a boolean is supplied; keep reserved space. |
| `bar:geometry()` | Return cached `x, y, width, height` in terminal client pixels, without an X query. Runtime reservation changes are reflected after the deferred geometry pass. |
| `bar:capture(ev)` | In this bar's button-press callback, capture the pointer for the press; return success. Wheel buttons cannot capture. |
| `bar:release()` | Release this bar's pointer capture. |
| `bar:close()` | Release capture, reservation, and resources. Repeated close is harmless. Other methods fail after closure. |

`on_event(t, bar, ev)` receives `button_press`, `button_release`, `motion`, `resize`, and `cancel`. Pointer events contain `id` (deepest hit part, or nil for root), bar-local `x, y`, part-local `local_x, local_y`, `x_root, y_root`, `state`, `time`, `shift`, `ctrl`, `meta`, and `button` for button events. Coordinates are pixels and have no terminal row/column interpretation. Events also provide bar `width` and `height`; cancellation provides `reason`. Event tables can be retained. Bar events never fall through to terminal selection, native scrollbar handling, text-window hooks, or application mouse reporting, regardless of the callback return value.

During capture, motion/release target the pressed part and retain out-of-bounds coordinates outside the window. A matching button release automatically ends capture after the callback; other buttons do not. Hiding/closing the bar, removing the captured node, unmapping/destroying the terminal, or a handler error cancels capture. A removed ID recreated later has a new identity. A failing callback is reported with a traceback and disabled. Callbacks may submit, hide, or close their own bar.

To enable the example scrollbar, copy or link `scrollbar.lua` into the plugin directory. Configure its thickness, minimum handle height, wheel step, and priority using the constants at the top. It uses terminal foreground/background, hides on the alternate screen or empty history while keeping its strip, and supports left-button handle dragging, page clicks, and three-line wheel steps while hovered. It never enables, disables, or uses the native scrollbar implementation; both can be active.

### Detecting alternate-screen applications

For a shortcut that should only run on the primary screen, query the screen
when the key is pressed:

```lua
nrxvt.api.bind ("C-S-K", function (t)
  -- Leave the key to the application while the alternate screen is active.
  if nrxvt.api.current_screen (t) == 1 then return false end
  nrxvt.api.prompt_jump (t, -1)
end)
```

Plugins that need to react as soon as the screen changes can implement
`on_screen_change (t, screen)` instead. No timer, process inspection, or shell
integration is needed. The hook runs after urxvt has updated its screen state,
so `nrxvt.api.current_screen (t)` already returns the new value.

This reliably reports primary versus alternate screen, not whether an arbitrary
program is a fullscreen TUI. Applications can draw a TUI on the primary screen,
and multiplexers such as tmux can keep the outer terminal on the alternate
screen even when an inner pane shows a shell prompt. Disabling secondary-screen
storage does not change the reported mode; the value follows urxvt's screen
mode requests.

### Prompt markers

Prompt markers are detected on Return after nonempty commands with conventional
`$ `, `# `, `% ` or `> ` prompts. Ctrl+PageUp and Ctrl+PageDown navigate them.
Markers follow their lines through scrollback and resizing; fully clearing a
line removes its marker. Commands without intervening output share the latest
marker. Detection skips the alternate screen. Plugins can mark other prompt
formats explicitly, for example from `on_osc_seq`, and use the same navigation:

```lua
-- Add alternative shortcuts for the built-in prompt navigation.
nrxvt.api.bind ("M-Up", function (t) nrxvt.api.prompt_jump (t, -1) end)
nrxvt.api.bind ("M-Down", function (t) nrxvt.api.prompt_jump (t, 1) end)
```

Inside a hook, `nrxvt.api.prompt_marker (t, (nrxvt.api.cursor (t)), true)` marks the cursor row.

The marker functions use the same row coordinates as `nrxvt.api.line`. Stored row numbers
can change or expire when output scrolls or the terminal resizes, so query them
again when needed. Markers are separate from cell renditions, so highlighting
and styling plugins do not overwrite them.

## The nrxvt.api namespace

| function | description |
|---|---|
| `nrxvt.api.on_*(fn)` | appends `fn (t, ...)` to the corresponding hook listed above, e.g. `nrxvt.api.on_start (fn)`; registration is limited to startup |
| `nrxvt.api.bind (key, fn)` | calls `fn (t)` when `key` is pressed, using the keysym resource syntax (e.g. `"M-s"`, `"C-S-Up"`). The key is consumed unless `fn` returns `false`. As with keysym resources, extra modifiers are ignored. |
| `nrxvt.api.style {fg=, bg=, bold=, italic=, underline=, reverse=, blink=}` | returns `and_mask, or_mask`. Colors are indices (0-15 for the basic colors) or `"default"`. Attributes set to `true` are added, set to `false` are removed, and omitted ones are left alone. |
| `nrxvt.api.timer (after, fn [, repeat])` | calls `fn ()` after `after` seconds, then every `repeat` seconds. Returns a timer with `:start ([after [, repeat]])` and `:stop ()`. Active timers don't need to be referenced, and they never keep urxvt running. |
| `nrxvt.api.spawn (argv)` | starts a detached program. `argv` is an argument table, or a string run with `/bin/sh -c`. |
| `nrxvt.api.regex (pattern [, flags])` | compiles a perl compatible regular expression, see below |
| `nrxvt.api.regex_escape (s)` | escapes `s` to match literally, like perl's `quotemeta` or `\Q...\E` |
| `nrxvt.api.keysym_name (keysym)` | e.g. `"Return"` |
| `nrxvt.api.warn (msg)` | prints to stderr |
| `nrxvt.api.version`, `nrxvt.api.plugin_dir` | |
| `nrxvt.api.EV_NONE`, `EV_READ`, `EV_WRITE` | for `nrxvt.api.pty_events` |
| `nrxvt.api.RS_Bold`, `RS_Italic`, `RS_Blink`, `RS_RVid`, `RS_Uline`, `RS_Sel` | rendition bits, e.g. for `nrxvt.api.xor_span`. urxvt draws selections with `RS_Sel + RS_RVid`: reversed, or in `highlightColor`/`highlightTextColor` instead if those are set |

The LuaJIT `ffi` and `jit` modules are available as usual.

### Regular expressions

`nrxvt.api.regex` takes the place of perl's regexes, so patterns from perl
extension resources (e.g. `URxvt.matcher.pattern.1`) generally work
unchanged. It is
backed by PCRE2, which the lua plugin system always comes with. Patterns
and subjects are UTF-8 with Unicode semantics, like perl with character
strings: `\w`, `\s`, `\b`, `[[:upper:]]`, `\p{Lu}` and caseless matching
know non-ASCII characters, and `.` matches a whole character. Invalid UTF-8
in a subject never matches but is no error.

`flags` is a string of perl modifiers: `i` (caseless), `m` (`^` and `$`
match at newlines), `s` (`.` matches newlines) and `x` (extended). Invalid
patterns and flags raise an error, wrap them in `pcall` when they come from
resources or user input. Compile patterns once and reuse them, matching
uses PCRE2's JIT.

| method | description |
|---|---|
| `re:find (s [, init])` | `start, end, captures...` of the first match at or after byte `init` (default 1, negative counts from the end), or `nil`. Like `string.find`, positions are 1-based byte offsets, inclusive. |
| `re:match (s [, init])` | the captures of the first match, or the whole match if the pattern has no groups, or `nil`, like `string.match` |
| `re:gfind (s [, init])` | iterator over all non-overlapping matches, each returning what `re:find` would. Empty matches are handled like perl's `m//g`. |

Groups that did not take part in a match are `false`. Matching raises an
error when it hits PCRE2's match limit, e.g. with catastrophic
backtracking, instead of freezing the terminal.

```lua
local URL = nrxvt.api.regex ([[\bhttps?://[^\s<>"]+]], "i")

for s, e in URL:gfind (nrxvt.api.line (t, row)) do ... end

-- a literal search, caseless unless the query contains upper case
local query = nrxvt.api.regex (nrxvt.api.regex_escape (text),
                           nrxvt.api.regex ("\\p{Lu}"):find (text) and "" or "i")
```
