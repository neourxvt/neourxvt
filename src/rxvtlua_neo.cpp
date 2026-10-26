/*----------------------------------------------------------------------*
 * File:	rxvtlua_neo.cpp - embedded LuaJIT plugin system
 *----------------------------------------------------------------------*
 *
 * Loads a Lua config (-c or $XDG_CONFIG_HOME/nrxvt/init.lua) and plugins in
 * nrxvt/plugins (default ~/.config/nrxvt). Plugins register hook functions
 * called with a terminal object and hook arguments. See doc/lua-plugins.md.
 *
 * With neither configuration nor plugins no Lua state is created. Hooks
 * with no handlers are skipped before any argument conversion.
 *----------------------------------------------------------------------*/

#include "../config.h"
#include "rxvt.h"

#if ENABLE_LUA

#include "keyboard.h"
#include "version.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <new>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>

#include <lua.hpp>

// nrxvt.api.regex works on UTF-8 strings, the 8 bit library
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

rxvt_lua_interp rxvt_lua;

// hook names as seen by plugins, i.e. "on_" + the lowercased hook name
static const char *const hook_upper_names[] = {
# define def(sym) # sym,
# include "hookinc.h"
# undef def
};
static char hook_names[HOOK_NUM][32];

// registry references
static int hook_refs[HOOK_NUM]; // array of handler functions per hook, 0 if none
// Startup-only counts allow failed files to roll back their registrations
// without adding bookkeeping to hook dispatch.
static int hook_counts[HOOK_NUM];
static int traceback_ref;       // debug.traceback

// the terminal the current hook/plugin code runs for, needed by functions
// parsing keysyms for nrxvt.api.bind and managing terminal-scoped objects
static rxvt_term *cur_term;

// Configuration is evaluated once; its defaults are applied independently
// to each terminal before resource-dependent initialization begins.
static int config_options_ref;
static bool config_loading;

// key bindings registered with nrxvt.api.bind
struct lua_binding
{
  KeySym sym;
  unsigned int state;
  int fn_ref;
};
static vector<lua_binding> bindings;

// overlay boxes created with nrxvt.api.new_overlay, like the overlay objects of
// perl. The shown ones are swapped into the screen around every refresh.
struct lua_overlay
{
  rxvt_term *term; // 0 once closed
  int x, y, w, h;  // position as given, size including the border
  bool border;
  rend_t rstyle;   // default rendition
  text_t **text;
  rend_t **rend;
  bool shown;
  int self_ref;    // keeps shown overlays alive, 0 if hidden

  // the area swapped into the screen by the refresh in progress, if any
  bool swapped;
  int sx, sy, sw, sh, sview;

  void swap ();
  void close (lua_State *L);
};
// all open overlays, the shown ones are drawn in this order
static vector<lua_overlay *> overlays;

// cells changed by nrxvt.api.highlight, restored after the refresh
struct saved_rend
{
  int row, col;
  rend_t rend;
};
static vector<saved_rend> highlights;
static rxvt_term *refresh_term; // terminal inside on_refresh_begin..end, if any

/////////////////////////////////////////////////////////////////////////////
// helpers

// Encodes the unicode character c as UTF-8 into buf (at least 4 bytes),
// returns the number of bytes.
static int
utf8_encode (unicode_t c, char *buf)
{
  if (c < 0x80)
    {
      buf[0] = c;
      return 1;
    }
  if (c < 0x800)
    {
      buf[0] = 0xc0 | (c >> 6);
      buf[1] = 0x80 | (c & 0x3f);
      return 2;
    }
  if (c < 0x10000)
    {
      buf[0] = 0xe0 | (c >> 12);
      buf[1] = 0x80 | ((c >> 6) & 0x3f);
      buf[2] = 0x80 | (c & 0x3f);
      return 3;
    }
  if (c < 0x110000)
    {
      buf[0] = 0xf0 | (c >> 18);
      buf[1] = 0x80 | ((c >> 12) & 0x3f);
      buf[2] = 0x80 | ((c >> 6) & 0x3f);
      buf[3] = 0x80 | (c & 0x3f);
      return 4;
    }
  // replacement character for anything invalid
  return utf8_encode (0xfffd, buf);
}

// Encodes one screen cell as UTF-8 into buf (at least CELL_UTF8_MAX bytes).
// Wide character continuation cells (NOCHAR) yield nothing, composed
// characters are expanded.
enum { CELL_COMPOSE_MAX = 16, CELL_UTF8_MAX = CELL_COMPOSE_MAX * 4 };

static int
cell_utf8 (text_t t, char *buf)
{
  if (t == NOCHAR)
    return 0;

#if ENABLE_COMBINING
  if (IS_COMPOSE (t))
    {
      wchar_t chars[CELL_COMPOSE_MAX];

      if (rxvt_composite.expand (t) > CELL_COMPOSE_MAX)
        return utf8_encode (0xfffd, buf);

      int n = rxvt_composite.expand (t, chars);
      int len = 0;

      for (int i = 0; i < n; i++)
        len += utf8_encode (chars[i], buf + len);

      return len;
    }
#endif

  return utf8_encode (t, buf);
}

// number of columns of a row that contain text
static int
row_len (rxvt_term *term, line_t &l)
{
  int len = l.l;
  clamp_it (len, 0, term->ncol);
  return len;
}

static void
push_wcs (lua_State *L, const wchar_t *str, int len)
{
  char *utf8 = rxvt_wcstoutf8 (str, len);
  lua_pushstring (L, utf8);
  free (utf8);
}

// Lua numbers are doubles, go through an integer type wide enough for all
// 32 bit values to get a well defined conversion.
static rend_t
check_rend (lua_State *L, int idx, rend_t def)
{
  return lua_isnoneornil (L, idx) ? def : (rend_t)(int64_t)luaL_checknumber (L, idx);
}

static void
push_xevent (lua_State *L, rxvt_term *term, XEvent *xe)
{
  lua_createtable (L, 0, 14);

#define setint(name, val) (lua_pushinteger (L, (val)), lua_setfield (L, -2, name))
#define setnum(name, val) (lua_pushnumber (L, (lua_Number)(val)), lua_setfield (L, -2, name))
#define setbool(name, val) (lua_pushboolean (L, (val)), lua_setfield (L, -2, name))

  setint ("type", xe->type);
  setnum ("window", xe->xany.window);
  setbool ("send_event", xe->xany.send_event);

  switch (xe->type)
    {
      case KeyPress:
      case KeyRelease:
      case ButtonPress:
      case ButtonRelease:
      case MotionNotify:
        {
          // the layout of these fields is shared by all five event types
          unsigned int state = xe->xmotion.state;

          setnum ("time", xe->xmotion.time);
          setint ("x", xe->xmotion.x);
          setint ("y", xe->xmotion.y);
          setint ("x_root", xe->xmotion.x_root);
          setint ("y_root", xe->xmotion.y_root);
          // absolute row (negative rows are in the scrollback), like nrxvt.api.line
          setint ("row", xe->xmotion.y / term->fheight + term->view_start);
          setint ("col", xe->xmotion.x / term->fwidth);
          setnum ("state", state);
          setbool ("shift", state & ShiftMask);
          setbool ("ctrl", state & ControlMask);
          setbool ("meta", state & term->ModMetaMask);

          if (xe->type == KeyPress || xe->type == KeyRelease)
            setint ("keycode", xe->xkey.keycode);
          else if (xe->type == ButtonPress || xe->type == ButtonRelease)
            setint ("button", xe->xbutton.button);
        }
        break;

      case ConfigureNotify:
        setint ("x", xe->xconfigure.x);
        setint ("y", xe->xconfigure.y);
        setint ("width", xe->xconfigure.width);
        setint ("height", xe->xconfigure.height);
        break;

      case PropertyNotify:
        setnum ("atom", xe->xproperty.atom);
        setint ("state", xe->xproperty.state);
        setnum ("time", xe->xproperty.time);
        break;

      case ClientMessage:
        setnum ("message_type", xe->xclient.message_type);
        setint ("format", xe->xclient.format);
        lua_createtable (L, 5, 0);
        for (int i = 0; i < 5; i++)
          {
            lua_pushnumber (L, (lua_Number)xe->xclient.data.l[i]);
            lua_rawseti (L, -2, i + 1);
          }
        lua_setfield (L, -2, "data");
        break;
    }

#undef setint
#undef setnum
#undef setbool
}

// Pushes the hook arguments, returns their number.
static int
push_args (lua_State *L, rxvt_term *term, va_list ap)
{
  int n = 0;

  for (;; n++)
    switch ((data_type)va_arg (ap, int))
      {
        case DT_END:
          return n;

        case DT_INT:
          lua_pushinteger (L, va_arg (ap, int));
          break;

        case DT_LONG:
          lua_pushnumber (L, (lua_Number)va_arg (ap, long));
          break;

        case DT_STR:
          lua_pushstring (L, va_arg (ap, char *));
          break;

        case DT_STR_LEN:
          {
            char *str = va_arg (ap, char *);
            int len = va_arg (ap, int);
            lua_pushlstring (L, str, len);
          }
          break;

        case DT_WCS_LEN:
          {
            wchar_t *str = va_arg (ap, wchar_t *);
            int len = va_arg (ap, int);
            push_wcs (L, str, len);
          }
          break;

        case DT_LCS_LEN:
          {
            long *str = va_arg (ap, long *);
            int len = va_arg (ap, int);

            lua_createtable (L, len, 0);
            for (int i = 0; i < len; i++)
              {
                lua_pushnumber (L, (lua_Number)str[i]);
                lua_rawseti (L, -2, i + 1);
              }
          }
          break;

        case DT_XEVENT:
          push_xevent (L, term, va_arg (ap, XEvent *));
          break;

        default:
          rxvt_fatal ("FATAL: unable to pass unknown hook argument type to lua\n");
      }
}

// Calls the function at the top of the stack (below its nargs arguments)
// in protected mode, reporting errors with a traceback. Returns whether the
// call succeeded, leaving nresults values (or nothing on error).
static bool
pcall (lua_State *L, int nargs, int nresults, const char *what)
{
  int func = lua_gettop (L) - nargs;

  lua_rawgeti (L, LUA_REGISTRYINDEX, traceback_ref);
  lua_insert (L, func);

  int status = lua_pcall (L, nargs, nresults, func);
  lua_remove (L, func);

  if (status)
    {
      rxvt_warn ("lua: %s: %s\n", what, lua_tostring (L, -1));
      lua_pop (L, 1);
      return false;
    }

  return true;
}

// Returns the terminal of the terminal object at index 1, raising an error
// for destroyed terminals.
static rxvt_term *
check_term (lua_State *L)
{
  luaL_checktype (L, 1, LUA_TTABLE);
  lua_rawgeti (L, 1, 0);
  rxvt_term *term = (rxvt_term *)lua_touserdata (L, -1);
  lua_pop (L, 1);

  if (!term)
    luaL_error (L, "terminal has been destroyed");

  return term;
}

// Checks that row is a valid row number of term and returns its line.
static line_t &
check_row (lua_State *L, rxvt_term *term, int idx)
{
  int row = luaL_checkinteger (L, idx);

  if (!IN_RANGE_EXC (row, term->top_row, term->nrow))
    luaL_error (L, "row %d out of range [%d, %d)", row, term->top_row, term->nrow);

  return ROW_of (term, row);
}

