#!/usr/bin/env python3
"""Exercise Lua bars on an isolated Xvfb display, without pixel readback.

Run: python3 tests/bars.py build/src/nrxvt
Requires Xvfb, a C compiler, X11 and XTEST development libraries.
"""

import ctypes as C
import os
from pathlib import Path
import pty
import re
import select
import subprocess
import sys
import tempfile
import time
import tty


def wait_for(predicate, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.005)
    raise AssertionError("timed out waiting for bar test")


CONTROLLER = r'''
local function reply(t, s) nrxvt.api.write(t, "BAR:" .. s .. "\n") end
local function input(t, bar, ev)
  if ev.type == "resize" then return end
  reply(t, "event " .. ev.type .. " " .. (ev.id or "root") .. " " .. (ev.x or 0) .. " " .. (ev.y or 0))
  if ev.type == "button_press" and ev.button == 1 then
    if t.bar_action == "capture" then assert(bar:capture(ev))
    elseif t.bar_action == "close" then bar:close()
    elseif t.bar_action == "error" then
      assert(bar:capture(ev))
      error("intentional bar handler error")
    end
  end
end
nrxvt.api.on_init(function (t)
  t.test_bars = {}
  if os.getenv("BAR_TEST_SCROLLBAR") == "1" then return end
  local function add(name, edge, thickness, priority)
    local bar = nrxvt.api.new_bar(t, {edge=edge, thickness=thickness, priority=priority, on_event=input})
    t.test_bars[name] = bar
  end
  add("top", "top", 24, 0)
  add("bottom", "bottom", 7, 0)
  add("left_outer", "left", 5, -1)
  add("left_tie", "left", 6, -1)
  add("left_inner", "left", 8, 1)
  add("right", "right", 9, 0)
  t.test_tree = { bg="background", children={
    {id="fixed", size={px=4}},
    {id="group", size={ratio=1}, children={
      {id="nested", size={ratio=1}, bg="foreground", text="A"},
    }},
    {id="last", size={ratio=3}, bg=4, text="B"},
  }}
  t.test_bars.top:submit(t.test_tree)
end)
nrxvt.api.on_osc_seq(function (t, op, args)
  if op ~= 777 then return end
  local cmd, name = args:match("^(%S+)%s*(.*)$")
  local b = t.test_bars[name]
  if cmd == "state" then
    reply(t, "state " .. nrxvt.api.ncol(t) .. " " .. nrxvt.api.nrow(t) .. " " .. nrxvt.api.top_row(t) .. " " .. nrxvt.api.view_start(t) .. " " .. nrxvt.api.current_screen(t))
  elseif cmd == "geometry" then
    reply(t, "geometry " .. table.concat({b:geometry()}, " "))
  elseif cmd == "same" then
    t.test_bars.top:submit(t.test_tree)
    reply(t, "ok")
  elseif cmd == "text" then
    t.test_tree.children[3].text = "changed"
    t.test_bars.top:submit(t.test_tree)
    reply(t, "ok")
  elseif cmd == "batch" then
    -- Both disjoint edits must survive until the same deferred paint.
    t.test_tree.children[1].bg = "foreground"
    t.test_bars.top:submit(t.test_tree)
    t.test_tree.children[3].bg = 2
    t.test_bars.top:submit(t.test_tree)
    reply(t, "ok")
  elseif cmd == "hex" then
    -- Mixed-case RGB on a rectangle and its text. Switching variants changes
    -- only the RGB value, which must still be detected as a change.
    local last = t.test_tree.children[3]
    last.bg, last.fg = name == "a" and "#FF00ff" or "#123456", "#00ff00"
    t.test_bars.top:submit(t.test_tree)
    reply(t, "ok")
  elseif cmd == "hide" or cmd == "show" then
    b:visible(cmd == "show"); reply(t, "ok")
  elseif cmd == "close" then b:close(); b:close(); reply(t, "ok")
  elseif cmd == "new" then
    t.test_bars.dynamic = nrxvt.api.new_bar(t, {edge="left", thickness=4, priority=10})
    reply(t, "ok")
  elseif cmd == "action" then t.bar_action = name; reply(t, "ok")
  elseif cmd == "hovered" then reply(t, "hovered " .. tostring(b:hovered()))
  elseif cmd == "thickness" then
    local bar_name, value = name:match("^(%S+)%s*(%d*)$")
    b = t.test_bars[bar_name]
    if value ~= "" then b:thickness(tonumber(value)) end
    reply(t, "thickness " .. b:thickness())
  elseif cmd == "badthickness" then
    -- Every rejected value must leave the reservation unchanged.
    local before = b:thickness()
    for _, value in ipairs{0, -1, 1.5, 32768, 32767, "x", 0/0} do
      assert(not pcall(b.thickness, b, value))
    end
    assert(b:thickness() == before)
    reply(t, "ok")
  elseif cmd == "scrollbar" then
    local s=t.lua_scrollbar
    reply(t, "scrollbar " .. tostring(s.bar:visible()) .. " " .. table.concat({s.bar:geometry()}, " "))
  elseif cmd == "view" then nrxvt.api.view_start(t, tonumber(name)); reply(t, "ok")
  elseif cmd == "invalid" then
    local bad = {
      {children={{id="dup",size={px=1}}, {id="dup",size={px=2}}}},
      {children={[2]={id="hole",size={px=2}}}},
      {children={{id="negative",size={px=-1}}}},
      {children={{id="mixed",size={px=1,ratio=1}}}},
      {children={{id="nan",size={ratio=0/0}}}},
      {children={{id="utf8",size={px=1},text="\255"}}},
      {children={{id="field",size={px=1},unknown=true}}},
      {children={{id="newline",size={px=1},text="a\nb"}}},
      {children={{id="short_hex",size={px=1},bg="#12345"}}},
      {children={{id="long_hex",size={px=1},fg="#1234567"}}},
      {children={{id="bad_hex",size={px=1},bg="#12345g"}}},
    }
    for _, tree in ipairs(bad) do assert(not pcall(t.test_bars.top.submit,t.test_bars.top,tree)) end
    t.test_bars.top:submit(t.test_tree)
    reply(t, "ok")
  elseif cmd == "overflow" then
    t.test_bars.top:submit{children={
      {id="large",size={px=32767},bg="foreground"},
      {id="clipped",size={px=20}}, {id="zero",size={ratio=1}},
    }}
    reply(t, "ok")
  elseif cmd == "restore" then t.test_bars.top:submit(t.test_tree); reply(t, "ok")
  elseif cmd == "move" then
    t.test_tree.children[2].children = {}
    t.test_tree.children[3].children = {{id="nested",size={ratio=1},bg="foreground"}}
    t.test_bars.top:submit(t.test_tree); reply(t, "ok")
  elseif cmd == "benchmark" then
    local count=tonumber(name)
    local tree={children={}}
    for i=1,count do tree.children[i]={id=tostring(i),size={ratio=1}} end
    local b=t.test_bars.top
    b:submit(tree); b:submit(tree)
    local start=os.clock()
    for i=1,100 do b:submit(tree) end
    local unchanged=os.clock()-start
    start=os.clock()
    for i=1,100 do tree.children[count].bg=i%2; b:submit(tree) end
    reply(t, string.format("benchmark %d %.6f %.6f",count,unchanged,os.clock()-start))
  end
  return true
end)
'''


