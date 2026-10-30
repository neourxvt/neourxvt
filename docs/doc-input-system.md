# Input system

This document describes the current input handling in the Lua-enabled terminal. The terminal's own bindings and Lua plugins can consume events before they reach the child process running in the PTY. Their precedence follows the dispatch paths below; there is no configurable input-layer or priority API. Available native actions depend on build options and terminal settings. The Lua and Perl plugin interfaces are mutually exclusive.

For plugin setup and the complete Lua API, see [Lua plugins](lua-plugins.md).

## Event entry and raw hooks

X events are dispatched to the handler for their destination window. With X input-method support, `XFilterEvent` can consume an event before terminal dispatch.

In the terminal's X event handler, the bar manager first suppresses pointer motion and releases associated with presses that originated in bars, preventing them from leaking into terminal handling. For events whose destination is the text viewport, `nrxvt.api.on_x_event` then runs before the event-specific keyboard or mouse handler. A truthy return stops that event's processing. This hook does not receive events targeting a bar or the native scrollbar window.

The raw hook is therefore an earlier interception point than `on_key_press`, `on_button_press`, or `on_motion_notify`. It receives the X event rather than the translated keysym and byte string supplied to `on_key_press`.

## Keyboard presses

The ordinary key-press path is:

```text
on_x_event, if the event targets the text viewport
  -> X lookup and terminal key encoding
  -> first matching Lua bind callback
  -> on_key_press callbacks
  -> native keysym resource binding
  -> built-in terminal shortcuts
  -> on_tt_write callbacks
  -> PTY write to the child process
```

Each consuming stage stops the remaining path. This is not an unconditional pipeline: input-method filtering and ISO 14755 character-entry handling can stop a key before Lua key bindings and `on_key_press`. If a key produces no bytes and no action handles it, nothing is written to the PTY.

### Lookup, encoding, and event arguments

The terminal uses X input-method lookup or `XLookupString` to obtain a keysym and text. It prepares terminal input bytes before invoking Lua key bindings and `on_key_press`, including special-key escape sequences, keypad behavior, control transformations, and Meta encoding as applicable. The keysym passed to Lua is restored to the lookup keysym after temporary encoding transformations.

`on_key_press` receives `(t, ev, keysym, str)`. `keysym` is a numeric X11 keysym; `nrxvt.api.keysym_name(keysym)` returns its X11 name where available. `str` is the prepared byte string that would normally be sent to the child, not a separate committed-text event. It may contain escape sequences or NUL bytes, or be empty. A text-producing event can have no keysym.

Key event tables contain `type`, `window`, `send_event`, `time`, `state`, `keycode`, `shift`, `ctrl`, `meta`, and pointer coordinates. The general X event conversion also supplies `row` and `col`; these describe coordinates, not a keyboard focus target. Modifying the Lua table does not modify the underlying X event or the already prepared input bytes.

### Lua bindings

`nrxvt.api.bind(keyspec, fn)` appends a binding and calls `fn(t)` when it matches a key press. The callback does not receive the key event, keysym, or byte string.

Bindings use the native keysym resource syntax, such as `M-s`, `C-S-Up`, or `Control-Return`. The key name is resolved through X11 keysym lookup, with a hexadecimal numeric fallback. There is no Neovim-style `<C-s>` notation or multi-key sequence matching in this API.

Supported modifier names include `Shift`, `Lock`, `Control`, `Meta`, `Mod1` through `Mod5`, `NumLock`, `ISOLevel3`, and `AppKeypad`, with aliases defined in `keysym_vocabulary` in `src/xdefaults.cpp`. `M` and `A` both denote Meta.

For matching, the event's modifier state is restricted to keyboard modifier bits, then augmented with logical Meta, NumLock, and ISO Level 3 bits according to the terminal's modifier masks. An application-keypad bit is added when application-keypad mode differs from the Shift state. A binding matches when its keysym equals the event keysym and all its requested modifier bits are present. Additional modifiers are ignored.

Lua bindings are searched in registration order, without sorting by modifier specificity. Consequently, an earlier binding requesting fewer modifiers can match before a later, more specific binding when their keysyms are equal. Shift can itself change the lookup keysym, so modifier overlap alone does not guarantee that two bindings match the same event.

Only the first matching binding is invoked:

| Callback outcome | Dispatch result |
|---|---|
| Exactly the boolean `false` | Continue to `on_key_press` hooks, then native handling if no hook consumes the event. |
| Any other return value, including no return or `nil` | Consume the key. |
| Error | Report the error and consume the key. The binding remains registered. |

Returning `false` does not try a second Lua binding. Registering the same key again appends another entry rather than replacing the first one.

### Lua key hooks

If no Lua binding consumed the key, `on_key_press` callbacks run in registration order. A truthy return consumes the event and skips later callbacks and native handling. `nil` or `false` allows the next hook to run. In Lua, values such as `0` and an empty string are truthy.

This differs from binding callbacks: a binding with no return consumes the event, while a key hook with no return passes it on.

A failing hook is reported and disabled. Dispatch continues with later hooks for that event. This differs from a failing binding, which consumes the event and is not disabled by binding dispatch.

Lua bindings always precede key hooks, regardless of their relative registration times. An active plugin implementing a mode through `on_key_press` cannot intercept a key already consumed by another plugin's binding.

### Native keysym resources

When Lua leaves a key unconsumed, the native keyboard manager checks configured keysym resources. Its modifier normalization and subset matching are the same as for Lua bindings, but it sorts bindings by the number of requested modifier bits, preferring more specific matches. This is modifier specificity, not a configurable plugin priority.

Registering an identical native keysym/modifier pair replaces the existing native action. Native lookup selects one matching action; it does not run a chain of matching actions.

| Native action | Behavior |
|---|---|
| `builtin:` | Continue with built-in terminal shortcuts and ordinary input handling. |
| `builtin-string:` | Send the prepared key bytes through the user-input write path and skip built-in shortcuts. |
| `string:...` or a value without an action prefix | Send the configured bytes through the user-input write path. |
| `command:...` | Process the configured bytes as terminal output. |
| `perl:...` | Invoke the user-command hook, which Lua plugins can also implement. |
| Recognized `eval:` clipboard helpers in a build without Perl | Perform the native clipboard action. |
| Other prefixed actions | Invoke the action hook. |

Actions other than `builtin:` consume the original key even if the invoked action hook does not handle the action. They do not fall through to another binding or to built-in shortcuts.

### Built-in shortcuts

After Lua and native keysym bindings, the terminal checks its built-in shortcuts. These include prompt navigation, scrollback navigation, selection paste, clipboard copy/paste, ISO 14755 input, and printing where enabled. A shortcut can perform an action without writing the original key bytes to the child. Otherwise, nonempty prepared bytes enter the user-input write path.

## Keyboard releases

Key releases have their own `on_key_release(t, ev, keysym)` hook. `nrxvt.api.bind` only matches presses; it does not bind releases or associate a consumed press with a later release.

Release processing performs ISO 14755 handling before `on_key_release`, so that feature can consume a release first. The release keysym uses `XLookupKeysym` with Shift selecting the lookup index, rather than the full press lookup path. A consuming release hook stops subsequent native release handling, including mouse slip-wheeling cleanup where enabled. Ordinary releases do not generate child-process key input.

## Registration, scope, and competing plugins

Configuration and plugins load once per Lua interpreter: `init.lua` first, then non-hidden plugin files in alphabetical order. Hook registration appends callbacks in that order, with registration order deciding the order within each file. Hooks can only be registered during startup loading.

`bind` appends to a separate interpreter-wide list. Its implementation requires a current terminal context and accepts registration from startup code or callbacks running with that context. Registering from a terminal callback does not make the resulting binding terminal-local. Repeating that registration adds more entries.

Bindings and hooks apply to every terminal using the interpreter. Callbacks receive the terminal table `t`; plugins keep terminal-specific activation state there. There is no public binding deletion or replacement API, owner namespace, input-context stack, keyboard capture, or configurable dispatch priority.

The example search, URL selection, and keyboard-selection plugins implement active modes by checking state on `t` in key hooks and consuming keys while active. If several such states are active, the first consuming hook wins according to registration order. Lua bindings still precede all of those hooks. A binding from `init.lua` is registered before plugin bindings, but even if it returns `false`, later matching Lua bindings are not tried.

## Mouse buttons and motion in the text viewport

For text-viewport button presses, `on_button_press(t, ev)` runs before terminal selection, paste, and application mouse reporting. A truthy return stops that processing. Without consumption, the terminal handles the button according to application mouse modes and its native selection or paste behavior.