// Applies (r & and_mask) | or_mask to the columns [c0, c1) of line l.
// Font bits are never touched by the masks, but the font is reselected if
// the style (bold/italic) changes.
static void
apply_rend (rxvt_term *term, line_t &l, int c0, int c1, rend_t and_mask, rend_t or_mask)
{
  and_mask |= RS_fontMask;
  or_mask &= ~RS_fontMask;

  for (int col = c0; col < c1; col++)
    {
      rend_t r = (l.r[col] & and_mask) | or_mask;

      if (GET_STYLE (r) != GET_STYLE (l.r[col]))
        r = SET_FONT (r, FONTSET_of (term, r)->find_font (l.t[col]));

      l.r[col] = r;
    }
}

/////////////////////////////////////////////////////////////////////////////
// terminal API functions

// nrxvt.api.nrow (t) -> rows of the screen
static int
t_nrow (lua_State *L)
{
  lua_pushinteger (L, check_term (L)->nrow);
  return 1;
}

// nrxvt.api.ncol (t) -> columns of the screen
static int
t_ncol (lua_State *L)
{
  lua_pushinteger (L, check_term (L)->ncol);
  return 1;
}

// nrxvt.api.top_row (t) -> row number of the oldest scrollback line (<= 0)
static int
t_top_row (lua_State *L)
{
  lua_pushinteger (L, check_term (L)->top_row);
  return 1;
}

// nrxvt.api.view_start (t [, row]) -> row shown at the top of the window (<= 0),
// scrolls the view first if row is given
static int
t_view_start (lua_State *L)
{
  rxvt_term *term = check_term (L);

  if (!lua_isnoneornil (L, 2))
    term->scr_changeview (luaL_checkinteger (L, 2));

  lua_pushinteger (L, term->view_start);
  return 1;
}

// nrxvt.api.current_screen (t) -> 0 for the primary screen, 1 for the alternate.
// Read the parser's state directly; no PTY queries or output heuristics.
static int
t_current_screen (lua_State *L)
{
  lua_pushinteger (L, check_term (L)->current_screen);
  return 1;
}

// nrxvt.api.prompt_marker (t, row [, marked]) -> whether this row is marked.
// Explicit marking lets plugins support prompts the heuristic cannot see.
static int
t_prompt_marker (lua_State *L)
{
  rxvt_term *term = check_term (L);
  line_t &line = check_row (L, term, 2);

  if (!lua_isnoneornil (L, 3))
    {
      luaL_checktype (L, 3, LUA_TBOOLEAN);
      if (lua_toboolean (L, 3))
        line.f |= LINE_PROMPT;
      else
        line.f &= ~LINE_PROMPT;
    }

  lua_pushboolean (L, line.valid () && (line.f & LINE_PROMPT));
  return 1;
}

// nrxvt.api.prompt_markers (t) -> marked rows in order, oldest first.
static int
t_prompt_markers (lua_State *L)
{
  rxvt_term *term = check_term (L);
  lua_newtable (L);
  int index = 0;

  for (int row = term->top_row; row < term->nrow; row++)
    {
      line_t &line = ROW_of (term, row);
      if (line.valid () && (line.f & LINE_PROMPT))
        {
          lua_pushinteger (L, row);
          lua_rawseti (L, -2, ++index);
        }
    }

  return 1;
}

// nrxvt.api.prompt_jump (t, direction) -> view_start after navigating.
static int
t_prompt_jump (lua_State *L)
{
  rxvt_term *term = check_term (L);
  term->scr_prompt_jump (luaL_checkinteger (L, 2));
  lua_pushinteger (L, term->view_start);
  return 1;
}

// nrxvt.api.cursor (t [, row, col]) -> row, col
// The cursor position, the cursor is moved first if row and col are given.
// Like perl's screen_cur, rows in the scrollback are allowed, program output
// would go there as well, so pause the pty (nrxvt.api.pty_events) while it is there.
static int
t_cursor (lua_State *L)
{
  rxvt_term *term = check_term (L);

  if (!lua_isnoneornil (L, 2))
    {
      int row = luaL_checkinteger (L, 2);
      int col = luaL_checkinteger (L, 3);

      clamp_it (row, term->top_row, term->nrow - 1);
      clamp_it (col, 0, term->ncol - 1);

      term->screen.cur.row = row;
      term->screen.cur.col = col;
      // like any explicit cursor movement, this cancels a pending wrap
      term->screen.flags &= ~Screen_WrapNext;
      term->want_refresh = 1;
    }

  lua_pushinteger (L, term->screen.cur.row);
  lua_pushinteger (L, term->screen.cur.col);
  return 2;
}

// nrxvt.api.focus (t) -> whether the window has the input focus
static int
t_focus (lua_State *L)
{
  lua_pushboolean (L, check_term (L)->focus);
  return 1;
}

// nrxvt.api.font_size (t) -> width, height of a character cell in pixels
static int
t_font_size (lua_State *L)
{
  rxvt_term *term = check_term (L);
  lua_pushinteger (L, term->fwidth);
  lua_pushinteger (L, term->fheight);
  return 2;
}

// nrxvt.api.window (t) -> X window id of the toplevel window
static int
t_window (lua_State *L)
{
  lua_pushnumber (L, (lua_Number)check_term (L)->parent);
  return 1;
}

// Return the decorated frame and its monitor in root coordinates. Query only
// from window-event hooks, rather than adding X round trips to every redraw.
static int
t_window_geometry (lua_State *L)
{
  rxvt_term *term = check_term (L);
  Window root, child;
  int x, y, unused_x, unused_y;
  unsigned int width, height, border, depth;
  if (!term->parent
      || !XGetGeometry (term->dpy, term->parent, &root, &unused_x, &unused_y,
                        &width, &height, &border, &depth)
      || !XTranslateCoordinates (term->dpy, term->parent, term->display->root,
                                 0, 0, &x, &y, &child))
    return 0;

  int frame_x = x - int (border), frame_y = y - int (border);
  int frame_width = width + 2 * border, frame_height = height + 2 * border;
  Atom type;
  int format;
  unsigned long count, remaining;
  unsigned char *data = 0;
  // i3 advertises the actual decoration widths, including hidden edge
  // borders. Do not guess them from a configured border thickness.
  if (XGetWindowProperty (term->dpy, term->parent,
                          term->display->atom ("_NET_FRAME_EXTENTS"),
                          0, 4, False, XA_CARDINAL, &type, &format,
                          &count, &remaining, &data) == Success
      && type == XA_CARDINAL && format == 32 && count == 4)
    {
      const unsigned long *extents = (const unsigned long *)data;
      if (extents[0] <= 65535 && extents[1] <= 65535
          && extents[2] <= 65535 && extents[3] <= 65535)
        {
          frame_x = x - int (extents[0]); frame_y = y - int (extents[2]);
          frame_width = width + extents[0] + extents[1];
          frame_height = height + extents[2] + extents[3];
        }
    }
  if (data) XFree (data);

  int monitor_x = 0, monitor_y = 0;
  int monitor_width = DisplayWidth (term->dpy, term->display->screen);
  int monitor_height = DisplayHeight (term->dpy, term->display->screen);
#if HAVE_XRANDR
  // A window spanning outputs belongs to the one with the largest overlap.
  int64_t best = 0;
  for (const auto &monitor : term->display->monitors)
    {
      int w = max (0, min (frame_x + frame_width, monitor.x + monitor.width) - max (frame_x, monitor.x));
      int h = max (0, min (frame_y + frame_height, monitor.y + monitor.height) - max (frame_y, monitor.y));
      int64_t area = int64_t (w) * h;
      if (area > best)
        {
          best = area;
          monitor_x = monitor.x; monitor_y = monitor.y;
          monitor_width = monitor.width; monitor_height = monitor.height;
        }
    }
#endif
  lua_pushinteger (L, frame_x); lua_pushinteger (L, frame_y);
  lua_pushinteger (L, frame_width); lua_pushinteger (L, frame_height);
  lua_pushinteger (L, monitor_x); lua_pushinteger (L, monitor_y);
  lua_pushinteger (L, monitor_width); lua_pushinteger (L, monitor_height);
  return 8;
}

// nrxvt.api.line (t, row) -> text, wrapped
// The UTF-8 text of a row, wrapped tells whether the line continues on the
// next row. Each column contributes its character, the second column of
// wide characters contributes nothing.
static int
t_line (lua_State *L)
{
  rxvt_term *term = check_term (L);
  line_t &l = check_row (L, term, 2);
  int len = row_len (term, l);

  luaL_Buffer b;
  luaL_buffinit (L, &b);

  for (int col = 0; col < len; col++)
    {
      char buf[CELL_UTF8_MAX];
      luaL_addlstring (&b, buf, cell_utf8 (l.t[col], buf));
    }

  luaL_pushresult (&b);
  lua_pushboolean (L, l.is_longer ());
  return 2;
}

// nrxvt.api.col (t, row, byte) -> column of the character starting at the (1-based)
// byte offset byte of nrxvt.api.line (t, row), or the column after the text for
// offsets beyond it
static int
t_col (lua_State *L)
{
  rxvt_term *term = check_term (L);
  line_t &l = check_row (L, term, 2);
  int want = luaL_checkinteger (L, 3) - 1;
  int len = row_len (term, l);
  int ofs = 0;
  int col;

  for (col = 0; col < len; col++)
    {
      char buf[CELL_UTF8_MAX];
      int n = cell_utf8 (l.t[col], buf);

      // NOCHAR cells (n == 0) belong to the character before them
      if (n && ofs >= want)
        break;

      ofs += n;
    }

  lua_pushinteger (L, col);
  return 1;
}

// number of cells of a row as part of its logical line: all columns for
// rows that continue on the next row (like perl's ROW_l), otherwise the
// columns that contain text
static int
line_cells (rxvt_term *term, line_t &l)
{
  return l.is_longer () ? term->ncol : row_len (term, l);
}

// nrxvt.api.line_bounds (t, row) -> beg, end, len
// The first and last row of the logical line that row is part of, and its
// length in cells, like perl's line object. Offsets into the line are
// (row - beg) * nrxvt.api.ncol (t) + col.
static int
t_line_bounds (lua_State *L)
{
  rxvt_term *term = check_term (L);
  int row = luaL_checkinteger (L, 2);
  check_row (L, term, 2);

  int beg = row, end = row;

  while (beg > term->top_row && ROW_of (term, beg - 1).is_longer ())
    beg--;

  while (end < term->nrow - 1 && ROW_of (term, end).is_longer ())
    end++;

  lua_pushinteger (L, beg);
  lua_pushinteger (L, end);
  lua_pushinteger (L, (end - beg) * term->ncol + line_cells (term, ROW_of (term, end)));
  return 3;
}

// nrxvt.api.cells (t, row) -> { cell, ... }
// The UTF-8 text of every cell of a row, one table entry per column, so
// that column col is at index col + 1. The second column of wide characters
// is "". Rows that continue on the next row have nrxvt.api.ncol (t) cells, the
// others as many as they contain text, so the cells of the rows of a
// logical line concatenate to nrxvt.api.line_bounds offsets.
static int
t_cells (lua_State *L)
{
  rxvt_term *term = check_term (L);
  line_t &l = check_row (L, term, 2);
  int len = line_cells (term, l);

  lua_createtable (L, len, 0);

  for (int col = 0; col < len; col++)
    {
      char buf[CELL_UTF8_MAX];
      lua_pushlstring (L, buf, cell_utf8 (l.t[col], buf));
      lua_rawseti (L, -2, col + 1);
    }

  return 1;
}