class X11:
    def __init__(self, name):
        self.x = C.CDLL("libX11.so.6")
        self.xt = C.CDLL("libXtst.so.6")
        self.x.XOpenDisplay.argtypes = [C.c_char_p]
        self.x.XOpenDisplay.restype = C.c_void_p
        self.d = self.x.XOpenDisplay(name.encode())
        assert self.d
        self.x.XCloseDisplay.argtypes = [C.c_void_p]
        self.x.XSync.argtypes = [C.c_void_p, C.c_int]
        self.x.XDefaultRootWindow.argtypes = [C.c_void_p]
        self.x.XDefaultRootWindow.restype = C.c_ulong
        self.root = self.x.XDefaultRootWindow(self.d)
        self.x.XGetGeometry.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong),
            C.POINTER(C.c_int), C.POINTER(C.c_int), *([C.POINTER(C.c_uint)] * 4)]
        self.x.XResizeWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_uint, C.c_uint]
        self.x.XClearArea.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_int]
        self.x.XUnmapWindow.argtypes = [C.c_void_p, C.c_ulong]
        self.x.XMapWindow.argtypes = [C.c_void_p, C.c_ulong]
        self.x.XGrabPointer.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_uint, C.c_int,
                                       C.c_int, C.c_ulong, C.c_ulong, C.c_ulong]
        self.x.XUngrabPointer.argtypes = [C.c_void_p, C.c_ulong]
        self.xt.XTestFakeMotionEvent.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_ulong]
        self.xt.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]

    def geometry(self, window):
        root, x, y = C.c_ulong(), C.c_int(), C.c_int()
        width, height, border, depth = (C.c_uint() for _ in range(4))
        assert self.x.XGetGeometry(self.d, window, C.byref(root), C.byref(x), C.byref(y),
            C.byref(width), C.byref(height), C.byref(border), C.byref(depth))
        return x.value, y.value, width.value, height.value

    def motion(self, x, y):
        self.xt.XTestFakeMotionEvent(self.d, -1, x, y, 0)
        self.x.XSync(self.d, False)

    def button(self, button, down):
        self.xt.XTestFakeButtonEvent(self.d, button, down, 0)
        self.x.XSync(self.d, False)

    def grab(self):
        result = self.x.XGrabPointer(self.d, self.root, False, 0, 1, 1, 0, 0, 0)
        if result == 0:
            self.x.XUngrabPointer(self.d, 0)
            self.x.XSync(self.d, False)
        return result