Shift or Meta on a press sets the native mouse-reporting bypass state. When that bypass is active, native handling can use terminal selection and scrolling instead of reporting the event to the child.

For text-viewport releases, `on_button_release(t, ev)` runs before release reporting and native selection/paste actions, but some native cleanup has already occurred: scrollbar state is reset and selection autoscrolling is stopped where enabled. Consuming the hook cannot undo that cleanup. Native wheel scrolling is handled on release where enabled.

Motion has a different order. When application mouse reporting is active and not bypassed, the terminal can report motion to the child and exits that motion path without invoking `on_motion_notify`. Otherwise, motion targeting the text viewport reaches `on_motion_notify(t, ev)` before native selection dragging. A truthy return suppresses that native drag handling. Registering a motion hook also enables motion events without pressed buttons when the terminal starts.

The earlier `on_x_event` hook can intercept text-viewport motion before application reporting. `on_motion_notify` cannot generally do so. A consumed press does not automatically suppress its later release or motion; ordinary text-window hooks must manage any such state themselves.

The native scrollbar has its own window and native handling. Text-viewport button and motion hooks do not act as a general event chain for scrollbar events.

## Bar pointer events

Bars have separate windows and dispatch pointer events through their configured `on_event(t, bar, ev)` callback. The event targets the deepest hit part, or the root when no child is hit. Pointer events include bar-local and part-local coordinates, root coordinates, modifiers, time, and a button for button events. They do not use terminal row/column coordinates.

Bar callback returns are ignored. Bar pointer events never fall through to text-viewport hooks, native scrollbar handling, terminal selection, or application mouse reporting, even when there is no callback or the callback does nothing. A bar's `priority` setting controls edge placement, not input-handler precedence.

During a bar's button-press callback, `bar:capture(ev)` can capture buttons 1 through 3 if the X pointer grab succeeds. Wheel buttons cannot capture. Capture routes motion and releases to the pressed part, with coordinates allowed outside the bar. Starting a new capture cancels any existing bar capture.

A matching button release ends capture after its callback; releases of other buttons do not. Explicit release, hiding or closing the bar, removing the captured part, unmapping or destroying the terminal, or a handler error ends capture. Cancellation can deliver a `cancel` event with a reason. A handler error is reported and disables that bar callback. Pointer suppression in the terminal's event entry prevents a bar-originated press from leaking orphan motion or release events into text handling.

## Bytes written to the child

The child process sees terminal input bytes, not Lua callback results or X events. Consuming a key prevents its normal path from producing those bytes, but the callback can separately write data or perform an action.

`on_tt_write(t, data)` is a later interception point in both `tt_write_user_input` and `tt_write`. A truthy return drops that write. This can affect ordinary keys, configured strings, application mouse reports, plugin writes, paste output, and terminal-generated replies, rather than only physical keyboard input.

`nrxvt.api.write(t, data)` sends supplied bytes through `tt_write`, including `on_tt_write`; it does not generate a key event, redo key encoding, or run key mappings. `nrxvt.api.feed(t, data)` instead processes supplied bytes as output from the child. Neither operation means continuing dispatch with the next input handler.

Pastes have a separate `on_tt_paste(t, data)` interception point before their output is written. Returning truthy drops the paste. Paste data is not converted into individual key presses or run through key bindings.

## Implementation references

- [X event filtering and delivery](../src/rxvttoolkit.cpp): `rxvt_display::flush_cb`.
- [Terminal input dispatch and PTY writes](../src/command.cpp): `rxvt_term::x_cb`, `key_press`, `key_release`, `button_press`, `button_release`, `mouse_report`, `tt_write_user_input`, and `tt_write`.
- [Lua registration and dispatch](../src/rxvtlua_neo.cpp): `u_on`, `u_bind`, `invoke_binding`, `rxvt_lua_interp::invoke`, `push_xevent`, `t_write`, and `t_feed`.
- [Keysym syntax](../src/xdefaults.cpp): `keysym_vocabulary`, `rxvt_term::parse_keysym`, and `bind_action`.
- [Native keysym matching and actions](../src/keyboard.cpp): `compare_priority`, `register_action`, `lookup_keysym`, and `dispatch`.
- [Bar pointer routing and capture](../src/rxvtbar_neo.cpp): `rxvt_bar_manager::x_cb`, `consume_pointer`, and the bar callback/capture helpers.
- [Paste processing](../src/screen.cpp): `rxvt_term::paste` and `tt_paste`.