// nrxvt.api.rend (t, row, c0, c1, and_mask, or_mask)
// Permanently changes the rendition of the columns [c0, c1) of a row until
// the line changes again, use it in on_line_update.
static int
t_rend (lua_State *L)
{
  rxvt_term *term = check_term (L);
  line_t &l = check_row (L, term, 2);
  int c0 = luaL_checkinteger (L, 3);
  int c1 = luaL_checkinteger (L, 4);
  rend_t and_mask = check_rend (L, 5, ~(rend_t)0);
  rend_t or_mask  = check_rend (L, 6, 0);

  clamp_it (c0, 0, term->ncol);
  clamp_it (c1, c0, term->ncol);

  apply_rend (term, l, c0, c1, and_mask, or_mask);
  term->want_refresh = 1;
  return 0;
}

// nrxvt.api.highlight (t, row, c0, c1, and_mask, or_mask)
// Like nrxvt.api.rend, but only for the refresh in progress, so it is only
// allowed in on_refresh_begin.
static int
t_highlight (lua_State *L)
{
  rxvt_term *term = check_term (L);
  int row = luaL_checkinteger (L, 2);
  int c0 = luaL_checkinteger (L, 3);
  int c1 = luaL_checkinteger (L, 4);
  rend_t and_mask = check_rend (L, 5, ~(rend_t)0);
  rend_t or_mask  = check_rend (L, 6, 0);

  if (refresh_term != term)
    return luaL_error (L, "nrxvt.api.highlight is only allowed in on_refresh_begin");

  // silently ignore rows outside of the view, it saves plugins the check
  if (!IN_RANGE_EXC (row - term->view_start, 0, term->nrow)
      || !IN_RANGE_EXC (row, term->top_row, term->nrow))
    return 0;

  clamp_it (c0, 0, term->ncol);
  clamp_it (c1, c0, term->ncol);

  line_t &l = ROW_of (term, row);

  for (int col = c0; col < c1; col++)
    {
      saved_rend saved = { row, col, l.r[col] };
      highlights.push_back (saved);
    }

  apply_rend (term, l, c0, c1, and_mask, or_mask);
  return 0;
}

// XORs mask into the renditions of the columns [c0, c1) of a visible row
// for the refresh in progress, the cells are restored like highlights.
static void
xor_cells (rxvt_term *term, int row, int c0, int c1, rend_t mask)
{
  line_t &l = ROW_of (term, row);

  // font bits are never touched, like in apply_rend
  mask &= ~RS_fontMask;

  for (int col = c0; col < c1; col++)
    {
      saved_rend saved = { row, col, l.r[col] };
      highlights.push_back (saved);

      rend_t r = l.r[col] ^ mask;

      if (GET_STYLE (r) != GET_STYLE (l.r[col]))
        r = SET_FONT (r, FONTSET_of (term, r)->find_font (l.t[col]));

      l.r[col] = r;
    }
}

// Reads the r0, c0, r1, c1 arguments of nrxvt.api.xor_span/nrxvt.api.xor_rect, raising an
// error outside of on_refresh_begin. Returns false if no row is visible.
static bool
check_xor_args (lua_State *L, rxvt_term *term, int &r0, int &c0, int &r1, int &c1)
{
  r0 = luaL_checkinteger (L, 2);
  c0 = luaL_checkinteger (L, 3);
  r1 = luaL_checkinteger (L, 4);
  c1 = luaL_checkinteger (L, 5);

  if (refresh_term != term)
    luaL_error (L, "only allowed in on_refresh_begin");

  clamp_it (c0, 0, term->ncol);
  clamp_it (c1, 0, term->ncol);

  return r0 < term->view_start + term->nrow && r1 >= term->view_start;
}

// nrxvt.api.xor_span (t, r0, c0, r1, c1 [, mask]) - like perl's scr_xor_span, XORs
// mask (default nrxvt.api.RS_RVid) into the renditions from (r0, c0) up to,
// but not including, (r1, c1), rows in between are covered completely.
// Like nrxvt.api.highlight, it is only allowed in on_refresh_begin and undone
// after the refresh.
static int
t_xor_span (lua_State *L)
{
  rxvt_term *term = check_term (L);
  int r0, c0, r1, c1;

  if (!check_xor_args (L, term, r0, c0, r1, c1))
    return 0;

  rend_t mask = check_rend (L, 6, RS_RVid);
  int view_end = term->view_start + term->nrow; // exclusive

  // only the visible part is changed
  if (r0 < term->view_start)
    r0 = term->view_start, c0 = 0;
  if (r1 >= view_end)
    r1 = view_end - 1, c1 = term->ncol;

  for (int row = r0; row <= r1; row++)
    xor_cells (term, row, row == r0 ? c0 : 0, row == r1 ? c1 : term->ncol, mask);

  return 0;
}

// nrxvt.api.xor_rect (t, r0, c0, r1, c1 [, mask1 [, mask2]]) - like perl's
// scr_xor_rect, XORs mask1 (default nrxvt.api.RS_RVid) into the columns
// [c0, c1) of the rows r0 to r1 (inclusive), and mask2 (default
// nrxvt.api.RS_RVid + nrxvt.api.RS_Uline) into the trailing blanks of each row but
// the first one, which is how urxvt shows rectangular selections. Only
// allowed in on_refresh_begin, undone after the refresh.
static int
t_xor_rect (lua_State *L)
{
  rxvt_term *term = check_term (L);
  int r0, c0, r1, c1;

  if (!check_xor_args (L, term, r0, c0, r1, c1))
    return 0;

  rend_t mask1 = check_rend (L, 6, RS_RVid);
  rend_t mask2 = check_rend (L, 7, RS_RVid | RS_Uline);

  max_it (r0, term->view_start);
  min_it (r1, term->view_start + term->nrow - 1);

  for (int row = r0; row <= r1; row++)
    {
      text_t *t = ROW_of (term, row).t;

      xor_cells (term, row, c0, c1, mask1);

      // the trailing blanks, as in scr_xor_rect
      int col = c1;
      while (col > c0 && (t[col - 1] == NOCHAR || unicode::is_space (t[col - 1])))
        col--;

      // scr_xor_rect XORs the first blank twice, i.e. not at all
      xor_cells (term, row, col + 1, c1, mask2);
    }

  return 0;
}

// nrxvt.api.write (t, data) - sends data to the program, as if it was typed
static int
t_write (lua_State *L)
{
  rxvt_term *term = check_term (L);
  size_t len;
  const char *data = luaL_checklstring (L, 2, &len);
  term->tt_write (data, len);
  return 0;
}

#if defined (KEYSYM_RESOURCE)
// nrxvt.api.feed (t, data) - processes data as if the program had output it
static int
t_feed (lua_State *L)
{
  rxvt_term *term = check_term (L);
  size_t len;
  const char *data = luaL_checklstring (L, 2, &len);
  term->cmdbuf_append (data, len);
  return 0;
}
#endif

// nrxvt.api.refresh (t) - schedules a redraw, needed after changes made outside of
// hooks (e.g. in timers)
static int
t_refresh (lua_State *L)
{
  rxvt_term *term = check_term (L);
  term->want_refresh = 1;
  term->refresh_check ();
  return 0;
}

// nrxvt.api.pty_events (t [, events]) -> previous events
// Like perl's pty_ev_events, the I/O events (nrxvt.api.EV_READ, nrxvt.api.EV_WRITE)
// urxvt waits for on the pty, changed to events if given. nrxvt.api.EV_NONE
// pauses the program output until the old events are restored. Writes to
// the program that can't be sent at once resume it, as urxvt then waits
// for the pty to become writable (and readable) again.
static int
t_pty_events (lua_State *L)
{
  rxvt_term *term = check_term (L);
  int old = term->pty_ev.events;

  if (!lua_isnoneornil (L, 2))
    term->pty_ev.set (luaL_checkinteger (L, 2));

  lua_pushinteger (L, old);
  return 1;
}

// nrxvt.api.overlay (t, x, y, w, h) - opens the overlay window (there is only one),
// negative x/y count from the right/bottom, the border adds a cell on each
// side
static int
t_overlay (lua_State *L)
{
  rxvt_term *term = check_term (L);
  term->scr_overlay_new (luaL_checkinteger (L, 2), luaL_checkinteger (L, 3),
                         luaL_checkinteger (L, 4), luaL_checkinteger (L, 5));
  return 0;
}

// nrxvt.api.overlay_text (t, x, y, text [, and_mask, or_mask]) - writes text into the
// overlay, the masks apply to the default overlay rendition
static int
t_overlay_text (lua_State *L)
{
  rxvt_term *term = check_term (L);
  int x = luaL_checkinteger (L, 2);
  int y = luaL_checkinteger (L, 3);
  const char *text = luaL_checkstring (L, 4);
  rend_t and_mask = check_rend (L, 5, ~(rend_t)0);
  rend_t or_mask  = check_rend (L, 6, 0);
  rend_t rend = (OVERLAY_RSTYLE & (and_mask | RS_fontMask)) | (or_mask & ~RS_fontMask);

  wchar_t *wtext = rxvt_utf8towcs (text);

  for (wchar_t *s = wtext; *s; s++)
    {
      text_t t = *s;
      int width = WCWIDTH (t);

      while (width-- > 0)
        {
          term->scr_overlay_set (x++, y, t, rend);
          t = NOCHAR;
        }
    }

  free (wtext);
  term->want_refresh = 1;
  return 0;
}

// nrxvt.api.overlay_off (t) - closes the overlay
static int
t_overlay_off (lua_State *L)
{
  check_term (L)->scr_overlay_off ();
  return 0;
}

/////////////////////////////////////////////////////////////////////////////
// overlay objects

// Exchanges the overlay contents with the screen cells below it: called
// once to show it at the start of a refresh, and again at its end to
// restore the screen, like perl's overlay::swap.
void
lua_overlay::swap ()
{
  if (!swapped)
    {
      // negative positions count from the right/bottom, and the position
      // follows size changes of the terminal
      sx = max (0, min (MOD (x, term->ncol), term->ncol - w));
      sy = max (0, min (MOD (y, term->nrow), term->nrow - h));
      sw = min (w, term->ncol - sx);
      sh = min (h, term->nrow - sy);
      sview = term->view_start;

      // the cursor would be drawn on top of the overlay, hide it for this
      // refresh (scr_refresh restores the flags afterwards)
      if (IN_RANGE_EXC (term->screen.cur.col - sx, 0, sw)
          && IN_RANGE_EXC (term->screen.cur.row - sview - sy, 0, sh))
        term->screen.flags &= ~Screen_VisibleCursor;
    }

  for (int row = 0; row < sh; row++)
    {
      line_t &l = ROW_of (term, sview + sy + row);
      text_t *t1 = text[row], *t2 = l.t + sx;
      rend_t *r1 = rend[row], *r2 = l.r + sx;

      for (int col = 0; col < sw; col++)
        {
          text_t t = t1[col]; t1[col] = t2[col]; t2[col] = t;
          rend_t r = r1[col]; r1[col] = r2[col];
          r2[col] = SET_FONT (r, FONTSET_of (term, r)->find_font (t));
        }
    }

  swapped = !swapped;
}

