-- keyboard-select.lua - select and copy text with vi-like keys
--
-- A port of the keyboard-select perl extension
--   Author:  Bert Muennich
--   Website: http://www.github.com/muennich/urxvt-perls
--   License: GPLv2
--
-- ACTIVATE_KEY (M-Escape) starts the selection mode at the cursor, or if the
-- view is scrolled back at its first row (at the top of the scrollback) or
-- in its middle. The program is paused until it ends. SEARCH_KEY (unset by
-- default) starts it with a backward search. Then:
--
--   h/j/k/l, arrows    move the cursor left/down/up/right
--   0 ^ $ Home End     start, first non-blank, end of the line
--   w W b B e E        word motions
--   f F ; ,            jump to a character, repeat forwards/backwards
--   Tab                jump to the position where the mode was left the last
--                      time (it follows its text as lines are added to the
--                      scrollback)
--   g G H M L          top/bottom of the scrollback, top/middle/bottom of
--                      the window
--   C-f C-b C-d C-u    scroll a page/half a page down/up
--                      The cursor stays in the window, SCROLLOFF rows away
--                      from its edges, also when scrolling with the mouse
--   / ?                search forward/backward (case-insensitive unless the
--                      text contains upper case letters), n/N repeat,
--                      C-S-v in the search prompt pastes the clipboard
--                      In the prompt, C-a C-e (Home End) move the cursor
--                      to the start/end, Left Right by a character, C-u
--                      clears the text before the cursor, C-w removes the
--                      word before it
--   P p                search forward for the text of PRIMARY/the CLIPBOARD,
--                      as if pasted into the prompt of /
--   v V C-v            toggle normal/linewise/blockwise selection
--   o O                while selecting: move the cursor to the other end of
--                      the selection, O only to the other side in blockwise
--                      mode
--   iw aw iW aW        while selecting: select the word (or WORD) around the
--                      cursor, without/with the whitespace next to it,
--                      repeating extends the selection by the next word
--   y Return           copy the selection, Return quits afterwards
--   Y                  copy the selected lines, or the cursor line and quit
--   Escape             end the selection (v V C-v), or quit if there is none
--   q C-c              quit
--
-- Where the text goes is set by YANK_TARGET. With "clipboard", selecting text
-- puts it into PRIMARY right away, and y/Return also into the CLIPBOARD.
-- With "primary", y/Return only copy it to PRIMARY, unless the
-- NRxvt.keyboard-select.clipboard resource is true (which makes it copy to
-- the CLIPBOARD as well). Keysym resources of the perl extension keep
-- working, e.g.
--
--   NRxvt.keysym.M-Escape: perl:keyboard-select:activate
--   NRxvt.keysym.M-s:      perl:keyboard-select:search
--
-- Copy or link this file into ~/.config/nrxvt/plugins to enable it.

local api = nrxvt.api

local ACTIVATE_KEY = "M-Escape"
-- e.g. "M-s", which search.lua uses as well
local SEARCH_KEY = nil
-- like vim's scrolloff: the number of rows kept between the cursor and the
-- top/bottom of the window, as long as there is more scrollback to show
local SCROLLOFF = 3
-- "clipboard": the selected text is put into PRIMARY while selecting (like
-- selecting with the mouse does) and y copies it to the CLIPBOARD (and
-- PRIMARY); "primary": y copies it to PRIMARY only
local YANK_TARGET = "clipboard"

-- character classes for the word motions, like \s, \w and the rest in the
-- regular expressions of the perl version
local SPACE, WORD, PUNCT = 0, 1, 2
local SPACE_RE, WORD_RE = api.regex ("^\\s"), api.regex ("^\\w")
-- search texts with upper case letters are case-sensitive
local UPPER_RE = api.regex ("[[:upper:]]")

-- the cursor of the search prompt is drawn reversed, the text isn't
local PROMPT_TEXT = { api.style { reverse = false } }
local PROMPT_CURSOR = { api.style { reverse = true } }

-- the keys of move_cursor, those in SCROLL_KEYS only with Control
local CURSOR_KEYS = { g = true, G = true, ["0"] = true, ["^"] = true, ["$"] = true, H = true, M = true, L = true }
local SCROLL_KEYS = { f = true, b = true, d = true, u = true }
local MOVE_TO_KEYS = { [";"] = true, [","] = true, w = true, W = true, b = true, B = true, e = true, E = true }

-- one UTF-8 encoded character, as pattern
local UTF8_CHAR = "[%z\1-\127\194-\244][\128-\191]*"

-- number of characters of an UTF-8 string
local function length (s)
  return select (2, s:gsub ("[^\128-\191]", ""))
end

-- logical lines, like the line objects of perl

