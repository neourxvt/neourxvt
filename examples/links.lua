-- links.lua - underlines URLs and opens them
--
-- * every URL on screen is underlined (also when wrapped over rows)
-- * Ctrl + left click opens the URL under the pointer with OPENER
-- * SELECT_KEY (Mod4-Control-g) selects URLs from the keyboard: Up/Down or k/j move
--   between the URLs on screen, Return/o opens, y copies to the clipboard,
--   Escape/q quits
--
-- Copy or link this file into ~/.config/nrxvt/plugins to enable it.

local api = nrxvt.api

local OPENER = { "xdg-open" }
local SELECT_KEY = "Mod4-Control-g"

-- scheme://rest, only the schemes listed below are considered URLs
local URL_PATTERN = "%f[%w](%a[%w+.-]*)://[%w%-._~:/?#%[%]@!$&'()*+,;=%%]+"
local SCHEMES = { http = true, https = true, ftp = true, file = true }

local UNDERLINE = { api.style { underline = true } }
local SELECTED = { api.style { reverse = true } }

-- longest logical line (in rows) that is scanned, bounds the work for
-- e.g. huge single line JSON outputs
local MAX_ROWS = 64

-- Returns the {first, last} byte ranges of all URLs in text.
local function find_urls (text)
  local urls = {}
  local pos = 1

  while true do
    local s, e, scheme = text:find (URL_PATTERN, pos)
    if not s then break end
    pos = e + 1

    if SCHEMES[scheme:lower ()] then
      -- strip trailing punctuation, and closing parentheses/brackets that
      -- belong to the surrounding text, e.g. "(see https://x.org)."
      local url = text:sub (s, e)
      while true do
        local last = url:sub (-1)
        if last:find ("[.,;:!?'\"]") then
          url = url:sub (1, -2)
        elseif last == ")" and select (2, url:gsub ("%(", "")) < select (2, url:gsub ("%)", "")) then
          url = url:sub (1, -2)
        elseif last == "]" and select (2, url:gsub ("%[", "")) < select (2, url:gsub ("%]", "")) then
          url = url:sub (1, -2)
        else
          break
        end
      end

      if #url > #scheme + 3 then
        urls[#urls + 1] = { s, s + #url - 1, url }
      end
    end
  end

  return urls
end

-- Reads the logical line starting at row, which may be wrapped over several
-- rows. Returns its text and the rows as {row, first byte, last byte}.
local function logical_line (t, row)
  local parts, segments = {}, {}
  local ofs = 0
  local last = api.nrow (t) - 1

  for r = row, math.min (row + MAX_ROWS - 1, last) do
    local text, wrapped = api.line (t, r)
    parts[#parts + 1] = text
    segments[#segments + 1] = { r, ofs + 1, ofs + #text }
    ofs = ofs + #text
    if not wrapped then break end
  end

  return table.concat (parts), segments
end

-- column of a byte offset into the text of a row, without the C call for
-- the common case of plain ASCII rows
local function col_of (t, row, text_is_ascii, byte)
  if text_is_ascii then return byte - 1 end
  return api.col (t, row, byte)
end

-- Calls fn (row, c0, c1) for every row part of the byte range [s, e] of a
-- logical line.
local function for_each_part (t, text, segments, s, e, fn)
  for _, seg in ipairs (segments) do
    local row, first, last = seg[1], seg[2], seg[3]
    if s <= last and e >= first then
      local ascii = not text:sub (first, last):find ("[\128-\255]")
      local c0 = col_of (t, row, ascii, math.max (s, first) - first + 1)
      local c1 = col_of (t, row, ascii, math.min (e, last) - first + 2)
      fn (row, c0, c1)
    end
  end
end

-- the URL at (row, col), if any
local function url_at (t, row, col)
  -- find the start of the logical line
  local start = row
  while start > api.top_row (t) and row - start < MAX_ROWS and select (2, api.line (t, start - 1)) do
    start = start - 1
  end

  local text, segments = logical_line (t, start)
  for _, url in ipairs (find_urls (text)) do
    local hit
    for_each_part (t, text, segments, url[1], url[2], function (r, c0, c1)
      if r == row and col >= c0 and col < c1 then hit = true end
    end)
    if hit then return url[3] end
  end
end

-- all URLs on screen, top to bottom, as {url, parts = {{row, c0, c1}...}}
local function visible_urls (t)
  local urls = {}
  local row = api.view_start (t)
  local last = row + api.nrow (t) - 1

  while row > api.top_row (t) and select (2, api.line (t, row - 1)) do
    row = row - 1
  end

  while row <= last do
    local text, segments = logical_line (t, row)
    for _, url in ipairs (find_urls (text)) do
      local parts = {}
      for_each_part (t, text, segments, url[1], url[2], function (r, c0, c1)
        parts[#parts + 1] = { r, c0, c1 }
      end)
      urls[#urls + 1] = { url = url[3], parts = parts }
    end
    row = segments[#segments][1] + 1
  end

  return urls
end

local function open (url)
  local argv = { unpack (OPENER) }
  argv[#argv + 1] = url
  api.spawn (argv)
end

-- underline the URLs of every line that changed, this runs only once per
-- change of a line, not on every refresh
api.on_line_update (function (t, row)
  local text, segments = logical_line (t, row)
  if not text:find ("://", 1, true) then return end

  for _, url in ipairs (find_urls (text)) do
    for_each_part (t, text, segments, url[1], url[2], function (r, c0, c1)
      api.rend (t, r, c0, c1, unpack (UNDERLINE))
    end)
  end
end)

api.on_button_press (function (t, ev)
  if ev.button == 1 and ev.ctrl then
    local url = url_at (t, ev.row, ev.col)
    if url then
      t.links_pressed = url
      return true
    end
  end
end)

api.on_button_release (function (t, ev)
  if ev.button == 1 and t.links_pressed then
    -- only open if the release happened on the same URL
    if url_at (t, ev.row, ev.col) == t.links_pressed then
      open (t.links_pressed)
    end
    t.links_pressed = nil
    return true
  end
end)

-- keyboard selection

api.bind (SELECT_KEY, function (t)
  local urls = visible_urls (t)
  if #urls == 0 then return end

  t.links_select = { urls = urls, index = #urls }
  api.refresh (t)
end)

api.on_refresh_begin (function (t)
  local select = t.links_select
  if select then
    for _, part in ipairs (select.urls[select.index].parts) do
      api.highlight (t, part[1], part[2], part[3], unpack (SELECTED))
    end
  end
end)

api.on_key_press (function (t, ev, keysym, str)
  local select = t.links_select
  if not select then return end

  local key = api.keysym_name (keysym)

  if key == "Up" or key == "k" then
    select.index = math.max (1, select.index - 1)
  elseif key == "Down" or key == "j" then
    select.index = math.min (#select.urls, select.index + 1)
  elseif key == "Return" or key == "KP_Enter" or key == "o" then
    open (select.urls[select.index].url)
    t.links_select = nil
  elseif key == "y" then
    api.set_selection (t, select.urls[select.index].url, true)
    t.links_select = nil
  elseif key == "Escape" or key == "q" then
    t.links_select = nil
  end

  -- every key is consumed while selecting
  api.refresh (t)
  return true
end)

-- the view moved, the selected URLs might not be on screen anymore
api.on_view_change (function (t)
  t.links_select = nil
end)