// Hides the overlay and frees its contents, overlays can't be used after.
void
lua_overlay::close (lua_State *L)
{
  if (!term)
    return;

  // restore the screen if this happens during a refresh
  if (swapped)
    swap ();

  term->want_refresh = 1;

  if (self_ref)
    luaL_unref (L, LUA_REGISTRYINDEX, self_ref);

  for (int row = 0; row < h; row++)
    {
      delete [] text[row];
      delete [] rend[row];
    }

  delete [] text;
  delete [] rend;

  for (unsigned int i = 0; i < overlays.size (); i++)
    if (overlays[i] == this)
      {
        overlays.erase (overlays.begin () + i);
        break;
      }

  term = 0;
}

// Swaps the shown overlays of term into the screen (in is true) or back,
// in reverse order, so overlapping overlays restore the screen properly.
static void
swap_overlays (rxvt_term *term, bool in)
{
  int n = overlays.size ();

  for (int i = 0; i < n; i++)
    {
      lua_overlay *ov = overlays[in ? i : n - 1 - i];

      if (ov->term == term && (in ? ov->shown : ov->swapped))
        ov->swap ();
    }
}

static lua_overlay *
check_overlay (lua_State *L)
{
  lua_overlay *ov = (lua_overlay *)luaL_checkudata (L, 1, "nrxvt.overlay");

  if (!ov->term)
    luaL_error (L, "overlay has been closed");

  return ov;
}

// nrxvt.api.new_overlay (t, x, y, w, h [, border [, and_mask, or_mask]]) -> overlay
// Creates a hidden overlay box of w x h cells at column x and row y of the
// window, negative values count from the right/bottom. There is a border
// around it (taking another cell on each side) unless border is false.
// The masks change the default overlay rendition (black on yellow).
static int
t_new_overlay (lua_State *L)
{
  rxvt_term *term = check_term (L);
  int x = luaL_checkinteger (L, 2);
  int y = luaL_checkinteger (L, 3);
  int w = max (0, (int)luaL_checkinteger (L, 4));
  int h = max (0, (int)luaL_checkinteger (L, 5));
  bool border = lua_isnoneornil (L, 6) || lua_toboolean (L, 6);
  rend_t and_mask = check_rend (L, 7, ~(rend_t)0);
  rend_t or_mask  = check_rend (L, 8, 0);

  if (border)
    w += 2, h += 2;

  lua_overlay *ov = (lua_overlay *)lua_newuserdata (L, sizeof (lua_overlay));
  memset (ov, 0, sizeof (*ov));
  luaL_getmetatable (L, "nrxvt.overlay");
  lua_setmetatable (L, -2);

  ov->term = term;
  ov->x = x, ov->y = y, ov->w = w, ov->h = h;
  ov->border = border;
  ov->rstyle = (OVERLAY_RSTYLE & (and_mask | RS_fontMask)) | (or_mask & ~RS_fontMask);
  ov->text = new text_t *[h];
  ov->rend = new rend_t *[h];

  for (int row = 0; row < h; row++)
    {
      text_t *tp = ov->text[row] = new text_t[w];
      rend_t *rp = ov->rend[row] = new rend_t[w];

      for (int col = 0; col < w; col++)
        {
          text_t t = ' ';

          // the same box drawing characters as the builtin overlay
          if (border)
            {
              bool left = col == 0, right = col == w - 1;

              if (row == 0)
                t = left ? 0x2554 : right ? 0x2557 : 0x2550;
              else if (row == h - 1)
                t = left ? 0x255a : right ? 0x255d : 0x2550;
              else if (left || right)
                t = 0x2551;
            }

          tp[col] = t;
          rp[col] = ov->rstyle;
        }
    }

  overlays.push_back (ov);
  return 1;
}

// overlay:set (x, y, text [, and_mask, or_mask]) - writes text at column x
// of row y, inside of the border. The masks change the default rendition of
// the overlay, without them the rendition of the cells stays as it was.
static int
overlay_set (lua_State *L)
{
  lua_overlay *ov = check_overlay (L);
  int b = ov->border;
  int x = luaL_checkinteger (L, 2) + b;
  int y = luaL_checkinteger (L, 3) + b;
  const char *text = luaL_checkstring (L, 4);
  bool have_rend = !lua_isnoneornil (L, 5) || !lua_isnoneornil (L, 6);
  rend_t and_mask = check_rend (L, 5, ~(rend_t)0);
  rend_t or_mask  = check_rend (L, 6, 0);
  rend_t rend = (ov->rstyle & (and_mask | RS_fontMask)) | (or_mask & ~RS_fontMask);

  if (!IN_RANGE_EXC (y, b, ov->h - b))
    return 0;

  wchar_t *wtext = rxvt_utf8towcs (text);

  for (wchar_t *s = wtext; *s; s++)
    {
      text_t t = *s;
      int width = WCWIDTH (t);

      // wide characters continue with NOCHAR cells, like on the screen
      for (; width > 0; width--, x++, t = NOCHAR)
        if (IN_RANGE_EXC (x, b, ov->w - b))
          {
            ov->text[y][x] = t;
            if (have_rend)
              ov->rend[y][x] = rend;
          }
    }

  free (wtext);

  if (ov->shown)
    {
      ov->term->want_refresh = 1;
      ov->term->refresh_check ();
    }

  return 0;
}

// overlay:show () - shows the overlay on top of the other overlays. Shown
// overlays stay visible without references, until hidden or closed.
static int
overlay_show (lua_State *L)
{
  lua_overlay *ov = check_overlay (L);

  // move it to the end, so it is drawn last
  for (unsigned int i = 0; i < overlays.size (); i++)
    if (overlays[i] == ov)
      {
        overlays.erase (overlays.begin () + i);
        break;
      }

  overlays.push_back (ov);

  if (!ov->shown)
    {
      ov->shown = true;
      lua_pushvalue (L, 1);
      ov->self_ref = luaL_ref (L, LUA_REGISTRYINDEX);
    }

  // overlays are drawn by the refresh hooks
  rxvt_lua.should_invoke[HOOK_REFRESH_BEGIN] = 1;
  rxvt_lua.should_invoke[HOOK_REFRESH_END] = 1;

  ov->term->want_refresh = 1;
  ov->term->refresh_check ();
  return 0;
}

// overlay:hide ()
static int
overlay_hide (lua_State *L)
{
  lua_overlay *ov = check_overlay (L);

  if (ov->shown)
    {
      ov->shown = false;
      luaL_unref (L, LUA_REGISTRYINDEX, ov->self_ref);
      ov->self_ref = 0;

      ov->term->want_refresh = 1;
      ov->term->refresh_check ();
    }

  return 0;
}

// overlay:close () - hides the overlay for good and frees it, closing it
// again does nothing
static int
overlay_close (lua_State *L)
{
  lua_overlay *ov = (lua_overlay *)luaL_checkudata (L, 1, "nrxvt.overlay");
  ov->close (L);
  return 0;
}

static const luaL_Reg overlay_methods[] = {
  { "set",   overlay_set   },
  { "show",  overlay_show  },
  { "hide",  overlay_hide  },
  { "close", overlay_close },
  { 0, 0 }
};

// nrxvt.api.selection (t [, clipboard]) -> the selected (or clipboard) text, or nil
static int
t_selection (lua_State *L)
{
  rxvt_term *term = check_term (L);
  bool clipboard = lua_toboolean (L, 2);
  wchar_t *text    = clipboard ? term->selection.clip_text : term->selection.text;
  unsigned int len = clipboard ? term->selection.clip_len  : term->selection.len;

  if (text)
    push_wcs (L, text, len);
  else
    lua_pushnil (L);

  return 1;
}

// nrxvt.api.set_selection (t, text [, clipboard]) -> whether the selection could be
// claimed, makes text the PRIMARY (or CLIPBOARD) selection
static int
t_set_selection (lua_State *L)
{
  rxvt_term *term = check_term (L);
  const char *text = luaL_checkstring (L, 2);
  bool clipboard = lua_toboolean (L, 3);
  wchar_t *&sel    = clipboard ? term->selection.clip_text : term->selection.text;
  unsigned int &len = clipboard ? term->selection.clip_len  : term->selection.len;

  free (sel);
  sel = rxvt_utf8towcs (text);
  len = wcslen (sel);

  lua_pushboolean (L, term->selection_grab (CurrentTime, clipboard));
  return 1;
}

// optional X event time argument, e.g. ev.time, CurrentTime if omitted
static Time
check_time (lua_State *L, int idx)
{
  return lua_isnoneornil (L, idx) ? CurrentTime : (Time)luaL_checknumber (L, idx);
}

// Implements nrxvt.api.selection_beg/end/mark ([row, col]) -> row, col, which get
// or set the corners and the mark of the selection like perl's methods of
// the same names (followed by nrxvt.api.selection_make to select the text).
static int
selection_point (lua_State *L, int which)
{
  rxvt_term *term = check_term (L);
  row_col_t &rc = which == 0 ? term->selection.beg
                : which == 1 ? term->selection.end
                :              term->selection.mark;

  if (!lua_isnoneornil (L, 2))
    {
      rc.row = luaL_checkinteger (L, 2);
      rc.col = luaL_checkinteger (L, 3);

      // the end is exclusive, the same adjustments as in perl
      if (which == 1)
        {
          if (rc.col == 0)
            {
              // col 0 means the end of the previous row
              rc.row--;
              rc.col = term->ncol;
            }
          else if (IN_RANGE_EXC (rc.row, term->top_row, term->nrow)
                   && rc.col > ROW_of (term, rc.row).l)
            {
              // beyond the text means the whole row, including its newline
              rc.col = term->ncol;
            }
        }

      clamp_it (rc.col, 0, term->ncol);
      clamp_it (rc.row, term->top_row, term->nrow - 1);

      term->selection.screen = term->current_screen;
      term->selection_changed ();
      term->refresh_check ();
    }

  lua_pushinteger (L, rc.row);
  lua_pushinteger (L, rc.col);
  return 2;
}

static int t_selection_beg  (lua_State *L) { return selection_point (L, 0); }
static int t_selection_end  (lua_State *L) { return selection_point (L, 1); }
static int t_selection_mark (lua_State *L) { return selection_point (L, 2); }

// nrxvt.api.selection_make (t [, rect [, time]]) - selects the text between
// nrxvt.api.selection_beg and nrxvt.api.selection_end (t, a rectangle if rect is true) like a
// selection made with the mouse, and claims the PRIMARY selection
static int
t_selection_make (lua_State *L)
{
  rxvt_term *term = check_term (L);

  term->selection.op = SELECTION_CONT;
  term->selection.rect = lua_toboolean (L, 2);
  term->selection_make (check_time (L, 3));
  return 0;
}

// nrxvt.api.selection_grab (t [, clipboard [, time]]) -> whether the PRIMARY (or
// CLIPBOARD) selection could be claimed with the current text
static int
t_selection_grab (lua_State *L)
{
  rxvt_term *term = check_term (L);
  lua_pushboolean (L, term->selection_grab (check_time (L, 3), lua_toboolean (L, 2)));
  return 1;
}

// nrxvt.api.selection_clear (t [, clipboard]) - forgets the PRIMARY text and removes
// the highlight of the selection (or forgets the CLIPBOARD text)
static int
t_selection_clear (lua_State *L)
{
  check_term (L)->selection_clear (lua_toboolean (L, 2));
  return 0;
}