class Terminal:
    def __init__(self, binary, directory, env, x11, scrollbar=False, native=False, font="fixed"):
        self.directory = directory
        plugins = directory / "nrxvt/plugins"
        plugins.mkdir(parents=True)
        (plugins / "control.lua").write_text(CONTROLLER)
        if scrollbar:
            (plugins / "scrollbar.lua").symlink_to(Path(__file__).resolve().parent.parent / "examples/scrollbar.lua")
        self.trace = directory / "trace"
        self.errors = tempfile.TemporaryFile()
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        self.received = b""
        self.crossed = []
        self.process = subprocess.Popen([binary, "-pty-fd", str(self.master), "-geometry", "20x8",
            "-fn", font, "-sl", "1000", "-fg", "#ffffff", "-bg", "#000000",
            "-sb" if native else "+sb", "+bc",
            "-xrm", "NRxvt.internalBorder: 0", "-xrm", "NRxvt.refreshBatching: false"],
            pass_fds=(self.master,), stderr=self.errors, stdout=subprocess.DEVNULL,
            env=dict(env, XDG_CONFIG_HOME=str(directory), LD_PRELOAD=str(directory.parent / "probe.so"),
                     URXVT_BAR_TRACE=str(self.trace), BAR_TEST_SCROLLBAR="1" if scrollbar else ""))
        self.x11 = x11
        wait_for(lambda: self.trace.exists() and len(self.events("window")) >= (2 if scrollbar else 7))
        windows = self.events("window")
        self.parent, self.vt = map(int, windows[0][1:3])
        # Xvfb has no window manager; explicitly move the terminal to origin.
        self.x11.x.XMoveWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int]
        self.x11.x.XMoveWindow(self.x11.d, self.parent, 0, 0)
        self.x11.x.XSync(self.x11.d, False)
        self.command("state")
        self.send(b"\x1b[?25l")
        time.sleep(.08)

    def events(self, kind=None):
        if not self.trace.exists():
            return []
        events = [line.split() for line in self.trace.read_text().splitlines()]
        return [e for e in events if kind is None or e[0] == kind]

    def send(self, data):
        os.write(self.slave, data)

    def drain(self):
        while select.select([self.slave], [], [], 0)[0]:
            self.received += os.read(self.slave, 65536)
        # Commands clear the received buffer, so complete crossing lines are
        # moved to their own list where later checks can still see them.
        *lines, rest = self.received.split(b"\n")
        self.crossed.extend(l.decode().split() for l in lines if l.startswith((b"BAR:event enter", b"BAR:event leave")))
        self.received = b"".join(l + b"\n" for l in lines if not l.startswith((b"BAR:event enter", b"BAR:event leave"))) + rest

    def command(self, text, prefix=None):
        self.drain()
        self.received = b""
        self.send(b"\x1b]777;" + text.encode() + b"\x07")
        expected = ("BAR:" + (prefix or text.split()[0])).encode()
        if text.split()[0] not in ("state", "geometry", "scrollbar", "benchmark", "hovered", "thickness") and prefix is None:
            expected = b"BAR:ok"
        def got():
            self.drain()
            return next((line.decode() for line in self.received.splitlines() if line.startswith(expected)), None)
        return wait_for(got)

    def geometry(self, name):
        return tuple(map(int, self.command("geometry " + name).split()[1:]))

    def state(self):
        return tuple(map(int, self.command("state").split()[1:]))

    def idle(self):
        time.sleep(.06)
        self.trace.write_text("")

    def close(self, expected_error=False):
        self.process.terminate()
        self.process.wait(timeout=3)
        os.close(self.master)
        os.close(self.slave)
        self.errors.seek(0)
        errors = self.errors.read().decode()
        self.errors.close()
        assert "X Error" not in errors and "Segmentation" not in errors, errors
        if not expected_error:
            assert "lua:" not in errors, errors
        return errors