local Line = {}
Line.__index = Line

-- the logical line that row is part of
local function line (t, row)
  local beg, last, len = api.line_bounds (t, row)
  return setmetatable ({ t = t, beg = beg, last = last, l = len, ncol = api.ncol (t) }, Line)
end

function Line:offset_of (row, col)
  return (row - self.beg) * self.ncol + col
end

function Line:coord_of (offset)
  -- the offset of an empty line's last cell is -1, perl's integer division
  -- maps it to its first row, and the cursor clamps the column
  if offset < 0 then return self.beg, 0 end
  return self.beg + math.floor (offset / self.ncol), offset % self.ncol
end

-- the text of the cells, one entry per cell
function Line:cells ()
  if not self.cell_list then
    local cells = {}
    for row = self.beg, self.last do
      for _, cell in ipairs (api.cells (self.t, row)) do
        cells[#cells + 1] = cell
      end
    end
    self.cell_list = cells
  end
  return self.cell_list
end

-- the character classes of the cells, the second cell of wide characters
-- belongs to the character
function Line:classes ()
  if not self.class_list then
    local classes = {}
    for i, cell in ipairs (self:cells ()) do
      if cell == "" then
        classes[i] = classes[i - 1] or SPACE
      elseif SPACE_RE:find (cell) then
        classes[i] = SPACE
      elseif WORD_RE:find (cell) then
        classes[i] = WORD
      else
        classes[i] = PUNCT
      end
    end
    self.class_list = classes
  end
  return self.class_list
end

-- the UTF-8 text of the line and the byte position each cell starts at
function Line:text ()
  local cells = self:cells ()
  local starts, pos = {}, 1
  for i, cell in ipairs (cells) do
    starts[i] = pos
    pos = pos + #cell
  end
  starts[#cells + 1] = pos
  return table.concat (cells), starts
end

-- the offset of the cell containing a byte position of Line:text
local function offset_at (starts, byte)
  local offset = 0
  while starts[offset + 2] and starts[offset + 2] <= byte do
    offset = offset + 1
  end
  return offset
end

-- the status area

local function status_area (t, ks, extra)
  local stat

  if ks.search ~= "" then
    -- the padding makes room for the cursor at the end of the text
    stat = ks.search .. string.rep (" ", api.ncol (t) - length (ks.search))
  else
    stat = ""
    if ks.select ~= "" then
      stat = "-V" .. (ks.select ~= "n" and ks.select:upper () or "") .. "- "
    end

    local top, view = api.top_row (t), api.view_start (t)
    if top == 0 then
      stat = stat .. "All"
    elseif view == top then
      stat = stat .. "Top"
    elseif view == 0 then
      stat = stat .. "Bot"
    else
      stat = stat .. string.format ("%2d%%", math.floor ((top - view) * 100 / top))
    end

    if extra then stat = extra .. " " .. stat end
  end

  -- a borderless one row overlay in the bottom right corner
  local len = length (stat)
  if not ks.overlay or ks.overlay_len ~= len then
    if ks.overlay then ks.overlay:close () end
    ks.overlay = api.new_overlay (t, -1, -1, len, 1, false)
    ks.overlay_len = len
  end

  ks.overlay:set (0, 0, stat, unpack (PROMPT_TEXT))
  if ks.search ~= "" then
    -- redraws the character at the cursor reversed, or a blank at the end
    local col = length (ks.search:sub (1, ks.search_cur))
    local cell = ks.search:match ("^" .. UTF8_CHAR, ks.search_cur + 1) or " "
    if col < len then ks.overlay:set (col, 0, cell, unpack (PROMPT_CURSOR)) end
  end
  ks.overlay:show ()
end

-- SCROLLOFF, but small enough to leave a row for the cursor
local function scrolloff (nrow)
  return math.min (SCROLLOFF, math.floor ((nrow - 1) / 2))
end

-- the first and last row the cursor may be at without the view having to
-- scroll, there is no margin where the view can't scroll any further
local function cursor_range (t)
  local view, nrow, top = api.view_start (t), api.nrow (t), api.top_row (t)
  local so = scrolloff (nrow)

  return view == top and view or view + so,
         view == 0 and view + nrow - 1 or view + nrow - 1 - so
end

-- keeps the cursor in the window (and SCROLLOFF rows away from its edges) by
-- scrolling the view, the perl version does this on every refresh, here it
-- is done after key presses
local function show_cursor (t)
  local row = api.cursor (t)
  local view, nrow, top = api.view_start (t), api.nrow (t), api.top_row (t)
  local so = scrolloff (nrow)
  local new = view

  if row < view + so then
    new = row - so
  elseif row > view + nrow - 1 - so then
    new = row - nrow + 1 + so
  end

  new = math.max (top, math.min (0, new))
  if new ~= view then api.view_start (t, new) end
end

-- selection

-- the selected area, an exclusive end column; in blockwise mode the rows
-- are the first rows of logical lines and the columns are offsets
local function calc_span (t, ks)
  local cr, cc = api.cursor (t)
  local br, bc, er, ec

  if ks.select == "b" then
    local cl, al = line (t, cr), line (t, ks.ar)
    br, bc = cl.beg, cl:offset_of (cr, cc)
    er, ec = al.beg, al:offset_of (ks.ar, ks.ac)
    if br > er then br, er = er, br end
    if bc > ec then bc, ec = ec, bc end
  elseif cr < ks.ar then
    br, bc, er, ec = cr, cc, ks.ar, ks.ac
  elseif cr > ks.ar then
    br, bc, er, ec = ks.ar, ks.ac, cr, cc
  else
    br, er = cr, cr
    if cc < ks.ac then bc, ec = cc, ks.ac else bc, ec = ks.ac, cc end
  end

  if ks.select == "l" then
    br, er = line (t, br).beg, line (t, er).last
    bc, ec = 0, api.ncol (t)
  else
    ec = ec + 1
  end

  return br, bc, er, ec
end

local function toggle_select (t, ks, mode)
  if ks.select == mode then
    ks.select = ""
  else
    if ks.select == "" then
      ks.ar, ks.ac = api.cursor (t)
    end
    ks.select = mode
  end

  status_area (t, ks)
end

-- an empty selection, which removes the selection highlight
local function clear_selection (t)
  api.selection_beg (t, 1, 0)
  api.selection_end (t, 1, 0)
end

-- copies the selection to PRIMARY (and CLIPBOARD)
local function copy (t, ks, time, clipboard)
  local br, bc, er, ec = calc_span (t, ks)
  if ks.select == "l" then ec = line (t, er).l end

  api.selection_beg (t, br, bc)
  api.selection_end (t, er, ec)
  api.selection_make (t, ks.select == "b", time)

  local text = api.selection (t)
  if clipboard and text then
    api.set_selection (t, text, true)
  end
end

-- puts the selected text into PRIMARY, without leaving the selection of the
-- terminal (the highlight is drawn by the mode itself)
local function sync_primary (t, ks, time)
  if not ks.live_primary or ks.select == "" then return end

  copy (t, ks, time, false)
  clear_selection (t)
end

-- cursor movement

-- puts the cursor on the logical line of row, as close to the column it
-- wants to be at (ks.offset, or the end of the line) as possible
local function put_cursor (t, ks, row)
  local ln = line (t, row)
  local offset = (ks.dollar > 0 or ks.offset >= ln.l) and ln.l - 1 or ks.offset
  api.cursor (t, ln:coord_of (offset))
end

local function move_cursor (t, ks, key)
  local cr, cc = api.cursor (t)
  local ln = line (t, cr)
  local top, nrow = api.top_row (t), api.nrow (t)

  if key == "k" and ln.beg > top then
    cr = ln.beg - 1
  elseif key == "j" and ln.last < nrow - 1 then
    cr = ln.last + 1
  elseif key == "h" and ks.offset > 0 then
    local offset = ln:offset_of (cr, cc) - 1
    -- skip the second cell of wide characters
    local cells = ln:cells ()
    while offset > 0 and cells[offset + 1] == "" do offset = offset - 1 end
    ks.offset = math.max (offset, 0)
    ks.dollar = 0
  elseif key == "l" and ks.offset < ln.l - 1 then
    local offset = ks.offset + 1
    local cells = ln:cells ()
    while offset < ln.l - 1 and cells[offset + 1] == "" do offset = offset + 1 end
    ks.offset = offset
  elseif key == "f" or key == "d" or key == "b" or key == "u" then
    local view = api.view_start (t)
    local amount = (key == "d" or key == "u") and math.floor (nrow / 2) or nrow - 1
    if key == "b" or key == "u" then amount = -amount end

    -- move the cursor as far as the window could be scrolled, but keep it
    -- out of the SCROLLOFF margins of the new view
    local new = api.view_start (t, math.max (top, math.min (0, view + amount)))
    local first, last = cursor_range (t)
    cr = math.max (first, math.min (last, cr + new - view))
  elseif key == "g" then
    cr, ks.offset, ks.dollar = top, 0, 0
  elseif key == "G" then
    cr, ks.offset, ks.dollar = nrow - 1, 0, 0
  elseif key == "0" then
    ks.offset, ks.dollar = 0, 0
  elseif key == "^" then
    local cells, offset = ln:cells (), 0
    while cells[offset + 1] == " " or cells[offset + 1] == "\t" do offset = offset + 1 end
    ks.offset, ks.dollar = offset, 0
  elseif key == "$" then
    ks.dollar = ln:offset_of (cr, cc) + 1
    ks.offset = ln.l - 1
  elseif key == "H" then
    cr = cursor_range (t)
  elseif key == "M" then
    cr = api.view_start (t) + math.floor (nrow / 2)
  elseif key == "L" then
    local _, last = cursor_range (t)
    cr = last
  end

  put_cursor (t, ks, cr)

  status_area (t, ks)
end

-- Whether a word (or WORD, if big) starts at offset i, i.e. whether the
-- characters before and at i match the perl pattern \w[^\w\s]|\W\w|\s\S
-- (\s\S for WORDs).
local function starts_word (classes, i, big)
  local a, b = classes[i], classes[i + 1]
  if big then return a == SPACE and b ~= SPACE end
  return a ~= b and b ~= SPACE
end

-- Whether a word (or WORD) ends at offset i, the perl pattern
-- [^\w\s](?=\w)|\w(?=\W)|\S(?=\s|$) (\S(?=\s|$) for WORDs).
local function ends_word (classes, i, big)
  local a, b = classes[i + 1], classes[i + 2]
  if a == SPACE then return false end
  if big then return b == nil or b == SPACE end
  return b ~= a
end

-- f/F/;/, and the word motions. Unlike the perl version, they start from
-- the cursor rather than the column remembered for j/k.
local function move_to (t, ks, key)
  local cr, cc = api.cursor (t)
  local ln = line (t, cr)
  local offset = ln:offset_of (cr, cc)
  local nrow = api.nrow (t)
  local found

  if key == ";" or key == "," then
    local char = ks.move_char
    if not char then return end

    local cells = ln:cells ()
    if ks.move_dir * (key == "," and -1 or 1) > 0 then
      for i = offset + 1, ln.l - 1 do
        if cells[i + 1] == char then found = i break end
      end
    else
      for i = offset - 1, 0, -1 do
        if cells[i + 1] == char then found = i break end
      end
    end
  elseif key == "b" or key == "B" then
    -- the last word start before the cursor, or the start of the line,
    -- or the last word of the previous lines
    while true do
      local classes = ln:classes ()
      for i = offset - 1, 1, -1 do
        if starts_word (classes, i, key == "B") then found = i break end
      end
      if found then break end

      if offset > 0 then
        found = 0
        break
      end
      if ln.beg <= api.top_row (t) then break end

      ln = line (t, ln.beg - 1)
      offset = ln.l
    end
  elseif key == "e" or key == "E" then
    -- the next word end after the cursor, in the following lines too
    offset = offset + 1
    while true do
      local classes = ln:classes ()
      for i = offset, ln.l - 1 do
        if ends_word (classes, i, key == "E") then found = i break end
      end
      if found or ln.last + 1 >= nrow then break end

      ln = line (t, ln.last + 1)
      offset = 0
    end
  else
    -- w/W: the next word start, or the start of the next line
    local classes = ln:classes ()
    for i = offset + 1, ln.l - 1 do
      if starts_word (classes, i, key == "W") then found = i break end
    end

    if not found and ln.last + 1 < nrow then
      ln = line (t, ln.last + 1)
      found = 0
    end
  end

  if found then
    ks.dollar = 0
    ks.offset = found
    api.cursor (t, ln:coord_of (found))
  end
end

-- text objects

-- the first and last offset of the run of equal character classes around i,
-- for WORDs every non-space character is of the same class
local function run_bounds (classes, i, l, big)
  local function class (j)
    local c = classes[j + 1]
    return (big and c ~= SPACE) and WORD or c
  end

  local c = class (i)
  local s, e = i, i
  while s > 0 and class (s - 1) == c do s = s - 1 end
  while e < l - 1 and class (e + 1) == c do e = e + 1 end
  return s, e, c
end

-- selects the word around the cursor like vim's iw/aw (kind is "i" or "a"),
-- or extends the selection by the next word if more than a character is
-- selected; only within the logical line of the cursor
local function text_object (t, ks, kind, big)
  local cr, cc = api.cursor (t)
  local ln = line (t, cr)
  if ln.l == 0 then return end

  local classes, l = ln:classes (), ln.l
  local pos = math.min (ln:offset_of (cr, cc), l - 1)
  local single = ks.ar == cr and ks.ac == cc
  local backward = not single and (cr < ks.ar or (cr == ks.ar and cc < ks.ac))
  local s, e, c

  -- the run to start with: the cursor's, or the one next to it
  local from = single and pos or (backward and pos - 1 or pos + 1)
  if from < 0 or from >= l then return end
  s, e, c = run_bounds (classes, from, l, big)

  if kind == "a" then
    -- plus the whitespace after the word, the whitespace before it if there
    -- is none after (or at the other side when extending backwards), or the
    -- word after the whitespace
    local function whitespace_after ()
      if e + 1 < l then
        local _, e2, c2 = run_bounds (classes, e + 1, l, big)
        if c2 == SPACE or c == SPACE then e = e2 return true end
      end
    end
    local function whitespace_before ()
      if s > 0 then
        local s2, _, c2 = run_bounds (classes, s - 1, l, big)
        if c2 == SPACE or c == SPACE then s = s2 return true end
      end
    end

    if backward then
      whitespace_before ()
    elseif not whitespace_after () and single then
      whitespace_before ()
    end
  end

  if ks.select == "l" then toggle_select (t, ks, "n") end

  ks.dollar = 0
  if single then
    ks.ar, ks.ac = ln:coord_of (s)
  end

  local target = backward and s or e
  ks.offset = target
  api.cursor (t, ln:coord_of (target))
end

-- search

-- the search text as literal pattern, case-insensitive unless it contains
-- upper case
local function set_pattern (ks, text)
  if text ~= "" then
    ks.pattern = api.regex (api.regex_escape (text), UPPER_RE:find (text) and "" or "i")
  else
    ks.pattern = nil
  end
end

-- the start of the last match in text that ends before byte limit, matches
-- don't overlap like with perl's m//g
local function last_match (text, pattern, limit)
  local found
  for s, e in pattern:gfind (text) do
    if e >= limit then break end
    found = s
  end
  return found
end

local function find_next (t, ks, dir)
  if not ks.pattern then return false end
  dir = dir or ks.search_dir

  local pattern = ks.pattern

  local cr, cc = api.cursor (t)
  local ln = line (t, cr)
  local offset = ln:offset_of (cr, cc)
  if dir > 0 then offset = offset + 1 end

  local text, starts = ln:text ()
  local byte

  if dir > 0 then
    byte = pattern:find (text, starts[math.min (offset, ln.l) + 1])
  else
    byte = last_match (text, pattern, starts[math.max (0, math.min (offset, ln.l)) + 1])
  end

  -- check the other lines with the plain row texts first, which is much
  -- cheaper than splitting them into cells, scrollbacks can be large
  local top, nrow = api.top_row (t), api.nrow (t)
  local row = dir > 0 and ln.last + 1 or ln.beg - 1

  while not byte and row >= top and row < nrow do
    local beg, last = api.line_bounds (t, row)
    local parts = {}
    for r = beg, last do parts[#parts + 1] = (api.line (t, r)) end

    if pattern:find (table.concat (parts)) then
      ln = line (t, row)
      text, starts = ln:text ()
      if dir > 0 then
        byte = pattern:find (text)
      else
        byte = last_match (text, pattern, math.huge)
      end
    end

    row = dir > 0 and last + 1 or beg - 1
  end

  if not byte then return false end

  ks.dollar = 0
  ks.offset = offset_at (starts, byte)
  api.cursor (t, ln:coord_of (ks.offset))
  status_area (t, ks)
  return true
end

-- activation

local function activate (t, search)
  local ks = {
    select = "", dollar = 0, move_to = false,
    search = "", search_dir = 1, search_mode = false,
    -- the number of bytes of ks.search before the prompt's cursor
    search_cur = 0,
    clipboard = YANK_TARGET == "clipboard" or api.resource_boolean (t, "keyboard-select.clipboard"),
    -- the selected text is put into PRIMARY as the selection changes
    live_primary = YANK_TARGET == "clipboard",
    -- the selection is drawn like urxvt does it
    hl_sel = api.resource (t, "highlightColor") ~= nil,
  }

  if search then
    ks.search, ks.search_dir, ks.search_mode = "?", -1, true
    ks.search_cur = 1
  end

  ks.oldcr, ks.oldcc = api.cursor (t)
  -- the program is paused, so the cursor can move freely
  ks.old_pty_events = api.pty_events (t, api.EV_NONE)

  -- the program's cursor is out of the window when scrolled back, so the
  -- view is not scrolled to it: at the bottom of the scrollbuffer the cursor
  -- starts at the one of the program, at the top at the first row, anywhere
  -- else in the middle of the window
  local view, nrow = api.view_start (t), api.nrow (t)
  if view ~= 0 then
    local first, last = cursor_range (t)
    local row = view == api.top_row (t) and view or view + math.floor (nrow / 2)
    api.cursor (t, math.max (first, math.min (last, row)), 0)
  end
  ks.srhcr, ks.srhcc = api.cursor (t)

  local cr, cc = api.cursor (t)
  local ln = line (t, cr)
  ks.offset = ln:offset_of (cr, cc)

  clear_selection (t)
  t.keyboard_select = ks

  if ks.offset >= ln.l then
    ks.offset = ln.l > 0 and ln.l - 1 or 0
    api.cursor (t, ln:coord_of (ks.offset))
  end

  show_cursor (t)
  status_area (t, ks)
  api.refresh (t)
end

-- moves the cursor to the position where the mode was left the last time,
-- scrolling the view there, with the row in the middle, if it is out of the
-- window
local function goto_remembered (t, ks)
  local mem = t.keyboard_select_last
  if not mem then return end

  local view, nrow, top = api.view_start (t), api.nrow (t), api.top_row (t)
  local row = math.max (top, math.min (nrow - 1, mem.row))

  if row < view or row >= view + nrow then
    api.view_start (t, math.max (top, math.min (0, row - math.floor (nrow / 2))))
  end

  local first, last = cursor_range (t)
  ks.offset, ks.dollar = mem.offset, mem.dollar
  put_cursor (t, ks, math.max (first, math.min (last, row)))
  status_area (t, ks)
end

local function deactivate (t)
  local ks = t.keyboard_select

  clear_selection (t)
  if ks.overlay then ks.overlay:close () end

  t.keyboard_select = nil
  -- remembered for the next time the mode is entered
  t.keyboard_select_last = { row = api.cursor (t), offset = ks.offset, dollar = ks.dollar }
  -- the view is left where it is, only the cursor goes back to where it was
  api.cursor (t, ks.oldcr, ks.oldcc)
  api.pty_events (t, ks.old_pty_events)
  api.refresh (t)
end

-- key handling

-- the byte count before the character preceding/following the cursor
local function char_before (ks)
  local i = ks.search_cur
  while i > 1 and ks.search:byte (i) >= 0x80 and ks.search:byte (i) < 0xc0 do i = i - 1 end
  return i - 1
end

local function char_after (ks)
  local _, e = ks.search:find ("^" .. UTF8_CHAR, ks.search_cur + 1)
  return e or ks.search_cur
end

-- replaces the text between the byte counts from and to (the first byte of
-- the search is the / or ? that opened the prompt) and puts the cursor at
-- the end of the replacement
local function search_replace (ks, from, to, text)
  ks.search = ks.search:sub (1, from) .. text .. ks.search:sub (to + 1)
  ks.search_cur = from + #text
end

-- adds text at the cursor and searches again from where the search started
local function search_append (t, ks, text)
  search_replace (ks, ks.search_cur, ks.search_cur, text)
  set_pattern (ks, ks.search:sub (2))
  api.cursor (t, ks.srhcr, ks.srhcc)
  find_next (t, ks)
  status_area (t, ks)
end

-- adds pasted text to the search like typed text, only its first line as
-- matches never span lines
local function search_paste (t, ks, text)
  -- the search might be over by the time the text arrives
  if not text or t.keyboard_select ~= ks or ks.search == "" then return end

  text = text:match ("^[^\r\n]*"):gsub ("%c", "")
  if text ~= "" then
    search_append (t, ks, text)
    api.refresh (t)
  end
end

-- the keys of the search prompt
local function search_key (t, ks, ev, key, char, enter)
  if key == "Escape" then
    if ks.search_mode then return deactivate (t) end
    ks.search = ""
    status_area (t, ks)
  elseif key == "BackSpace" then
    if #ks.search == 1 then
      -- nothing left to delete, this ends the search
      ks.search = ""
      if ks.search_mode then return deactivate (t) end
    elseif ks.search_cur > 1 then
      search_replace (ks, char_before (ks), ks.search_cur, "")
    end
    status_area (t, ks)
  elseif ev.ctrl and key:lower () == "u" then
    -- clears the text before the cursor
    search_replace (ks, 1, ks.search_cur, "")
    status_area (t, ks)
  elseif ev.ctrl and key:lower () == "w" then
    -- removes the blanks and the word before the cursor
    local before = ks.search:sub (2, ks.search_cur)
    search_replace (ks, 1 + #before:gsub ("%s*%S*$", ""), ks.search_cur, "")
    status_area (t, ks)
  elseif key == "Home" or (ev.ctrl and key:lower () == "a") then
    ks.search_cur = 1
    status_area (t, ks)
  elseif key == "End" or (ev.ctrl and key:lower () == "e") then
    ks.search_cur = #ks.search
    status_area (t, ks)
  elseif key == "Left" then
    if ks.search_cur > 1 then ks.search_cur = char_before (ks) end
    status_area (t, ks)
  elseif key == "Right" then
    ks.search_cur = char_after (ks)
    status_area (t, ks)
  elseif enter then
    set_pattern (ks, ks.search:sub (2))
    ks.search = ""
    api.cursor (t, ks.srhcr, ks.srhcc)
    if not find_next (t, ks) then
      if ks.search_mode then return deactivate (t) end
      status_area (t, ks)
    end
  elseif ev.ctrl and ev.shift and key:lower () == "v" then
    -- pastes the clipboard
    api.selection_fetch (t, true, function (text) search_paste (t, ks, text) end, ev.time)
  elseif char then
    search_append (t, ks, char)
  end
end

local function key_press (t, ks, ev, keysym, str)
  -- printable ASCII keysyms are their characters, e.g. "$" for "dollar"
  local key = keysym >= 0x20 and keysym < 0x7f and string.char (keysym)
              or api.keysym_name (keysym) or ""
  local lower = key:lower ()
  local ctrl = ev.ctrl
  -- the typed text, without control characters (or Meta's escape prefix)
  local char = str ~= "" and not str:find ("%c") and str or nil
  local enter = key == "Return" or (ctrl and lower == "m")

  if ctrl and lower == "c" then
    deactivate (t)
  elseif ks.search ~= "" then
    search_key (t, ks, ev, key, char, enter)
  elseif ks.textobj then
    -- typing the capital W presses Shift first, which is no key to cancel with
    if keysym >= 0xffe1 and keysym <= 0xffee then return end

    -- the second key of iw/aw/iW/aW, anything else cancels
    local kind = ks.textobj
    ks.textobj = nil
    if key == "w" or key == "W" then text_object (t, ks, kind, key == "W") end
    status_area (t, ks)
  elseif ks.move_to then
    if key == "Escape" then
      ks.move_to = false
      status_area (t, ks)
    elseif char then
      ks.move_to = false
      ks.move_char = char
      move_to (t, ks, ";")
      status_area (t, ks)
    end
  elseif key == "Escape" and ks.select ~= "" then
    -- like vim, this only ends the visual mode
    clear_selection (t)
    ks.select = ""
    status_area (t, ks)
  elseif key == "Escape" or lower == "q" then
    deactivate (t)
  elseif lower == "y" or enter then
    local quit = false

    if key == "Y" and ks.select ~= "l" then
      quit = ks.select == ""
      toggle_select (t, ks, "l")
    end

    if ks.select ~= "" then
      copy (t, ks, ev.time, ks.clipboard)

      if lower == "y" then
        clear_selection (t)
        ks.select = ""
        status_area (t, ks)
      else
        quit = true
      end
    end

    if quit then deactivate (t) end
  elseif key == "V" then
    toggle_select (t, ks, "l")
  elseif key == "v" then
    toggle_select (t, ks, ctrl and "b" or "n")
  elseif key == "k" or key == "Up" then
    move_cursor (t, ks, "k")
  elseif key == "j" or key == "Down" then
    move_cursor (t, ks, "j")
  elseif key == "h" or key == "Left" then
    move_cursor (t, ks, "h")
  elseif key == "l" or key == "Right" then
    move_cursor (t, ks, "l")
  elseif key == "Tab" then
    goto_remembered (t, ks)
  elseif key == "End" then
    move_cursor (t, ks, "$")
  elseif key == "Home" then
    move_cursor (t, ks, "^")
  elseif CURSOR_KEYS[key] or (ctrl and SCROLL_KEYS[key]) then
    move_cursor (t, ks, key)
  elseif (key == "o" or key == "O") and ks.select ~= "" then
    -- the cursor goes to the other end of the selection and back again, O
    -- only changes the side in blockwise mode
    local cr, cc = api.cursor (t)
    local ar, ac = ks.ar, ks.ac
    local cln, aln = line (t, cr), line (t, ar)
    local co, ao = cln:offset_of (cr, cc), aln:offset_of (ar, ac)

    if key == "O" and ks.select == "b" then
      -- the rows stay, the columns (offsets in the lines) are exchanged
      ks.ar, ks.ac = aln:coord_of (math.min (co, math.max (aln.l - 1, 0)))
      api.cursor (t, cln:coord_of (math.min (ao, math.max (cln.l - 1, 0))))
    else
      ks.ar, ks.ac = cr, cc
      api.cursor (t, ar, ac)
    end
    ks.offset, ks.dollar = ao, 0
  elseif (key == "i" or key == "a") and ks.select ~= "" then
    ks.textobj = key
    status_area (t, ks, key)
  elseif lower == "f" then
    ks.move_to = true
    ks.move_dir = key == "F" and -1 or 1
    status_area (t, ks, key)
  elseif MOVE_TO_KEYS[key] then
    move_to (t, ks, key)
  elseif key == "/" or key == "?" then
    ks.search = key
    ks.search_cur = 1
    ks.search_dir = key == "?" and -1 or 1
    ks.srhcr, ks.srhcc = api.cursor (t)
    status_area (t, ks)
  elseif key == "P" or key == "p" then
    -- a forward search for the text of PRIMARY (P) or the CLIPBOARD (p),
    -- which is typed as if it had been pasted into the prompt of /
    ks.search = "/"
    ks.search_cur = 1
    ks.search_dir = 1
    ks.srhcr, ks.srhcc = api.cursor (t)
    status_area (t, ks)
    api.selection_fetch (t, key == "p", function (text) search_paste (t, ks, text) end, ev.time)
  elseif lower == "n" then
    find_next (t, ks, ks.search_dir * (key == "N" and -1 or 1))
  end

  if t.keyboard_select then
    show_cursor (t)
    sync_primary (t, ks, ev.time)
  end
  api.refresh (t)
end

api.bind (ACTIVATE_KEY, function (t)
  -- while active, the key is handled like any other (M-Escape is Escape)
  if t.keyboard_select then return false end
  activate (t)
end)

if SEARCH_KEY then
  api.bind (SEARCH_KEY, function (t)
    if t.keyboard_select then return false end
    activate (t, true)
  end)
end

-- the commands of the perl extension, for keysym resources
local COMMANDS = { activate = false, search = true }

api.on_user_command (function (t, cmd)
  local search = COMMANDS[cmd:match ("^keyboard%-select:(.*)$")]
  if search == nil then return end
  if not t.keyboard_select then activate (t, search) end
  return true
end)

api.on_action (function (t, ext, action)
  if ext ~= "keyboard-select" or COMMANDS[action] == nil then return end
  if not t.keyboard_select then activate (t, COMMANDS[action]) end
  return true
end)

api.on_key_press (function (t, ev, keysym, str)
  local ks = t.keyboard_select
  if not ks then return end

  key_press (t, ks, ev, keysym, str)

  -- every key is consumed while selecting
  return true
end)

-- nothing is sent to the program while selecting, a write that has to
-- wait for the pty would also resume its output
api.on_tt_write (function (t)
  return t.keyboard_select ~= nil
end)

-- the remembered position moves along with the text that is scrolled into
-- the scrollback, if the text is discarded it stays at the oldest line
api.on_scroll_back (function (t, lines, top_row)
  local mem = t.keyboard_select_last
  if not mem then return end

  mem.row = math.max (top_row, mem.row - lines)
end)

-- another screen has different text, which is not the one remembered
api.on_screen_change (function (t) t.keyboard_select_last = nil end)
api.on_reset (function (t) t.keyboard_select_last = nil end)

-- when the view is scrolled by other means than our keys, e.g. the mouse
-- wheel or scrollbar, the cursor follows into the window and the scroll
-- position in the status area is kept up to date
api.on_view_change (function (t)
  local ks = t.keyboard_select
  if not ks then return end

  local row = api.cursor (t)
  local first, last = cursor_range (t)
  if row < first or row > last then
    put_cursor (t, ks, math.max (first, math.min (last, row)))
    sync_primary (t, ks)
  end

  -- a pending f/F is shown in front of the position
  status_area (t, ks, ks.textobj or (ks.move_to and (ks.move_dir == -1 and "F" or "f") or nil))
end)

-- draws the selection, the highlight is undone after the refresh
api.on_refresh_begin (function (t)
  local ks = t.keyboard_select
  if not ks or ks.select == "" then return end

  local cr, cc = api.cursor (t)
  local br, bc, er, ec = calc_span (t, ks)
  local hl, reverse_cursor

  if ks.hl_sel then
    -- highlightColor is not applied to the cursor
    hl, reverse_cursor = api.RS_Sel, false
  else
    hl, reverse_cursor = api.RS_RVid, ks.select ~= "l"
  end

  if ks.select == "b" then
    local co = line (t, cr):offset_of (cr, cc)
    local dollar = ks.dollar > 0 and co >= ks.dollar - 1

    local r = br
    while r <= er do
      local ln = line (t, r)
      if bc < ln.l then
        if dollar then ec = ln.l end
        local r0, c0 = ln:coord_of (bc)
        local r1, c1 = ln:coord_of (math.min (ec, ln.l))
        api.xor_span (t, r0, c0, r1, c1, hl)
      elseif r == cr then
        reverse_cursor = false
      end
      r = ln.last + 1
    end
  else
    api.xor_span (t, br, bc, er, ec, hl)
  end

  -- the reversed cursor would be invisible on reversed text
  if reverse_cursor then
    api.xor_span (t, cr, cc, cr, cc + 1, hl)
  end
end)