// nrxvt.api.selection_request (t [, clipboard [, time]]) - pastes the PRIMARY (or
// CLIPBOARD) selection into the program, like a middle click (Shift-Insert)
static int
t_selection_request (lua_State *L)
{
  rxvt_term *term = check_term (L);
  term->selection_request (check_time (L, 3), lua_toboolean (L, 2) ? Sel_Clipboard : Sel_Primary);
  return 0;
}

// selection requests of nrxvt.api.selection_fetch, which hand the text to a lua
// function instead of pasting it, the cb_sv of the rxvt_selection
struct lua_fetch
{
  rxvt_selection *req;
  rxvt_term *term;
  int fn_ref;
};

static vector<lua_fetch *> fetches;

// Removes a fetch from the list and frees it with its request, without
// calling its function, e.g. when its terminal is destroyed.
static void
fetch_free (lua_State *L, lua_fetch *f)
{
  for (unsigned int i = 0; i < fetches.size (); i++)
    if (fetches[i] == f)
      {
        fetches.erase (fetches.begin () + i);
        break;
      }

  luaL_unref (L, LUA_REGISTRYINDEX, f->fn_ref);
  delete f->req;
  delete f;
}

// nrxvt.api.selection_fetch (t, clipboard, fn [, time]) -> whether it was requested
// Requests the PRIMARY (or CLIPBOARD) selection like nrxvt.api.selection_request,
// but calls fn (text) with it instead of pasting it into the program, or
// fn (nil) if there is none. Only one fetch per terminal can be pending,
// false is returned while there is one.
static int
t_selection_fetch (lua_State *L)
{
  rxvt_term *term = check_term (L);
  bool clipboard = lua_toboolean (L, 2);
  luaL_checktype (L, 3, LUA_TFUNCTION);
  Time tm = check_time (L, 4);

  for (unsigned int i = 0; i < fetches.size (); i++)
    if (fetches[i]->term == term)
      {
        lua_pushboolean (L, 0);
        return 1;
      }

  // a property of its own, so a paste running at the same time does not
  // overwrite the data
  Atom prop = XInternAtom (term->dpy, "_URXVT_LUA_SELECTION", False);

  lua_fetch *f = new lua_fetch;
  f->term = term;
  lua_pushvalue (L, 3);
  f->fn_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  f->req = new rxvt_selection (term->display, clipboard ? Sel_Clipboard : Sel_Primary, tm, term->vt, prop, term);
  f->req->cb_sv = f;
  fetches.push_back (f);

  // run () may already finish (and free) the request, e.g. when there is
  // no selection owner but a cut buffer
  f->req->run ();

  lua_pushboolean (L, 1);
  return 1;
}

// called by rxvt_selection::finish for the requests of nrxvt.api.selection_fetch,
// data is in the locale encoding or 0 if there is no selection
void
rxvt_lua_interp::selection_finish (rxvt_selection *sel, char *data, unsigned int len)
{
  lua_fetch *f = (lua_fetch *)sel->cb_sv;
  rxvt_term *term = f->term;
  int base = lua_gettop (L);

  lua_rawgeti (L, LUA_REGISTRYINDEX, f->fn_ref);

  if (data)
    {
      SET_LOCALE (term->locale);
      wchar_t *w = rxvt_mbstowcs (data, len);
      push_wcs (L, w, wcslen (w));
      free (w);
    }
  else
    lua_pushnil (L);

  // freed before the call, so the function can start another fetch
  fetch_free (L, f);

  rxvt_term *prev_term = cur_term;
  cur_term = term;
  pcall (L, 1, 0, "selection_fetch");
  cur_term = prev_term;

  lua_settop (L, base);
}

// nrxvt.api.resource (t, name) -> value of the X resource for the configured name/class, or nil
static int
t_resource (lua_State *L)
{
  rxvt_term *term = check_term (L);
  lua_pushstring (L, term->x_resource (luaL_checkstring (L, 2)));
  return 1;
}

// nrxvt.api.resource_boolean (t, name) -> true or false, nil if the resource is not
// set. Like perl's x_resource_boolean, "true", "yes", "on" and "1" (in any
// case, surrounding blanks are ignored) are true, anything else is false.
static int
t_resource_boolean (lua_State *L)
{
  rxvt_term *term = check_term (L);
  const char *value = term->x_resource (luaL_checkstring (L, 2));

  if (!value)
    {
      lua_pushnil (L);
      return 1;
    }

  while (isspace ((unsigned char)*value))
    value++;

  int len = strlen (value);
  while (len && isspace ((unsigned char)value[len - 1]))
    len--;

  static const char *const yes[] = { "true", "yes", "on", "1" };
  bool result = false;

  for (unsigned int i = 0; i < ecb_array_length (yes); i++)
    result |= len == (int)strlen (yes[i]) && !strncasecmp (value, yes[i], len);

  lua_pushboolean (L, result);
  return 1;
}

static const luaL_Reg terminal_functions[] = {
  { "new_bar",       rxvt_bar_new    },
  { "nrow",          t_nrow          },
  { "ncol",          t_ncol          },
  { "top_row",       t_top_row       },
  { "view_start",    t_view_start    },
  { "current_screen", t_current_screen },
  { "prompt_marker", t_prompt_marker },
  { "prompt_markers", t_prompt_markers },
  { "prompt_jump",   t_prompt_jump   },
  { "cursor",        t_cursor        },
  { "focus",         t_focus         },
  { "font_size",     t_font_size     },
  { "window",        t_window        },
  { "window_geometry", t_window_geometry },
  { "line",          t_line          },
  { "col",           t_col           },
  { "line_bounds",   t_line_bounds   },
  { "cells",         t_cells         },
  { "rend",          t_rend          },
  { "highlight",     t_highlight     },
  { "xor_span",      t_xor_span      },
  { "xor_rect",      t_xor_rect      },
  { "write",         t_write         },
#if defined (KEYSYM_RESOURCE)
  { "feed",          t_feed          },
#endif
  { "refresh",       t_refresh       },
  { "pty_events",    t_pty_events    },
  { "overlay",       t_overlay       },
  { "overlay_text",  t_overlay_text  },
  { "overlay_off",   t_overlay_off   },
  { "new_overlay",   t_new_overlay   },
  { "selection",     t_selection     },
  { "set_selection", t_set_selection },
  { "selection_beg", t_selection_beg },
  { "selection_end", t_selection_end },
  { "selection_mark", t_selection_mark },
  { "selection_make", t_selection_make },
  { "selection_grab", t_selection_grab },
  { "selection_clear", t_selection_clear },
  { "selection_request", t_selection_request },
  { "selection_fetch", t_selection_fetch },
  { "resource",      t_resource      },
  { "resource_boolean", t_resource_boolean },
  { 0, 0 }
};

/////////////////////////////////////////////////////////////////////////////
// runtime API

// Each nrxvt.api.on_* closure captures its hook ID once. Store callbacks directly
// in the dispatcher's arrays; only loading and rollback use hook_counts.
static int
u_on (lua_State *L)
{
  if (!config_loading)
    return luaL_error (L, "hooks can only be registered during startup");

  luaL_checktype (L, 1, LUA_TFUNCTION);
  int htype = lua_tointeger (L, lua_upvalueindex (1));

  if (!hook_refs[htype])
    {
      lua_newtable (L);
      hook_refs[htype] = luaL_ref (L, LUA_REGISTRYINDEX);
    }

  lua_rawgeti (L, LUA_REGISTRYINDEX, hook_refs[htype]);
  lua_pushvalue (L, 1);
  lua_rawseti (L, -2, ++hook_counts[htype]);
  return 0;
}

// nrxvt.api.bind (keyspec, fn) - calls fn (t) when keyspec (keysym resource
// syntax, e.g. "M-s" or "C-S-Up") is pressed. The key is consumed unless fn
// returns false. Like keysym resources, extra modifiers are ignored.
static int
u_bind (lua_State *L)
{
  const char *spec = luaL_checkstring (L, 1);
  luaL_checktype (L, 2, LUA_TFUNCTION);

  if (!cur_term)
    return luaL_error (L, "nrxvt.api.bind needs to be called from plugin or hook code");

  lua_binding b;
  int sym = cur_term->parse_keysym (spec, b.state);

  if (sym == -1)
    return luaL_error (L, "unable to parse key '%s'", spec);

  b.sym = sym;
  lua_pushvalue (L, 2);
  b.fn_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  bindings.push_back (b);

  rxvt_lua.should_invoke[HOOK_KEY_PRESS] = 1;
  return 0;
}

// nrxvt.api.style {fg=, bg=, bold=, italic=, underline=, reverse=, blink=}
//   -> and_mask, or_mask
// fg/bg are color indices (0-15 for the basic colors) or "default", the
// attributes are set (true) or cleared (false), omitted ones stay as they are.
static int
u_style (lua_State *L)
{
  luaL_checktype (L, 1, LUA_TTABLE);

  rend_t and_mask = ~(rend_t)0;
  rend_t or_mask = 0;

  static const struct { const char *name; rend_t bit; } attrs[] = {
    { "bold",      RS_Bold   },
    { "italic",    RS_Italic },
    { "underline", RS_Uline  },
    { "reverse",   RS_RVid   },
    { "blink",     RS_Blink  },
  };

  for (unsigned int i = 0; i < ecb_array_length (attrs); i++)
    {
      lua_getfield (L, 1, attrs[i].name);
      if (!lua_isnil (L, -1))
        {
          and_mask &= ~attrs[i].bit;
          if (lua_toboolean (L, -1))
            or_mask |= attrs[i].bit;
        }
      lua_pop (L, 1);
    }

  static const struct { const char *name; rend_t mask; int shift; int def; } colors[] = {
    { "fg", RS_fgMask, RS_fgShift, Color_fg },
    { "bg", RS_bgMask, RS_bgShift, Color_bg },
  };

  for (unsigned int i = 0; i < ecb_array_length (colors); i++)
    {
      lua_getfield (L, 1, colors[i].name);
      if (!lua_isnil (L, -1))
        {
          int color;

          if (lua_type (L, -1) == LUA_TSTRING && !strcmp (lua_tostring (L, -1), "default"))
            color = colors[i].def;
          else
            {
              int index = luaL_checkinteger (L, -1);

              if (!IN_RANGE_INC (index, 0, maxTermCOLOR - minCOLOR))
                return luaL_error (L, "%s color %d out of range [0, %d]", colors[i].name, index, maxTermCOLOR - minCOLOR);

              color = minCOLOR + index;
            }

          and_mask &= ~colors[i].mask;
          or_mask |= (rend_t)color << colors[i].shift;
        }
      lua_pop (L, 1);
    }

  lua_pushnumber (L, and_mask);
  lua_pushnumber (L, or_mask);
  return 2;
}