def check_scene(t):
    assert t.state()[:2] == (20, 8)
    top, bottom = t.geometry("top"), t.geometry("bottom")
    outer, tie, inner = (t.geometry(n) for n in ("left_outer", "left_tie", "left_inner"))
    right = t.geometry("right")
    assert top[:2] == (0, 0) and top[3] == 24
    assert bottom[0] == 0 and bottom[2] == top[2] and bottom[3] == 7
    assert outer[:3] == (0, 24, 5) and tie[:3] == (5, 24, 6) and inner[:3] == (11, 24, 8)
    assert outer[3] == tie[3] == inner[3] == right[3] == bottom[1] - 24
    assert right[0] + right[2] == top[2]
    before = t.x11.geometry(t.vt)
    t.command("hide left_outer"); time.sleep(.03)
    assert t.x11.geometry(t.vt) == before and t.geometry("left_outer") == outer
    t.command("show left_outer")

    for operation in ("same", "invalid"):
        t.idle(); t.command(operation); time.sleep(.04)
        assert not t.events("fill") and not t.events("copy"), (operation, t.events())

    top_window = t.bar_windows["top"]
    t.idle(); t.command("text"); time.sleep(.04)
    copies = t.events("copy")
    assert copies and all(int(e[2]) == top_window for e in copies), copies
    assert all(e[5] != str(top[2]) for e in copies), copies
    assert not t.events("sync") and not t.events("query_pointer")

    t.idle()
    t.x11.x.XClearArea(t.x11.d, top_window, 1, 2, 3, 5, True)
    t.x11.x.XSync(t.x11.d, False)
    time.sleep(.04)
    assert t.events("copy") and not t.events("fill"), "Expose repainted instead of using retained pixels"

    # A paused frame still accepts declarations and PTY queries, but neither
    # disjoint edit may be presented before synchronized output ends.
    t.send(b"\x1b[?2026h")
    t.idle(); t.command("batch"); time.sleep(.04)
    assert not t.events("copy") and not t.events("fill")
    t.send(b"\x1b[?2026l")
    wait_for(lambda: t.events("copy"))
    copies = t.events("copy")
    assert any(int(e[3]) == 0 and int(e[5]) >= 4 for e in copies), copies
    assert any(int(e[3]) > 0 for e in copies), copies
    # RGB-only edits repaint like palette edits; overflow later drops the
    # RGB nodes and restore allocates their colors again.
    for variant in ("a", "b"):
        t.idle(); t.command("hex " + variant); time.sleep(.04)
        assert t.events("copy"), (variant, t.events())
    for operation in ("overflow", "restore", "move"):
        t.command(operation); time.sleep(.03)
    assert t.process.poll() is None

    outer_size = t.x11.geometry(t.parent)[2:]
    t.command("new"); time.sleep(.04)
    assert t.x11.geometry(t.parent)[2:] == outer_size
    assert t.x11.geometry(t.vt)[0] == before[0] + 4
    t.command("close dynamic"); time.sleep(.04)
    assert t.x11.geometry(t.parent)[2:] == outer_size and t.x11.geometry(t.vt)[0] == before[0]

    # Crossing events: one enter naming the part under the pointer, one leave,
    # regardless of visibility, and queryable hover state.
    crossed = t.crossed
    def crossings():
        t.drain()
        return crossed
    t.x11.motion(700, 500); time.sleep(.03); crossings(); crossed.clear()
    t.x11.motion(6, 4); t.x11.motion(8, 5)
    wait_for(lambda: crossings())
    # "move" above emptied the group, so it is the deepest part there.
    assert crossings() == [["BAR:event", "enter", "group", "6", "4"]], crossings()
    assert t.command("hovered top").split()[1] == "true"
    t.x11.motion(700, 500)
    wait_for(lambda: len(crossings()) == 2)
    assert crossings()[1][:3] == ["BAR:event", "leave", "root"], crossings()
    assert t.command("hovered top").split()[1] == "false"
    t.command("hide top"); crossings(); crossed.clear()
    t.x11.motion(6, 4)
    wait_for(lambda: crossings())
    t.x11.motion(700, 500)
    wait_for(lambda: len(crossings()) == 2)
    t.command("show top")

    # A captured drag leaving the bar reports a single leave, even though
    # releasing the grab outside generates another crossing.
    t.command("action capture"); crossings(); crossed.clear()
    t.x11.motion(6, 4); t.x11.button(1, True); time.sleep(.02)
    t.x11.motion(700, 500); time.sleep(.02)
    t.x11.button(1, False); time.sleep(.05)
    assert [c[1] for c in crossings()] == ["enter", "leave"], crossings()
    t.command("action none")

    # Runtime thickness keeps the outer window and moves the text area; a
    # bar growing under a resting pointer reports the crossing.
    vt = t.x11.geometry(t.vt)
    assert t.command("thickness top").split()[1] == "24"
    t.command("badthickness top")
    t.x11.motion(60, vt[1] + 3); time.sleep(.03); crossings(); crossed.clear()
    assert t.command("thickness top 30").split()[1] == "30"
    wait_for(lambda: t.geometry("top")[3] == 30)
    wait_for(lambda: t.x11.geometry(t.vt)[1] == vt[1] + 6)
    assert t.x11.geometry(t.parent)[2:] == outer_size
    assert t.geometry("left_outer")[1] == 30 and t.geometry("right")[1] == 30
    wait_for(lambda: [c[1] for c in crossings()] == ["enter"])
    t.command("thickness top 24")
    wait_for(lambda: t.x11.geometry(t.vt) == vt)
    wait_for(lambda: [c[1] for c in crossings()] == ["enter", "leave"])
    assert t.geometry("top")[3] == 24 and t.state()[:2] == (20, 8)
    t.x11.motion(700, 500); time.sleep(.02)

    # Hit testing must follow nested ratio boundaries, independent of font.
    width = top[2]
    boundary = 4 + int((width - 4) / 4 + .5)
    t.command("action capture")
    t.x11.motion(6, 4); t.x11.button(1, True); time.sleep(.02)
    t.x11.motion(width + 50, 400); time.sleep(.02)
    t.x11.button(1, False); time.sleep(.02)
    t.drain()
    assert b"event motion" in t.received and b"event button_release" in t.received, t.received
    assert str(width + 50).encode() in t.received, t.received
    t.command("action capture")
    t.x11.motion(boundary + 2, 4); t.x11.button(1, True); time.sleep(.02)
    assert t.x11.grab() == 1, "drag did not capture the pointer"
    t.command("hide top"); time.sleep(.02)
    assert t.x11.grab() == 0, "hiding leaked the pointer grab"
    t.x11.button(1, False)
    assert t.process.poll() is None
    t.command("show top")

    # A bounded CPU sample includes Lua conversion and native full-tree diff.
    # It deliberately excludes presentation; request checks cover that path.
    for count in (24, 128, 1024):
        print(t.command(f"benchmark {count}"))
    t.command("restore")
    t.command("action error")
    t.x11.motion(6, 4); t.x11.button(1, True); time.sleep(.03)
    assert t.x11.grab() == 0, "callback error leaked the pointer grab"
    t.x11.button(1, False)
    t.command("action close")
    t.x11.motion(2, 30); t.x11.button(1, True); t.x11.button(1, False)
    time.sleep(.04)
    assert t.x11.geometry(t.vt)[0] == before[0] - 5

    # Force a client smaller than all reservations; no zero-size X objects or
    # unsigned geometry underflow may result, even without a window manager.
    t.x11.x.XResizeWindow(t.x11.d, t.parent, 2, 2)
    t.x11.x.XSync(t.x11.d, False)
    time.sleep(.04)
    assert t.process.poll() is None


