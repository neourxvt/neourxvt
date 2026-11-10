-- A reserved scrollbar anchored toward the nearest horizontal monitor edge.
local api = nrxvt.api

local THICKNESS = 3
local MIN_HANDLE = 20
local WHEEL_LINES = 3
local PRIORITY = 0

local floor, min, max = math.floor, math.min, math.max

local function round(n) return floor(n + 0.5) end
local function clamp(n, low, high) return max(low, min(high, n)) end

local function sync(t)
  local s = t.lua_scrollbar
  if not s then return end
  local _, _, _, length = s.bar:geometry()
  local history, rows = max(0, -api.top_row(t)), api.nrow(t)
  local shown = api.current_screen(t) == 0 and history > 0 and length > 0
  s.bar:visible(shown)
  if not shown then
    s.grab = nil
    return
  end

  local handle = min(length, max(MIN_HANDLE, round(length * rows / (rows + history))))
  local travel = length - handle
  local start = round(travel * (clamp(api.view_start(t), -history, 0) + history) / history)
  s.history, s.rows, s.length, s.handle, s.travel, s.start = history, rows, length, handle, travel, start
  -- Complete trees are cheap here: reuse three stable nodes and submit only
  -- when their final pixel geometry changes, even while history keeps growing.
  if start ~= s.last_start or handle ~= s.last_handle or length ~= s.last_length then
    s.tree.children[1].size.px = start
    s.tree.children[2].size.px = handle
    s.tree.children[3].size.px = length - start - handle
    s.bar:submit(s.tree)
    s.last_start, s.last_handle, s.last_length = start, handle, length
  end
end

local function input(t, bar, ev)
  local s = t.lua_scrollbar
  if ev.type == "cancel" then s.grab = nil; return end
  if ev.type == "resize" then sync(t); return end
  -- Use current history and track geometry if output/resize arrived before
  -- the next scheduled frame; dragging never uses a stale scroll range.
  sync(t)
  if not bar:visible() then return end
  if ev.type == "button_press" then
    if ev.button == 4 or ev.button == 5 then
      api.view_start(t, api.view_start(t) + (ev.button == 4 and -WHEEL_LINES or WHEEL_LINES))
    elseif ev.button == 1 then
      if ev.y >= s.start and ev.y < s.start + s.handle then
        if s.travel > 0 and bar:capture(ev) then s.grab = ev.y - s.start end
      else
        api.view_start(t, api.view_start(t) + (ev.y < s.start and -s.rows or s.rows))
      end
    end
  elseif ev.type == "motion" and s.grab then
    if s.travel > 0 then
      s.grab = min(s.grab, s.handle - 1)
      local position = clamp(ev.y - s.grab, 0, s.travel)
      local row = -s.history + round(s.history * position / s.travel)
      if row ~= api.view_start(t) then api.view_start(t, row) end
    end
  elseif ev.type == "button_release" and ev.button == 1 then
    s.grab = nil
    bar:release()
  end
end

local function new_bar(t, edge)
  return api.new_bar(t, { edge = edge, thickness = THICKNESS, priority = PRIORITY,
                     visible = false, on_event = input })
end

local function anchor(t)
  local s = t.lua_scrollbar
  if not s then return end
  local x, _, width, _, monitor_x, _, monitor_width = api.window_geometry(t)
  if not x then return end
  -- Compare decorated frame centers, so i3 borders and monitor offsets do
  -- not bias placement. Centered and full-width windows keep the left bar.
  local edge = 2 * x + width > 2 * monitor_x + monitor_width and "right" or "left"
  if edge == s.edge then return end
  -- Edge reservations are immutable. Replace the bar before the deferred
  -- geometry pass, keeping the total reserved width and text grid unchanged.
  s.bar:close()
  s.edge, s.bar = edge, new_bar(t, edge)
  s.last_start, s.last_handle, s.last_length = nil, nil, nil
  sync(t)
end

api.on_init (function (t)
  local s = {
    edge = "left",
    tree = { bg = "background", children = {
      { id = "before", size = { px = 0 } },
      { id = "handle", size = { px = 0 }, bg = 8 },
      { id = "after", size = { px = 0 } },
    } },
  }
  t.lua_scrollbar = s
  s.bar = new_bar(t, s.edge)
end)

-- Placement queries run on window events, never on the output refresh path.
-- Property changes include i3 updating its frame extents after border changes.
api.on_start (anchor)
api.on_configure_notify (anchor)
api.on_map_notify (anchor)
api.on_property_notify (function (t, ev)
  if ev.window == api.window(t) then anchor(t) end
end)

-- One tuple/geometry calculation per refresh covers resets and explicit
-- scrollback clearing as well as ordinary output, without a polling timer.
api.on_start (sync)
api.on_refresh_begin (sync)
api.on_screen_change (sync)
api.on_reset (sync)