// nrxvt.api.spawn (argv) - starts a detached program, argv is a table of
// arguments or a string for /bin/sh -c. Returns whether fork succeeded.
static int
u_spawn (lua_State *L)
{
  // build the argument vector before forking, the child must not touch lua
  const char *sh_argv[] = { "/bin/sh", "-c", 0, 0 };
  const char **argv = sh_argv;

  if (lua_type (L, 1) == LUA_TSTRING)
    sh_argv[2] = lua_tostring (L, 1);
  else
    {
      luaL_checktype (L, 1, LUA_TTABLE);
      int n = lua_objlen (L, 1);

      if (n < 1)
        return luaL_error (L, "nrxvt.api.spawn needs a program to run");

      // lives on the lua stack, so it is freed even if an argument is bad
      argv = (const char **)lua_newuserdata (L, (n + 1) * sizeof (char *));

      for (int i = 0; i < n; i++)
        {
          lua_rawgeti (L, 1, i + 1);
          argv[i] = luaL_checkstring (L, -1);
          lua_pop (L, 1); // the table keeps the string alive
        }

      argv[n] = 0;
    }

  pid_t pid = fork ();

  if (pid == 0)
    {
      // double fork, so the program is reparented to init and never
      // becomes a zombie of the terminal
      if (fork () == 0)
        {
          setsid ();

          // don't leak the pty or the X connection into the program
          for (int fd = sysconf (_SC_OPEN_MAX) - 1; fd > 2; fd--)
            close (fd);

          signal (SIGCHLD, SIG_DFL);
          signal (SIGPIPE, SIG_DFL);

          execvp (argv[0], (char **)argv);
          _exit (127);
        }

      _exit (0);
    }

  if (pid > 0)
    // libev's child watcher may reap it before us, that is fine
    while (waitpid (pid, 0, 0) < 0 && errno == EINTR)
      ;

  lua_pushboolean (L, pid > 0);
  return 1;
}

// timers created by nrxvt.api.timer
struct lua_timer
{
  ev::timer w;
  lua_State *L;
  int fn_ref;
  int self_ref; // keeps the timer alive while it is active, 0 if not

  void cb (ev::timer &w, int revents);

  // Plugin timers must not keep urxvt running after the last terminal is
  // gone, so they are taken out of the reference count of the event loop
  // while active (see "ev_ref/ev_unref" in the libev documentation).
  void start ()
  {
    w.start ();
    ev_unref (EV_DEFAULT_UC);
  }

  void stop ()
  {
    if (w.is_active ())
      {
        ev_ref (EV_DEFAULT_UC);
        w.stop ();
      }
  }

  void unanchor ()
  {
    if (self_ref)
      {
        luaL_unref (L, LUA_REGISTRYINDEX, self_ref);
        self_ref = 0;
      }
  }
};

void
lua_timer::cb (ev::timer &w, int revents)
{
  // libev already stopped an expired one-shot timer, which dropped the
  // reference that start () had given back
  if (!w.is_active ())
    ev_ref (EV_DEFAULT_UC);

  lua_rawgeti (L, LUA_REGISTRYINDEX, fn_ref);
  pcall (L, 0, 0, "timer");

  // one-shot timers become collectable once they have fired, unless the
  // function restarted them
  if (!w.is_active ())
    unanchor ();
}

static lua_timer *
check_timer (lua_State *L)
{
  return (lua_timer *)luaL_checkudata (L, 1, "nrxvt.timer");
}

// timer:start ([after [, repeat]]) - (re)starts the timer
static int
timer_start (lua_State *L)
{
  lua_timer *timer = check_timer (L);

  // restarting an active timer starts it over with the full delay
  timer->stop ();

  if (!lua_isnoneornil (L, 2))
    timer->w.set (luaL_checknumber (L, 2), luaL_optnumber (L, 3, 0.));

  timer->start ();

  if (!timer->self_ref)
    {
      lua_pushvalue (L, 1);
      timer->self_ref = luaL_ref (L, LUA_REGISTRYINDEX);
    }

  return 0;
}

// timer:stop ()
static int
timer_stop (lua_State *L)
{
  lua_timer *timer = check_timer (L);
  timer->stop ();
  timer->unanchor ();
  return 0;
}

static int
timer_gc (lua_State *L)
{
  lua_timer *timer = check_timer (L);
  timer->stop ();
  luaL_unref (L, LUA_REGISTRYINDEX, timer->fn_ref);
  timer->~lua_timer ();
  return 0;
}

// nrxvt.api.timer (after, fn [, repeat]) -> timer
// Calls fn after `after` seconds and then every `repeat` seconds, if given.
// Active timers stay alive without references, stop them with timer:stop.
// They don't keep urxvt running once all terminals are closed.
static int
u_timer (lua_State *L)
{
  double after = luaL_checknumber (L, 1);
  luaL_checktype (L, 2, LUA_TFUNCTION);
  double repeat = luaL_optnumber (L, 3, 0.);

  lua_timer *timer = new (lua_newuserdata (L, sizeof (lua_timer))) lua_timer;
  luaL_getmetatable (L, "nrxvt.timer");
  lua_setmetatable (L, -2);

  timer->L = L;
  lua_pushvalue (L, 2);
  timer->fn_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  timer->self_ref = 0;
  timer->w.set<lua_timer, &lua_timer::cb> (timer);
  timer->w.set (after, repeat);

  lua_pushcfunction (L, timer_start);
  lua_pushvalue (L, -2);
  lua_call (L, 1, 0);

  return 1;
}

// nrxvt.api.warn (msg) - prints a message on stderr
static int
u_warn (lua_State *L)
{
  rxvt_warn ("%s\n", luaL_checkstring (L, 1));
  return 0;
}

// nrxvt.api.keysym_name (keysym) -> name of a keysym, e.g. "Return", or nil
static int
u_keysym_name (lua_State *L)
{
  lua_pushstring (L, XKeysymToString (luaL_checkinteger (L, 1)));
  return 1;
}

/////////////////////////////////////////////////////////////////////////////
// regular expressions (PCRE2), the replacement for the perl regexes

// a compiled pattern, the userdata of nrxvt.api.regex objects
struct lua_regex
{
  pcre2_code *code;
  pcre2_match_data *md; // reused by every match, plugins are single threaded
  uint32_t ncap;        // number of capture groups
};

static lua_regex *
check_regex (lua_State *L)
{
  return (lua_regex *)luaL_checkudata (L, 1, "nrxvt.regex");
}

static int
regex_gc (lua_State *L)
{
  lua_regex *re = check_regex (L);

  pcre2_match_data_free (re->md);
  pcre2_code_free (re->code);
  re->md = 0;
  re->code = 0;

  return 0;
}

// nrxvt.api.regex (pattern [, flags]) -> regex
// Compiles a perl compatible regular expression, raising an error if it is
// invalid. flags is a string of perl modifiers: i (caseless), m (multiline),
// s (dot matches newlines) and x (extended). Patterns and subjects are
// UTF-8 with Unicode semantics, i.e. \w, \s, [[:upper:]] and caseless
// matching know non-ASCII characters, like perl with character strings.
// Invalid UTF-8 in subjects never matches, but is no error.
static int
u_regex (lua_State *L)
{
  size_t len;
  const char *pattern = luaL_checklstring (L, 1, &len);
  const char *flags = luaL_optstring (L, 2, "");
  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF;

  for (const char *f = flags; *f; f++)
    switch (*f)
      {
        case 'i': options |= PCRE2_CASELESS;  break;
        case 'm': options |= PCRE2_MULTILINE; break;
        case 's': options |= PCRE2_DOTALL;    break;
        case 'x': options |= PCRE2_EXTENDED;  break;
        default:
          return luaL_error (L, "invalid regex flag '%c'", *f);
      }

  // created before compiling, so __gc frees everything if anything fails
  lua_regex *re = (lua_regex *)lua_newuserdata (L, sizeof (lua_regex));
  re->code = 0;
  re->md = 0;
  re->ncap = 0;
  luaL_getmetatable (L, "nrxvt.regex");
  lua_setmetatable (L, -2);

  int err;
  PCRE2_SIZE erroffset;
  re->code = pcre2_compile ((PCRE2_SPTR)pattern, len, options, &err, &erroffset, 0);

  if (!re->code)
    {
      PCRE2_UCHAR msg[256];
      pcre2_get_error_message (err, msg, sizeof (msg));
      return luaL_error (L, "invalid regex at offset %d: %s", (int)erroffset, (const char *)msg);
    }

  // the JIT is optional, matching falls back to the interpreter without it
  pcre2_jit_compile (re->code, PCRE2_JIT_COMPLETE);
  pcre2_pattern_info (re->code, PCRE2_INFO_CAPTURECOUNT, &re->ncap);

  re->md = pcre2_match_data_create_from_pattern (re->code, 0);
  if (!re->md)
    return luaL_error (L, "out of memory");

  return 1;
}

// Matches re against subject starting at byte offset, returns whether it
// matched, raising an error if matching failed (e.g. at a match limit).
static bool
regex_exec (lua_State *L, lua_regex *re, const char *subject, size_t len, size_t offset, uint32_t options)
{
  int rc = pcre2_match (re->code, (PCRE2_SPTR)subject, len, offset, options, re->md, 0);

  if (rc == PCRE2_ERROR_NOMATCH)
    return false;

  if (rc < 0)
    {
      PCRE2_UCHAR msg[256];
      pcre2_get_error_message (rc, msg, sizeof (msg));
      luaL_error (L, "regex match failed: %s", (const char *)msg);
    }

  return true;
}

// Pushes the results of the last match like string.find (find is true)
// or string.match, unset groups are false.
static int
regex_push (lua_State *L, lua_regex *re, const char *subject, bool find)
{
  PCRE2_SIZE *ov = pcre2_get_ovector_pointer (re->md);
  int n = 0;

  if (find)
    {
      lua_pushinteger (L, ov[0] + 1);
      lua_pushinteger (L, ov[1]);
      n = 2;
    }
  else if (!re->ncap)
    {
      lua_pushlstring (L, subject + ov[0], ov[1] - ov[0]);
      return 1;
    }

  luaL_checkstack (L, re->ncap, "too many captures");

  for (uint32_t i = 1; i <= re->ncap; i++)
    if (ov[2 * i] == PCRE2_UNSET)
      lua_pushboolean (L, 0);
    else
      lua_pushlstring (L, subject + ov[2 * i], ov[2 * i + 1] - ov[2 * i]);

  return n + re->ncap;
}

// The 0-based byte offset for the init argument at idx (1-based, negative
// counts from the end, like string.find), or -1 if it is past the end.
static lua_Integer
regex_init (lua_State *L, int idx, size_t len)
{
  lua_Integer init = luaL_optinteger (L, idx, 1);

  if (init < 0)
    init += len + 1;

  if (init < 1)
    init = 1;

  return init > (lua_Integer)len + 1 ? -1 : init - 1;
}

// re:find (s [, init]) -> start, end, captures... or nil, like string.find
// re:match (s [, init]) -> captures, or the whole match, or nil
static int
regex_find_match (lua_State *L, bool find)
{
  lua_regex *re = check_regex (L);
  size_t len;
  const char *subject = luaL_checklstring (L, 2, &len);
  lua_Integer offset = regex_init (L, 3, len);

  if (offset < 0 || !regex_exec (L, re, subject, len, offset, 0))
    {
      lua_pushnil (L);
      return 1;
    }

  return regex_push (L, re, subject, find);
}

static int regex_find  (lua_State *L) { return regex_find_match (L, true);  }
static int regex_match (lua_State *L) { return regex_find_match (L, false); }