def check_scrollbar(t):
    # Earlier terminals leave the pointer where this bar would grow under it.
    t.x11.motion(700, 500)
    assert t.command("scrollbar").split()[1] == "false"
    initial = t.x11.geometry(t.vt)
    window = next(int(e[2]) for e in t.events("window") if int(e[5]) == 3)
    pixmap = next(int(e[2]) for e in t.events("pixmap") if int(e[1]) == window)
    def handles():
        colors, fills = {}, []
        for e in t.events():
            if e[0] == "foreground":
                colors[e[1]] = int(e[2])
            # The handle is the only fill differing from the black background.
            elif e[0] == "fill" and int(e[1]) == pixmap and colors.get(e[6], 0) != 0:
                fills.append(tuple(map(int, e[2:6])))
        return fills
    t.send(b"line\r\n" * 12)
    wait_for(lambda: t.state()[2] < 0)
    time.sleep(.04)
    _, _, oldest, view, _ = t.state()
    length = initial[3]
    handle = min(length, max(20, int(length * 8 / (8 - oldest) + .5)))
    assert handles()[-1] == (0, length - handle, 3, handle), handles()
    assert handle > 20
    t.send(b"line\r\n" * 40)
    wait_for(lambda: t.state()[2] < -20)
    assert t.command("scrollbar").split()[1] == "true"
    _, y, width, length = map(int, t.command("scrollbar").split()[2:])
    assert width == 3 and length > 20
    time.sleep(.04)
    assert handles()[-1][3] == 20
    t.idle(); t.send(b"more\r\n"); time.sleep(.04)
    assert not any(int(e[2]) == window for e in t.events("copy")), "unchanged handle repainted"
    # Hovering widens the bar in 1px steps over the configured duration and
    # moves the text area; leaving shrinks it the same way and restores both.
    source = (Path(__file__).resolve().parent.parent / "examples/scrollbar.lua").read_text()
    hover = int(re.search(r"^local HOVER_THICKNESS = (\d+)", source, re.M)[1])
    duration = float(re.search(r"^local HOVER_DURATION = ([\d.]+)", source, re.M)[1])
    def width():
        return int(t.command("scrollbar").split()[4])
    def animate(target):
        start, seen = time.monotonic(), [width()]
        while seen[-1] != target:
            assert time.monotonic() - start < duration + 2, seen
            current = width()
            if current != seen[-1]:
                seen.append(current)
            time.sleep(.01)
        elapsed = time.monotonic() - start
        step = 1 if target > seen[0] else -1
        assert seen == list(range(seen[0], target + step, step)), seen
        # Generous lower bound: the steps must be spread out, not immediate.
        assert elapsed > duration * .6, (elapsed, seen)
    t.x11.motion(2, y + 10)
    animate(hover)
    wait_for(lambda: t.x11.geometry(t.vt)[0] == initial[0] + hover - 3)
    t.x11.motion(100, y + 10)
    animate(3)
    wait_for(lambda: t.x11.geometry(t.vt) == initial)
    # Leaving midway reverses from the current thickness without first
    # reaching the hover thickness.
    t.x11.motion(2, y + 10)
    wait_for(lambda: width() > 3)
    t.x11.motion(100, y + 10)
    widest = 3
    def shrunk():
        nonlocal widest
        widest = max(widest, width())
        return widest > 3 and width() == 3
    wait_for(shrunk)
    assert widest < hover, widest
    wait_for(lambda: t.x11.geometry(t.vt) == initial)
    t.send(b"\x1b[?1003h\x1b[?1006h")
    t.x11.motion(2, y + 2)
    t.x11.button(4, True); t.x11.button(4, False)
    wait_for(lambda: t.state()[3] == -3)
    t.x11.button(1, True); t.x11.button(1, False)
    wait_for(lambda: t.state()[3] == -11)
    t.command("view 0")
    t.x11.motion(2, y + length - 2); t.x11.button(1, True)
    t.x11.motion(60, y - 30)
    wait_for(lambda: t.state()[3] == t.state()[2])
    t.x11.motion(60, y + length + 50)
    wait_for(lambda: t.state()[3] == 0)
    # A drag keeps the bar wide outside it until the button is released.
    wait_for(lambda: width() == hover)
    t.x11.button(1, False)
    wait_for(lambda: width() == 3)
    t.drain()
    assert b"\x1b[<" not in t.received, t.received
    t.send(b"\x1b[?1049h")
    wait_for(lambda: t.command("scrollbar").split()[1] == "false")
    assert t.x11.geometry(t.vt) == initial
    t.send(b"\x1b[?1049l")
    wait_for(lambda: t.command("scrollbar").split()[1] == "true")
    assert t.x11.geometry(t.vt) == initial
    # urxvt clears saved lines through OSC 720 with a zero argument.
    t.send(b"\x1b]720;0\x07")
    wait_for(lambda: t.command("scrollbar").split()[1] == "false")
    assert t.x11.geometry(t.vt) == initial


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="urxvt-bars-") as name:
        directory = Path(name)
        subprocess.run(["cc", "-shared", "-fPIC", "-o", str(directory / "probe.so"),
                        str(Path(__file__).with_name("bar-probe.c")), "-ldl", "-lX11"], check=True)
        errors = tempfile.TemporaryFile()
        read_fd, write_fd = os.pipe()
        server = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "800x600x24", "-nolisten", "tcp"],
                                  pass_fds=(write_fd,), stderr=errors)
        os.close(write_fd)
        try:
            if not select.select([read_fd], [], [], 5)[0]:
                errors.seek(0)
                raise RuntimeError("Xvfb did not start: " + errors.read().decode()[-2000:])
            display = ":" + os.read(read_fd, 128).decode().strip()
            x11 = X11(display)
            env = dict(os.environ, DISPLAY=display)
            try:
                for i, (scrollbar, native, font) in enumerate(((False, False, "fixed"), (False, True, "xft:monospace"),
                                                               (True, False, "fixed"), (True, True, "fixed"))):
                    t = Terminal(binary, directory / str(i), env, x11, scrollbar, native, font)
                    try:
                        if scrollbar:
                            check_scrollbar(t)
                        else:
                            windows = t.events("window")
                            t.bar_windows = {name: next(int(e[2]) for e in windows if tuple(map(int, e[3:])) == t.geometry(name))
                                             for name in ("top", "bottom", "left_outer", "left_tie", "left_inner", "right")}
                            check_scene(t)
                    finally:
                        terminal_errors = t.close(expected_error=not scrollbar)
                        if not scrollbar:
                            assert terminal_errors.count("lua: bar event:") == 1, terminal_errors
                    print(f"PASS: {'scrollbar' if scrollbar else 'scene/layout/damage/input'}; native={native}; font={font}")
            finally:
                x11.x.XCloseDisplay(x11.d)
        finally:
            server.terminate()
            server.wait(timeout=3)
            os.close(read_fd)
            errors.close()


if __name__ == "__main__":
    main()