// the iterator of re:gfind, the upvalues are the regex, the subject, the
// offset to continue at and whether the last match was empty
static int
regex_gfind_next (lua_State *L)
{
  lua_regex *re = (lua_regex *)lua_touserdata (L, lua_upvalueindex (1));
  size_t len;
  const char *subject = lua_tolstring (L, lua_upvalueindex (2), &len);
  lua_Integer offset = lua_tointeger (L, lua_upvalueindex (3));
  bool empty = lua_toboolean (L, lua_upvalueindex (4));

  if (offset < 0 || !re->code)
    return 0;

  bool found;

  if (empty)
    {
      // after an empty match, look for a non-empty one at the same place
      // first, then move on by a character, like perl's m//g
      found = regex_exec (L, re, subject, len, offset, PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED);

      if (!found && offset < (lua_Integer)len)
        {
          do
            offset++;
          while (offset < (lua_Integer)len && (subject[offset] & 0xc0) == 0x80);

          found = regex_exec (L, re, subject, len, offset, 0);
        }
    }
  else
    found = regex_exec (L, re, subject, len, offset, 0);

  if (!found)
    {
      lua_pushinteger (L, -1);
      lua_replace (L, lua_upvalueindex (3));
      return 0;
    }

  PCRE2_SIZE *ov = pcre2_get_ovector_pointer (re->md);
  lua_pushinteger (L, ov[1]);
  lua_replace (L, lua_upvalueindex (3));
  lua_pushboolean (L, ov[0] == ov[1]);
  lua_replace (L, lua_upvalueindex (4));

  return regex_push (L, re, subject, true);
}

// re:gfind (s [, init]) -> iterator over the non-overlapping matches,
// returning start, end, captures... for each, like re:find
static int
regex_gfind (lua_State *L)
{
  check_regex (L);
  size_t len;
  luaL_checklstring (L, 2, &len);

  lua_settop (L, 3);
  lua_pushinteger (L, regex_init (L, 3, len));
  lua_replace (L, 3);
  lua_pushboolean (L, 0);
  lua_pushcclosure (L, regex_gfind_next, 4);

  return 1;
}

static const luaL_Reg regex_methods[] = {
  { "find",  regex_find  },
  { "match", regex_match },
  { "gfind", regex_gfind },
  { 0, 0 }
};

// nrxvt.api.regex_escape (s) -> s with every ASCII character besides letters,
// digits and "_" escaped, so it matches literally, like perl's quotemeta
static int
u_regex_escape (lua_State *L)
{
  size_t len;
  const char *s = luaL_checklstring (L, 1, &len);
  luaL_Buffer b;

  luaL_buffinit (L, &b);

  for (size_t i = 0; i < len; i++)
    {
      unsigned char c = s[i];

      if (c == 0)
        // a backslash before a NUL would not be a valid escape
        luaL_addstring (&b, "\\x00");
      else
        {
          if (c < 0x80 && !isalnum (c) && c != '_')
            luaL_addchar (&b, '\\');

          luaL_addchar (&b, c);
        }
    }

  luaL_pushresult (&b);
  return 1;
}

static const luaL_Reg api_functions[] = {
  { "bind",        u_bind        },
  { "regex",       u_regex       },
  { "regex_escape", u_regex_escape },
  { "style",       u_style       },
  { "spawn",       u_spawn       },
  { "timer",       u_timer       },
  { "warn",        u_warn        },
  { "keysym_name", u_keysym_name },
  { 0, 0 }
};

/////////////////////////////////////////////////////////////////////////////
// plugin loading

// Typed option proxies retain Lua values without shadowing __newindex.
static int
config_index (lua_State *L)
{
  const char *name = luaL_checkstring (L, 2);
  int option, type;
  if (rxvt_term::config_resource (name, option, type) < 0)
    return luaL_error (L, "unknown or unavailable option '%s'", name);

  lua_pushvalue (L, 2);
  lua_rawget (L, lua_upvalueindex (1));
  return 1;
}

static int
config_newindex (lua_State *L)
{
  if (!config_loading)
    return luaL_error (L, "configuration can only be changed during startup");

  size_t len;
  const char *name = luaL_checklstring (L, 2, &len);
  if (!len || len > 400 || memchr (name, 0, len))
    return luaL_error (L, "invalid resource name");
  for (size_t i = 0; i < len; i++)
    if (!isalnum ((unsigned char)name[i]) && !strchr ("._-+", name[i]))
      return luaL_error (L, "invalid resource name '%s'", name);

  int option, type;
  int resource = rxvt_term::config_resource (name, option, type);
  if (resource < 0)
    return luaL_error (L, "unknown or unavailable option '%s'", name);

  if (!lua_isnil (L, 3))
    {
      int value_type = lua_type (L, 3);
      int expected = type == 2 ? LUA_TBOOLEAN : type == 3 ? LUA_TNUMBER : LUA_TSTRING;
      if (value_type != expected)
        return luaL_error (L, "invalid value type for '%s'", name);
      if (value_type == LUA_TNUMBER && !isfinite (lua_tonumber (L, 3)))
        return luaL_error (L, "'%s' requires a finite number", name);
      if (value_type == LUA_TSTRING)
        {
          size_t length;
          const char *value = lua_tolstring (L, 3, &length);
          if (memchr (value, 0, length))
            return luaL_error (L, "'%s' cannot contain NUL bytes", name);
        }
    }

  lua_pushvalue (L, 2);
  lua_pushvalue (L, 3);
  lua_rawset (L, lua_upvalueindex (1));
  return 0;
}

// Keep the backing tables private so all normal assignments are validated.
static void
register_config (lua_State *L)
{
  lua_newtable (L);
  config_options_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  lua_newtable (L);
  lua_newtable (L);
  lua_rawgeti (L, LUA_REGISTRYINDEX, config_options_ref);
  lua_pushcclosure (L, config_index, 1);
  lua_setfield (L, -2, "__index");
  lua_rawgeti (L, LUA_REGISTRYINDEX, config_options_ref);
  lua_pushcclosure (L, config_newindex, 1);
  lua_setfield (L, -2, "__newindex");
  lua_pushliteral (L, "configuration");
  lua_setfield (L, -2, "__metatable");
  lua_setmetatable (L, -2);
  lua_setfield (L, -2, "opt");
}

// Apply defaults before Xresources are extracted. Existing resource slots
// and matching -xrm entries are explicit command-line overrides.
static void
apply_config (lua_State *L, rxvt_term *term, int ref)
{
  lua_rawgeti (L, LUA_REGISTRYINDEX, ref);
  lua_pushnil (L);
  while (lua_next (L, -2))
    {
      const char *name = lua_tostring (L, -2);
      int option, type;
      int resource = rxvt_term::config_resource (name, option, type);
      bool overridden = resource >= 0 && term->rs[resource];
      const char *explicit_value = 0;
#ifndef NO_RESOURCES
      if (term->option_db)
        explicit_value = term->x_resource (name, term->option_db);
#endif
      if (!overridden && (!explicit_value || resource >= 0))
        {
          // Convert a copy: lua_next requires its key to remain unchanged.
          lua_pushvalue (L, -1);
          const char *value = explicit_value ? explicit_value : lua_isboolean (L, -1)
                              ? lua_toboolean (L, -1) ? "true" : "false"
                              : lua_tostring (L, -1);
          if (resource >= 0)
            {
              char *copy = strdup (value);
              term->allocated.push_back (copy);
              term->rs[resource] = copy;
              if (type == 2)
                term->set_option (option, !strcasecmp (value, "true") || !strcasecmp (value, "yes")
                                         || !strcasecmp (value, "on") || !strcmp (value, "1"));
            }

          // Explicit resources are retained separately, so extension and
          // extended-palette overrides keep their original precedence.
          if (!explicit_value)
            {
              char *key = rxvt_temp_buf<char> (strlen (term->rs[Rs_name]) + strlen (name) + 2);
              sprintf (key, "%s.%s", term->rs[Rs_name], name);
              XrmPutStringResource (&term->lua.resources, key, value);
            }
          lua_pop (L, 1);
        }
      lua_pop (L, 1);
    }
  lua_pop (L, 1);
}

// $XDG_CONFIG_HOME/nrxvt, or ~/.config/nrxvt
static bool
config_dir (char *buf, size_t size)
{
  const char *config = getenv ("XDG_CONFIG_HOME");

  if (config && *config == '/')
    return snprintf (buf, size, "%s/nrxvt", config) < (int)size;

  const char *home = getenv ("HOME");

  if (!home || !*home)
    {
      struct passwd *pw = getpwuid (getuid ());
      home = pw ? pw->pw_dir : 0;
    }

  return home && snprintf (buf, size, "%s/.config/nrxvt", home) < (int)size;
}

static int
compare_names (const void *a, const void *b)
{
  return strcmp (*(char *const *)a, *(char *const *)b);
}

// Collects the plugin file names of dir, sorted. Returns 0 if there are
// none, otherwise a 0-terminated array to free with free_names.
static char **
find_plugins (const char *dir)
{
  DIR *d = opendir (dir);

  if (!d)
    return 0;

  vector<char *> names;

  while (struct dirent *e = readdir (d))
    {
      size_t len = strlen (e->d_name);

      // skip hidden files, e.g. editor backups
      if (e->d_name[0] != '.' && len > 4 && !strcmp (e->d_name + len - 4, ".lua"))
        names.push_back (strdup (e->d_name));
    }

  closedir (d);

  if (names.empty ())
    return 0;

  char **result = (char **)malloc ((names.size () + 1) * sizeof (char *));
  memcpy (result, &names[0], names.size () * sizeof (char *));
  result[names.size ()] = 0;
  qsort (result, names.size (), sizeof (char *), compare_names);

  return result;
}

// Registrations become active only after the whole file succeeds. On failure,
// remove its appended callbacks so earlier plugins keep their original arrays.
static bool
load_hooks (lua_State *L, const char *path, const char *name)
{
  if (luaL_loadfile (L, path))
    {
      rxvt_warn ("lua: %s\n", lua_tostring (L, -1));
      lua_pop (L, 1);
      return false;
    }

  int previous_counts[HOOK_NUM];
  memcpy (previous_counts, hook_counts, sizeof (previous_counts));
  bool ok = pcall (L, 0, 1, name);

  if (ok)
    {
      if (!lua_isnil (L, -1))
        {
          rxvt_warn ("lua: %s: register hooks with nrxvt.api.on_* instead of returning a value.\n", name);
          ok = false;
        }

      lua_pop (L, 1);
    }

  for (int htype = 0; htype < HOOK_NUM; htype++)
    if (hook_counts[htype] != previous_counts[htype])
      {
        if (ok)
          {
            rxvt_lua.should_invoke[htype] = 1;
            continue;
          }

        lua_rawgeti (L, LUA_REGISTRYINDEX, hook_refs[htype]);
        while (hook_counts[htype] > previous_counts[htype])
          {
            lua_pushnil (L);
            lua_rawseti (L, -2, hook_counts[htype]--);
          }
        lua_pop (L, 1);

        if (!hook_counts[htype])
          {
            luaL_unref (L, LUA_REGISTRYINDEX, hook_refs[htype]);
            hook_refs[htype] = 0;
          }
      }

  return ok;
}

static void
create_state (rxvt_term *term, const char *config, const char *dir, const char *init, char **plugins)
{
  lua_State *L = luaL_newstate ();

  if (!L)
    {
      rxvt_warn ("lua: unable to create the lua state, plugins disabled.\n");
      return;
    }

  luaL_openlibs (L);
  rxvt_bar_register (L);

  for (int htype = 0; htype < HOOK_NUM; htype++)
    {
      snprintf (hook_names[htype], sizeof (hook_names[htype]), "on_%s", hook_upper_names[htype]);
      for (char *p = hook_names[htype]; *p; p++)
        *p = tolower (*p);
    }

  lua_getglobal (L, "debug");
  lua_getfield (L, -1, "traceback");
  traceback_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  lua_pop (L, 1);

  luaL_newmetatable (L, "nrxvt.timer");
  lua_newtable (L);
  lua_pushcfunction (L, timer_start);
  lua_setfield (L, -2, "start");
  lua_pushcfunction (L, timer_stop);
  lua_setfield (L, -2, "stop");
  lua_setfield (L, -2, "__index");
  lua_pushcfunction (L, timer_gc);
  lua_setfield (L, -2, "__gc");
  lua_pop (L, 1);

  luaL_newmetatable (L, "nrxvt.overlay");
  lua_newtable (L);
  luaL_register (L, 0, overlay_methods);
  lua_setfield (L, -2, "__index");
  lua_pushcfunction (L, overlay_close);
  lua_setfield (L, -2, "__gc");
  lua_pop (L, 1);

  luaL_newmetatable (L, "nrxvt.regex");
  lua_newtable (L);
  luaL_register (L, 0, regex_methods);
  lua_setfield (L, -2, "__index");
  lua_pushcfunction (L, regex_gc);
  lua_setfield (L, -2, "__gc");
  lua_pop (L, 1);

  // Keep startup settings separate from the runtime API namespace.
  lua_newtable (L);
  register_config (L);
  lua_newtable (L);
  luaL_register (L, 0, api_functions);
  luaL_register (L, 0, terminal_functions);
  for (int htype = 0; htype < HOOK_NUM; htype++)
    {
      lua_pushinteger (L, htype);
      lua_pushcclosure (L, u_on, 1);
      lua_setfield (L, -2, hook_names[htype]);
    }
  lua_pushstring (L, VERSION);
  lua_setfield (L, -2, "version");
  lua_pushstring (L, dir);
  lua_setfield (L, -2, "plugin_dir");

  // Event and rendition masks used by the terminal API functions.
  static const struct { const char *name; lua_Number value; } constants[] = {
    { "EV_NONE",   ev::NONE  },
    { "EV_READ",   ev::READ  },
    { "EV_WRITE",  ev::WRITE },
    { "RS_Bold",   RS_Bold   },
    { "RS_Italic", RS_Italic },
    { "RS_Blink",  RS_Blink  },
    { "RS_RVid",   RS_RVid   },
    { "RS_Uline",  RS_Uline  },
    { "RS_Sel",    RS_Sel    },
  };

  for (unsigned int i = 0; i < ecb_array_length (constants); i++)
    {
      lua_pushnumber (L, constants[i].value);
      lua_setfield (L, -2, constants[i].name);
    }

  lua_setfield (L, -2, "api");
  lua_setglobal (L, "nrxvt");

  // Configuration modules and plugin helpers are never autoloaded.
  lua_getglobal (L, "package");
  lua_pushfstring (L, "%s/lua/?.lua;%s/lua/?/init.lua;%s/lib/?.lua;", config, config, dir);
  lua_getfield (L, -2, "path");
  lua_concat (L, 2);
  lua_setfield (L, -2, "path");
  lua_pop (L, 1);

  rxvt_lua.L = L;

  // the terminal is not usable before its lua object exists, which needs
  // the state, but keysym parsing in nrxvt.api.bind already works
  cur_term = term;
  config_loading = true;

  if (init && !load_hooks (L, init, init))
    rxvt_fatal ("lua: %s failed, aborting.\n", init);

  for (char **name = plugins; name && *name; name++)
    {
      char path[PATH_MAX];
      snprintf (path, sizeof (path), "%s/%s", dir, *name);

      load_hooks (L, path, *name);
    }

  config_loading = false;
  cur_term = 0;

  // the interpreter needs these to manage the terminal objects and the
  // temporary highlights, regardless of the plugins
  rxvt_lua.should_invoke[HOOK_INIT] = 1;
  rxvt_lua.should_invoke[HOOK_DESTROY] = 1;

  if (rxvt_lua.should_invoke[HOOK_REFRESH_BEGIN])
    rxvt_lua.should_invoke[HOOK_REFRESH_END] = 1;

  // motion events without pressed buttons need to be enabled on start
  if (rxvt_lua.should_invoke[HOOK_MOTION_NOTIFY])
    rxvt_lua.should_invoke[HOOK_START] = 1;
}

bool
rxvt_lua_interp::init (rxvt_term *term)
{
  // Load the configuration and plugins once; retain defaults for later
  // terminals without registering their hooks or key bindings twice.
  if (!L)
    {
      char config[PATH_MAX], dir[PATH_MAX], init[PATH_MAX];

      if (!config_dir (config, sizeof (config))
          || snprintf (dir, sizeof (dir), "%s/plugins", config) >= (int)sizeof (dir))
        return false;

      // An explicit config is required; only the default file is optional.
      // Keep plugin and helper-module lookup rooted in the usual directory.
      const char *init_path = term->rs[Rs_lua_config];
      if (!init_path || init_path == rxvt_term::resval_undef)
        {
          if (snprintf (init, sizeof (init), "%s/init.lua", config) >= (int)sizeof (init))
            return false;

          init_path = access (init, F_OK) == 0 || errno != ENOENT ? init : 0;
        }

      char **plugins = find_plugins (dir);

      if (!plugins && !init_path)
        return false;

      create_state (term, config, dir, init_path, plugins);

      for (char **name = plugins; name && *name; name++)
        free (*name);
      free (plugins);

      if (!L)
        return false;
    }

  apply_config (L, term, config_options_ref);

  // the terminal object, a table so plugins can store per-terminal state
  // in it, with the terminal pointer at index 0
  lua_createtable (L, 0, 1);
  lua_pushlightuserdata (L, term);
  lua_rawseti (L, -2, 0);
  term->lua.ref = luaL_ref (L, LUA_REGISTRYINDEX);

  return true;
}

/////////////////////////////////////////////////////////////////////////////
// hook invocation

// Calls the nrxvt.api.bind function matching a key press, if any. Returns
// whether the key was consumed.
static bool
invoke_binding (lua_State *L, rxvt_term *term, XEvent *xe, KeySym sym)
{
  // map the state like keyboard_manager::lookup_keysym
  unsigned int state = xe->xkey.state & OtherModMask;

  if (state & term->ModMetaMask)    state |= MetaMask;
  if (state & term->ModNumLockMask) state |= NumLockMask;
  if (state & term->ModLevel3Mask)  state |= Level3Mask;

  if (!!(term->priv_modes & PrivMode_aplKP) != !!(state & ShiftMask))
    state |= AppKeypadMask;

  for (unsigned int i = 0; i < bindings.size (); i++)
    if (bindings[i].sym == sym && (bindings[i].state & state) == bindings[i].state)
      {
        lua_rawgeti (L, LUA_REGISTRYINDEX, bindings[i].fn_ref);
        lua_rawgeti (L, LUA_REGISTRYINDEX, term->lua.ref);

        if (!pcall (L, 1, 1, "key binding"))
          return true;

        bool consumed = !(lua_isboolean (L, -1) && !lua_toboolean (L, -1));
        lua_pop (L, 1);
        return consumed;
      }

  return false;
}

bool
rxvt_lua_interp::call (rxvt_term *term, int nargs, const char *what)
{
  rxvt_term *previous = cur_term;
  cur_term = term;
  bool ok = pcall (L, nargs, 0, what);
  cur_term = previous;
  return ok;
}

bool
rxvt_lua_interp::invoke (rxvt_term *term, int htype, ...)
{
  if (!should_invoke[htype] || !term->lua.ref)
    return false;

  int base = lua_gettop (L);
  rxvt_term *prev_term = cur_term;
  cur_term = term;

  bool consumed = false;

  // pre-handling
  if (htype == HOOK_REFRESH_BEGIN)
    refresh_term = term;
  else if (htype == HOOK_REFRESH_END)
    // the plugins see the screen without the overlays again
    swap_overlays (term, false);
  else if (htype == HOOK_START && should_invoke[HOOK_MOTION_NOTIFY])
    {
      term->vt_emask_perl |= PointerMotionMask;
      term->vt_select_input ();
    }
  else if (htype == HOOK_KEY_PRESS && bindings.size ())
    {
      va_list ap;
      va_start (ap, htype);
      va_arg (ap, int);
      XEvent *xe = va_arg (ap, XEvent *);
      va_arg (ap, int);
      KeySym sym = va_arg (ap, int);
      va_end (ap);

      consumed = invoke_binding (L, term, xe, sym);
    }

  // call the hook functions of all plugins until one consumes the event
  if (!consumed && hook_refs[htype])
    {
      lua_rawgeti (L, LUA_REGISTRYINDEX, hook_refs[htype]);
      int handlers = lua_gettop (L);

      lua_rawgeti (L, LUA_REGISTRYINDEX, term->lua.ref);

      va_list ap;
      va_start (ap, htype);
      int nargs = push_args (L, term, ap) + 1;
      va_end (ap);

      int args = handlers + 1;
      int n = lua_objlen (L, handlers);

      for (int i = 1; i <= n && !consumed; i++)
        {
          lua_rawgeti (L, handlers, i);

          // handlers that failed before have been disabled
          if (!lua_isfunction (L, -1))
            {
              lua_pop (L, 1);
              continue;
            }

          for (int arg = 0; arg < nargs; arg++)
            lua_pushvalue (L, args + arg);

          if (pcall (L, nargs, 1, hook_names[htype]))
            {
              consumed = lua_toboolean (L, -1);
              lua_pop (L, 1);
            }
          else
            {
              // an error is most likely going to repeat on every call,
              // e.g. for every refresh, so report it only once
              rxvt_warn ("lua: disabling this %s handler.\n", hook_names[htype]);
              lua_pushboolean (L, 0);
              lua_rawseti (L, handlers, i);
            }
        }
    }

  lua_settop (L, base);

  // post-handling
  if (htype == HOOK_REFRESH_BEGIN)
    // after the highlights of the plugins, so they are not drawn on top
    swap_overlays (term, true);
  else if (htype == HOOK_REFRESH_END)
    {
      // restore in reverse order, so overlapping highlights end up with
      // the original rendition
      for (int i = highlights.size (); i--; )
        ROW_of (term, highlights[i].row).r[highlights[i].col] = highlights[i].rend;

      highlights.clear ();
      refresh_term = 0;
    }
  else if (htype == HOOK_DESTROY)
    {
      // Cancel gestures while the terminal object is still valid in Lua.
      delete term->lua.bars;
      term->lua.bars = 0;

      // overlays can't outlive their terminal, closing removes them from
      // the list, so search from the start again each time
      for (unsigned int i = 0; i < overlays.size (); )
        if (overlays[i]->term == term)
          overlays[i]->close (L);
        else
          i++;

      // neither can fetches, their functions are not called
      for (unsigned int i = 0; i < fetches.size (); )
        if (fetches[i]->term == term)
          fetch_free (L, fetches[i]);
        else
          i++;

      // invalidate the object, plugins might still hold references to it
      lua_rawgeti (L, LUA_REGISTRYINDEX, term->lua.ref);
      lua_pushnil (L);
      lua_rawseti (L, -2, 0);
      lua_pop (L, 1);
      luaL_unref (L, LUA_REGISTRYINDEX, term->lua.ref);
      term->lua.ref = 0;
    }

  cur_term = prev_term;
  return consumed;
}

#endif
