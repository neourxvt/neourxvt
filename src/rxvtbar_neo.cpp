// A complete Lua declaration is normalized into reusable native storage.
// Stable IDs preserve node identity; layout and paint run only for differences.
#include "../config.h"

#if ENABLE_LUA
#include <algorithm>
#include <cmath>
#include <climits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "rxvt.h"
#include <lua.hpp>
#include <X11/cursorfont.h>

namespace {

struct bar_node
{
  std::string id, text;
  std::vector<int> children;
  std::vector<text_t> cells;
  std::vector<int> fonts;
  int parent = -1, depth = 0, bg = Color_bg, fg = Color_fg;
  bool pixels = false, layout_dirty = false;
  double size = 0;
  int64_t start = 0, length = 0;
  int lo = 0, hi = 0;
  uint64_t identity = 0;
};

struct span { int lo, hi; };
typedef std::unordered_map<std::string, int> node_index;
struct bar;
typedef std::shared_ptr<bar> bar_ptr;
struct bar_handle { bar_ptr value; };

// Validation throws only inside the protected C++ entry point. lua_error is
// called after unwinding, never across strings/vectors with live destructors.
static void require (bool ok, const char *message)
{
  if (!ok) throw message;
}

static void field (lua_State *L, int table, const char *name)
{
  lua_pushstring (L, name);
  lua_rawget (L, table);
}

static void keys (lua_State *L, int table, const char *const *allowed)
{
  lua_pushnil (L);
  while (lua_next (L, table))
    {
      require (lua_type (L, -2) == LUA_TSTRING, "bar fields must be strings");
      const char *key = lua_tostring (L, -2);
      require (lua_objlen (L, -2) == strlen (key), "bar field contains a NUL byte");
      int i = 0;
      while (allowed[i] && strcmp (key, allowed[i])) ++i;
      require (allowed[i], "unknown bar field");
      lua_pop (L, 1);
    }
}

static double number (lua_State *L, int index)
{
  require (lua_type (L, index) == LUA_TNUMBER, "expected a bar number");
  double value = lua_tonumber (L, index);
  require (std::isfinite (value), "bar numbers must be finite");
  return value;
}

static int integer (lua_State *L, int index, int low, int high)
{
  double value = number (L, index);
  require (value >= low && value <= high && std::floor (value) == value,
           "bar integer out of range");
  return int (value);
}

// Node colors are either terminal palette slots or fixed "#RRGGBB" values.
// The flag lies above every palette slot, so a node color stays one plain
// value: comparing two of them detects any change, including RGB edits.
static const int rgb_color = 1 << 24;

static int color (lua_State *L, int table, const char *name, int fallback)
{
  field (L, table, name);
  int result = fallback;
  if (lua_type (L, -1) == LUA_TSTRING)
    {
      const char *s = lua_tostring (L, -1);
      require (lua_objlen (L, -1) == strlen (s), "invalid bar color");
      if (s[0] == '#')
        {
          require (strlen (s) == 7 && strspn (s + 1, "0123456789abcdefABCDEF") == 6,
                   "bar hex colors must be #RRGGBB");
          result = rgb_color | int (strtol (s + 1, 0, 16));
        }
      else
        {
          require (!strcmp (s, "foreground") || !strcmp (s, "background"),
                   "expected foreground, background, #RRGGBB, or a palette index");
          result = !strcmp (s, "foreground") ? Color_fg : Color_bg;
        }
    }
  else if (!lua_isnil (L, -1))
    result = minCOLOR + integer (L, -1, 0, maxTermCOLOR - minCOLOR);
  lua_pop (L, 1);
  return result;
}

// Strict UTF-8 decoding is independent of the current locale. The resulting
// cells use the same wide/combining representation as terminal screen cells.
static uint32_t codepoint (const std::string &s, size_t &i)
{
  unsigned char first = s[i++];
  if (first < 0x80) return first;
  int count = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : 1;
  require (first >= 0xc2 && first <= 0xf4 && i + count <= s.size (), "invalid bar UTF-8");
  uint32_t value = first & ((1 << (6 - count)) - 1);
  for (int j = 0; j < count; ++j)
    {
      unsigned char c = s[i++];
      require ((c & 0xc0) == 0x80, "invalid bar UTF-8");
      value = (value << 6) | (c & 63);
    }
  require (value >= (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u)
           && value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff), "invalid bar UTF-8");
  return value;
}

struct bar
{
  rxvt_bar_manager *manager;
  rxvt_term *term;
  int edge, thickness, priority, self_ref = 0, callback = 0;
  bool shown, surface_changed = true, resize_event = false, retained_valid = false;
  int x = 0, y = 0, width = 0, height = 0, surface_w = 0, surface_h = 0;
  Window window = 0;
  Pixmap pixmap = 0;
  GC gc = 0;
  Cursor cursor = 0;
  rxvt_drawable *drawable = 0;
  xevent_watcher watcher;
  std::vector<bar_node> nodes, scratch;
  node_index index, scratch_index;
  size_t used = 0;
  uint64_t serial = 0;
  std::vector<int> layouts;
  std::vector<span> damage;
  uint64_t captured = 0;
  std::string capture_key;
  unsigned int captured_button = 0;
  const XEvent *press = 0;
  uint64_t press_target = 0;
  // Allocated "#RRGGBB" colors, keyed by their node color value. Entries are
  // added before a declaration is accepted and dropped once unreferenced.
  std::unordered_map<int, rxvt_color> rgb;
  // Font drawing takes palette slots, so RGB text is drawn through a copy of
  // the terminal palette with the text color appended after its last slot.
  std::vector<rxvt_color> palette;

  bar (rxvt_bar_manager *manager, int edge, int thickness, int priority, bool shown)
  : manager (manager), term (manager->term), edge (edge), thickness (thickness),
    priority (priority), shown (shown)
  {
    nodes.resize (1);
    nodes[0].identity = ++serial;
    nodes[0].layout_dirty = true;
    layouts.push_back (0);
  }

  bool vertical () const { return edge == BAR_LEFT || edge == BAR_RIGHT; }
  int length () const { return vertical () ? height : width; }
  void wake ();
  void mark (int lo, int hi)
  {
    lo = max (lo, 0); hi = min (hi, length ());
    if (hi > lo) { damage.push_back ({lo, hi}); wake (); }
  }
  void mark (const bar_node &n) { mark (n.lo, n.hi); }
  void full () { mark (0, length ()); }
  int parse (lua_State *L, int table, int parent, int depth);
  void alloc_colors ();
  void accept ();
  void prune_colors ();
  void free_colors ();
  Pixel pixel (int c) const
  {
    if (!(c & rgb_color)) return term->pix_colors[c];
    auto found = rgb.find (c);
    // Accepted nodes always have an entry; this only guards invariants.
    return found != rgb.end () ? Pixel (found->second) : Pixel (term->pix_colors[Color_bg]);
  }
  void layout (int id, bool changed);
  void ensure_layout ();
  void prepare_text (bar_node &n);
  void clip (int lo, int hi);
  void render_node (int id, int lo, int hi);
  void paint ();
  void surface ();
  int hit (int position) const;
  void event (const char *type, int target, const XEvent *ev = 0, const char *reason = 0);
  void cancel (const char *reason);
  void close ();
};

} // namespace

struct rxvt_bar_state
{
  std::vector<bar_ptr> bars;
  bool work = false, shutting_down = false, order_dirty = true, servicing = false;
  int insets[4] = {};
  unsigned int pressed = 0; // suppress orphan releases after a bar is hidden/closed
};

namespace {

void bar::wake ()
{
  if (manager->s->shutting_down) return;
  manager->s->work = true;
  rxvt_term *t = manager->term;
  // refresh_check also flushes X output. Scheduling directly keeps repeated
  // damage marks and batched submissions from flushing once per changed node.
  if (!manager->s->servicing && t->parent && t->mapped
      && !t->sync_ev.is_active () && !t->flush_ev.is_active ())
    t->flush_ev.start (t->batch_pending && t->option (Opt_refreshBatching)
                      ? t->refresh_delay () : t->refresh_interval);
}

int bar::parse (lua_State *L, int table, int parent, int depth)
{
  require (depth <= 64 && lua_istable (L, table), "invalid bar node or nesting exceeds 64");
  require (lua_checkstack (L, 12), "bar declaration exceeds Lua stack capacity");
  static const char *const root_keys[] = {"bg", "fg", "text", "children", 0};
  static const char *const node_keys[] = {"id", "size", "bg", "fg", "text", "children", 0};
  keys (L, table, parent < 0 ? root_keys : node_keys);
  int id = used++;
  if (scratch.size () <= size_t (id)) scratch.resize (id + 1);
  // Recursive parsing may grow scratch, so never retain references across it.
  bar_node &n = scratch[id];
  n.parent = parent; n.depth = depth; n.children.clear ();
  n.bg = color (L, table, "bg", Color_bg);
  n.fg = color (L, table, "fg", Color_fg);
  n.id.clear (); n.text.clear (); n.size = 0; n.pixels = false;
  if (parent >= 0)
    {
      field (L, table, "id");
      require (lua_type (L, -1) == LUA_TSTRING, "bar node requires a string ID");
      size_t len;
      const char *value = lua_tolstring (L, -1, &len);
      require (len && !memchr (value, 0, len), "bar ID must be nonempty without NUL bytes");
      n.id.assign (value, len);
      auto found = scratch_index.find (n.id);
      require (found == scratch_index.end () || found->second == -1, "duplicate bar ID");
      if (found == scratch_index.end ()) scratch_index.emplace (n.id, id);
      else found->second = id;
      lua_pop (L, 1);
      field (L, table, "size");
      require (lua_istable (L, -1), "bar node requires a size table");
      int size_table = lua_gettop (L);
      static const char *const size_keys[] = {"px", "ratio", 0};
      keys (L, size_table, size_keys);
      field (L, size_table, "px");
      field (L, size_table, "ratio");
      require (lua_isnil (L, -1) != lua_isnil (L, -2), "specify exactly one of px and ratio");
      n.pixels = lua_isnil (L, -1);
      n.size = n.pixels ? integer (L, -2, 0, 32767) : number (L, -1);
      require (n.size >= 0, "bar ratio must be nonnegative");
      lua_pop (L, 3);
    }
  field (L, table, "text");
  if (!lua_isnil (L, -1))
    {
      require (lua_type (L, -1) == LUA_TSTRING, "bar text must be a UTF-8 string");
      size_t len;
      const char *value = lua_tolstring (L, -1, &len);
      require (len <= 65535, "bar text exceeds 65535 bytes");
      n.text.assign (value, len);
      for (size_t i = 0; i < n.text.size (); )
        {
          uint32_t c = codepoint (n.text, i);
          require (c >= 32 && !(c >= 127 && c < 160), "bar text must be a single line without control characters");
        }
    }
  lua_pop (L, 1);
  field (L, table, "children");
  if (!lua_isnil (L, -1))
    {
      require (lua_istable (L, -1), "bar children must be a dense array");
      int array = lua_gettop (L);
      size_t count = lua_objlen (L, array), entries = 0;
      require (count <= INT_MAX, "too many bar children");
      lua_pushnil (L);
      while (lua_next (L, array))
        {
          integer (L, -2, 1, count);
          ++entries;
          lua_pop (L, 1);
        }
      require (entries == count, "bar children must be a dense array");
      for (size_t i = 1; i <= count; ++i)
        {
          lua_rawgeti (L, array, i);
          int child = parse (L, lua_gettop (L), id, depth + 1);
          scratch[id].children.push_back (child);
          lua_pop (L, 1);
        }
    }
  lua_pop (L, 1);
  return id;
}

// Runs between parsing and acceptance, so a failed allocation rejects the
// whole declaration. Colors allocated before the failure are pruned later.
void bar::alloc_colors ()
{
  bool any = false;
  for (size_t id = 0; id < used; ++id)
    for (int c : {scratch[id].bg, scratch[id].fg})
      {
        if (!(c & rgb_color)) continue;
        any = true;
        if (rgb.count (c)) continue;
        // Expand 8-bit channels to X's 16-bit range (0xff -> 0xffff).
        rgba value (((c >> 16) & 0xff) * 0x101, ((c >> 8) & 0xff) * 0x101, (c & 0xff) * 0x101);
        rxvt_color allocated;
        require (allocated.set (term, value), "bar color allocation failed");
        rgb.emplace (c, allocated);
      }
  // Reserve here, where allocation failure is reported to Lua, so painting
  // RGB text never allocates.
  if (any) palette.reserve (TOTAL_COLORS + 1);
}

// Frees allocations no accepted node references any more.
void bar::prune_colors ()
{
  if (rgb.empty ()) return;
  std::vector<int> live;
  for (const bar_node &n : nodes)
    for (int c : {n.bg, n.fg})
      if (c & rgb_color) live.push_back (c);
  for (auto i = rgb.begin (); i != rgb.end (); )
    if (std::find (live.begin (), live.end (), i->first) == live.end ())
      { i->second.free (term); i = rgb.erase (i); }
    else ++i;
}

void bar::free_colors ()
{
  for (auto &entry : rgb) entry.second.free (term);
  rgb.clear ();
}

void bar::accept ()
{
  // The scratch ID map is reused across frames; discard only obsolete keys.
  for (auto i = scratch_index.begin (); i != scratch_index.end (); )
    if (i->second < 0) i = scratch_index.erase (i); else ++i;
  scratch.resize (used);
  layouts.clear ();
  for (size_t id = 0; id < scratch.size (); ++id)
    {
      bar_node &n = scratch[id];
      auto found = index.find (n.id);
      int old_id = id == 0 ? 0 : found == index.end () ? -1 : found->second;
      n.layout_dirty = true;
      n.start = n.length = n.lo = n.hi = 0;
      n.cells.clear (); n.fonts.clear ();
      if (old_id >= 0)
        {
          bar_node &old = nodes[old_id];
          n.identity = old.identity;
          n.start = old.start; n.length = old.length; n.lo = old.lo; n.hi = old.hi;
          n.layout_dirty = old.layout_dirty || old.children.size () != n.children.size ();
          if (!n.layout_dirty)
            for (size_t k = 0; k < n.children.size (); ++k)
              {
                const bar_node &a = nodes[old.children[k]], &b = scratch[n.children[k]];
                if (a.id != b.id || a.size != b.size || a.pixels != b.pixels)
                  { n.layout_dirty = true; break; }
              }
          if (old.text == n.text)
            { n.cells.swap (old.cells); n.fonts.swap (old.fonts); }
          if (old.bg != n.bg || old.fg != n.fg || old.text != n.text) mark (old);
          if (old.parent >= 0 && nodes[old.parent].id != scratch[n.parent].id)
            mark (old); // Reparenting must erase the old parent's uncovered area.
        }
      else n.identity = ++serial;
      if (n.layout_dirty) layouts.push_back (id);
    }
  for (size_t i = 1; i < nodes.size (); ++i)
    if (scratch_index.find (nodes[i].id) == scratch_index.end ()) mark (nodes[i]);
  nodes.swap (scratch);
  index.swap (scratch_index);
  if (captured)
    {
      auto target = index.find (capture_key);
      bool alive = capture_key.empty () ? nodes[0].identity == captured
                   : target != index.end () && nodes[target->second].identity == captured;
      if (!alive) cancel ("removed");
    }
  if (!layouts.empty ()) wake ();
}

void bar::layout (int id, bool changed)
{
  bar_node &p = nodes[id];
  if (!changed && !p.layout_dirty) return;
  p.layout_dirty = false;
  // Normalize by the largest weight to keep even very large finite ratios
  // from overflowing their sum. Pixels are never scaled to fit.
  int64_t fixed = 0;
  double largest = 0, weights = 0;
  for (int child : p.children)
    if (nodes[child].pixels) fixed += int (nodes[child].size);
    else largest = max (largest, nodes[child].size);
  if (largest)
    for (int child : p.children)
      if (!nodes[child].pixels) weights += nodes[child].size / largest;
  double remaining = max (int64_t (0), p.length - fixed);
  double cumulative = 0;
  int64_t used = 0;
  for (int child : p.children)
    {
      bar_node &n = nodes[child];
      double amount = n.pixels ? n.size : weights ? remaining * (n.size / largest) / weights : 0;
      cumulative += amount;
      int64_t end = int64_t (std::floor (cumulative + 0.5));
      int64_t start = p.start + used, length = end - used;
      int lo = int (clamp (start, int64_t (p.lo), int64_t (p.hi)));
      int hi = int (clamp (start + length, int64_t (p.lo), int64_t (p.hi)));
      bool moved = start != n.start || length != n.length || lo != n.lo || hi != n.hi;
      if (moved) mark (n);
      n.start = start; n.length = length; n.lo = lo; n.hi = hi;
      if (moved) mark (n);
      layout (child, moved);
      used = end;
    }
}

void bar::ensure_layout ()
{
  bar_node &root = nodes[0];
  if (root.length != length ())
    {
      root.start = root.lo = 0;
      root.length = root.hi = length ();
      layout (0, true);
    }
  // Parsed preorder places every ancestor before its dirty descendants.
  for (int id : layouts) layout (id, false);
  layouts.clear ();
}

void bar::prepare_text (bar_node &n)
{
  if (!n.cells.empty () || n.text.empty ()) return;
  for (size_t i = 0; i < n.text.size (); )
    {
      uint32_t c = codepoint (n.text, i);
      int width = WCWIDTH (c);
      if (!width)
        {
#if ENABLE_COMBINING
          int pos = int (n.cells.size ()) - 1;
          while (pos >= 0 && n.cells[pos] == NOCHAR) --pos;
          if (pos >= 0) n.cells[pos] = rxvt_composite.compose (n.cells[pos], c);
#endif
          continue;
        }
      width = clamp (width, 1, 2);
#if !UNICODE_3 && ENABLE_COMBINING
      if (c >= 0x10000) c = rxvt_composite.compose (c);
#elif !UNICODE_3
      if (c >= 0x10000) c = 0xfffd;
#endif
      n.cells.push_back (c);
      if (width == 2) n.cells.push_back (NOCHAR);
    }
  n.fonts.resize (n.cells.size ());
  int font = 2;
  for (size_t i = 0; i < n.cells.size (); ++i)
    {
      if (n.cells[i] != NOCHAR) font = term->fontset[0]->find_font (n.cells[i]);
      n.fonts[i] = font;
    }
}

void bar::clip (int lo, int hi)
{
  XRectangle r = {short (vertical () ? 0 : lo), short (vertical () ? lo : 0),
                  (unsigned short)(vertical () ? width : hi - lo),
                  (unsigned short)(vertical () ? hi - lo : height)};
  XSetClipRectangles (term->dpy, gc, 0, 0, &r, 1, Unsorted);
#if XFT
  XftDrawSetClipRectangles (*drawable, 0, 0, &r, 1);
#endif
}

void bar::render_node (int id, int lo, int hi)
{
  bar_node &n = nodes[id];
  lo = max (lo, n.lo); hi = min (hi, n.hi);
  if (hi <= lo) return;
  clip (lo, hi);
  XSetForeground (term->dpy, gc, pixel (n.bg));
  XFillRectangle (term->dpy, pixmap, gc, vertical () ? 0 : lo, vertical () ? lo : 0,
                  vertical () ? width : hi - lo, vertical () ? hi - lo : height);
  if (!n.text.empty ())
    {
      prepare_text (n);
      int64_t text_width = int64_t (n.cells.size ()) * term->fwidth;
      int64_t px = (vertical () ? width : n.length) / 2 - text_width / 2 + (vertical () ? 0 : n.start);
      rxvt_font *base = (*term->fontset[0])[rxvt_fontset::firstFont];
      int64_t py = (vertical () ? n.length : height) / 2 - (base->ascent + base->descent) / 2
                   + (vertical () ? n.start : 0);
      // Slice long text before drawing, including a cell of ink overhang on
      // either side. This also keeps X11's signed glyph coordinates in range.
      int left = vertical () ? 0 : lo, right = vertical () ? width : hi;
      int first = int (clamp ((int64_t (left) - px) / term->fwidth - 2, int64_t (0), int64_t (n.cells.size ())));
      int last = int (clamp ((int64_t (right) - px) / term->fwidth + 2, int64_t (0), int64_t (n.cells.size ())));
      while (first && first < int (n.cells.size ()) && n.cells[first] == NOCHAR) --first;
      while (last < int (n.cells.size ()) && n.cells[last] == NOCHAR) ++last;
      // Fonts read term->pix_colors[fg] (and Color_bg on some paths), so an
      // RGB foreground swaps in a full palette copy with it in an extra slot.
      int fg = n.fg;
      rxvt_color *saved_colors = term->pix_colors;
      auto found = (fg & rgb_color) ? rgb.find (fg) : rgb.end ();
      if (found != rgb.end ())
        {
          palette.assign (saved_colors, saved_colors + TOTAL_COLORS);
          palette.push_back (found->second);
          term->pix_colors = palette.data ();
          fg = TOTAL_COLORS;
        }
      else if (fg & rgb_color) fg = Color_fg; // Unreachable for accepted nodes.
      if (py >= -32768 && py <= 32767)
        for (int i = first; i < last; )
          {
            int end = i + 1;
            while (end < last && n.fonts[end] == n.fonts[i]) ++end;
            // -2 suppresses font background clearing: this node's rectangle
            // is already filled, including when the terminal uses an image.
            term->fontset[0]->operator [] (n.fonts[i])->draw (*drawable, int (px + int64_t (i) * term->fwidth),
                                                          int (py), n.cells.data () + i, end - i, fg, -2);
            i = end;
          }
      term->pix_colors = saved_colors;
    }
  // Child clips are ordered disjoint intervals. Binary search skips siblings
  // before the damaged span, and recursion never visits unrelated subtrees.
  auto begin = std::lower_bound (n.children.begin (), n.children.end (), lo,
                                [this](int child, int value) { return nodes[child].hi <= value; });
  for (auto i = begin; i != n.children.end () && nodes[*i].lo < hi; ++i)
    render_node (*i, lo, hi);
}

void bar::paint ()
{
  if (!term || !pixmap || !width || !height) return;
  ensure_layout ();
  if (damage.empty ()) return;
  std::sort (damage.begin (), damage.end (), [](span a, span b) { return a.lo < b.lo; });
  size_t count = 0;
  for (span r : damage)
    if (count && r.lo <= damage[count - 1].hi) damage[count - 1].hi = max (damage[count - 1].hi, r.hi);
    else damage[count++] = r;
  // The existing font implementations take their GC from the terminal. Swap
  // it only while painting, with no Lua calls, to isolate all bar clip state.
  GC saved = term->gc;
  term->gc = gc;
  for (size_t i = 0; i < count; ++i)
    {
      span r = damage[i];
      if (shown) render_node (0, r.lo, r.hi);
      else
        {
          clip (r.lo, r.hi);
          XSetForeground (term->dpy, gc, term->pix_colors[Color_bg]);
          XFillRectangle (term->dpy, pixmap, gc, 0, 0, width, height);
        }
      // Text can leave a narrower clip; presentation must copy the whole span.
      XSetClipMask (term->dpy, gc, None);
      XCopyArea (term->dpy, pixmap, window, gc, vertical () ? 0 : r.lo, vertical () ? r.lo : 0,
                 vertical () ? width : r.hi - r.lo, vertical () ? r.hi - r.lo : height,
                 vertical () ? 0 : r.lo, vertical () ? r.lo : 0);
    }
  term->gc = saved;
  damage.clear ();
  retained_valid = true;
}

void bar::surface ()
{
  if (!surface_changed || !term->parent) return;
  surface_changed = false;
  if (!width || !height)
    {
      if (window) XUnmapWindow (term->dpy, window);
      cancel ("unmapped");
      return;
    }
  if (!window)
    {
      window = XCreateSimpleWindow (term->dpy, term->parent, x, y, width, height, 0,
                                    term->pix_colors[Color_fg], term->pix_colors[Color_bg]);
      cursor = XCreateFontCursor (term->dpy, XC_left_ptr);
      XDefineCursor (term->dpy, window, cursor);
      XSelectInput (term->dpy, window, ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask);
      watcher.set<rxvt_bar_manager, &rxvt_bar_manager::x_cb> (manager);
      watcher.start (term->display, window);
      gc = XCreateGC (term->dpy, window, 0, 0);
      XSetGraphicsExposures (term->dpy, gc, False);
    }
  XMoveResizeWindow (term->dpy, window, x, y, width, height);
  if (width != surface_w || height != surface_h)
    {
      delete drawable;
      if (pixmap) XFreePixmap (term->dpy, pixmap);
      pixmap = XCreatePixmap (term->dpy, window, width, height, term->depth);
      retained_valid = false;
      drawable = new rxvt_drawable (term, pixmap);
      surface_w = width; surface_h = height;
      full ();
    }
  XMapWindow (term->dpy, window);
}

int bar::hit (int position) const
{
  int id = 0;
  for (;;)
    {
      const auto &children = nodes[id].children;
      auto i = std::lower_bound (children.begin (), children.end (), position,
                                 [this](int child, int p) { return nodes[child].hi <= p; });
      if (i == children.end () || position < nodes[*i].lo || position >= nodes[*i].hi) return id;
      id = *i;
    }
}

void bar::event (const char *type, int target, const XEvent *ev, const char *reason)
{
  if (callback <= 0 || !term || !term->lua.ref) return;
  lua_State *L = rxvt_lua.L;
  int base = lua_gettop (L);
  lua_rawgeti (L, LUA_REGISTRYINDEX, callback);
  lua_rawgeti (L, LUA_REGISTRYINDEX, term->lua.ref);
  lua_rawgeti (L, LUA_REGISTRYINDEX, self_ref);
  lua_createtable (L, 0, 18);
#define BAR_INT(key, value) (lua_pushnumber (L, (value)), lua_setfield (L, -2, (key)))
#define BAR_BOOL(key, value) (lua_pushboolean (L, (value)), lua_setfield (L, -2, (key)))
  lua_pushstring (L, type); lua_setfield (L, -2, "type");
  if (target > 0)
    { lua_pushlstring (L, nodes[target].id.data (), nodes[target].id.size ()); lua_setfield (L, -2, "id"); }
  if (reason) { lua_pushstring (L, reason); lua_setfield (L, -2, "reason"); }
  if (ev)
    {
      const XMotionEvent &m = ev->xmotion;
      BAR_INT ("x", m.x); BAR_INT ("y", m.y);
      BAR_INT ("local_x", m.x - (vertical () ? 0 : nodes[target].start));
      BAR_INT ("local_y", m.y - (vertical () ? nodes[target].start : 0));
      BAR_INT ("x_root", m.x_root); BAR_INT ("y_root", m.y_root);
      BAR_INT ("time", m.time); BAR_INT ("state", m.state);
      BAR_BOOL ("shift", m.state & ShiftMask); BAR_BOOL ("ctrl", m.state & ControlMask);
      BAR_BOOL ("meta", m.state & term->ModMetaMask);
      if (ev->type != MotionNotify) BAR_INT ("button", ev->xbutton.button);
    }
  BAR_INT ("width", width); BAR_INT ("height", height);
#undef BAR_INT
#undef BAR_BOOL
  bool ok = rxvt_lua.call (term, 3, "bar event");
  lua_settop (L, base);
  if (!ok && term)
    {
      luaL_unref (L, LUA_REGISTRYINDEX, callback);
      callback = 0;
      cancel ("error");
    }
}

void bar::cancel (const char *reason)
{
  if (!captured || !term) return;
  captured = 0; captured_button = 0;
  XUngrabPointer (term->dpy, CurrentTime);
  if (reason) event ("cancel", 0, 0, reason);
}

void bar::close ()
{
  if (!term) return;
  cancel ("closed");
  // Cancellation is a Lua callback and can recursively close this object.
  if (!term) return;
  if (window)
    {
      watcher.stop (term->display);
      delete drawable; drawable = 0;
      if (pixmap) XFreePixmap (term->dpy, pixmap);
      XFreeGC (term->dpy, gc);
      XFreeCursor (term->dpy, cursor);
      XDestroyWindow (term->dpy, window);
    }
  free_colors ();
  lua_State *L = rxvt_lua.L;
  if (callback > 0) luaL_unref (L, LUA_REGISTRYINDEX, callback);
  if (self_ref > 0) luaL_unref (L, LUA_REGISTRYINDEX, self_ref);
  callback = self_ref = 0;
  term = 0;
  manager->geometry_dirty = true;
  manager->s->order_dirty = true;
  manager->s->insets[edge] -= thickness;
  wake ();
}

static bar *check_bar (lua_State *L)
{
  auto *handle = (bar_handle *)luaL_checkudata (L, 1, "nrxvt.bar");
  bar *b = handle->value.get ();
  if (!b || !b->term) luaL_error (L, "bar has been closed");
  return b;
}

static int submit (lua_State *L)
{
  bar *b = check_bar (L);
  luaL_checktype (L, 2, LUA_TTABLE);
  try
    {
      b->used = 0;
      for (auto &entry : b->scratch_index) entry.second = -1;
      b->parse (L, 2, -1, 0);
      b->alloc_colors ();
      b->accept ();
      // accept can run a cancel callback that closes the bar and its colors.
      if (b->term) b->prune_colors ();
      return 0;
    }
  catch (const char *message) { lua_settop (L, 2); lua_pushstring (L, message); }
  catch (const std::bad_alloc &) { lua_settop (L, 2); lua_pushliteral (L, "bar allocation failed"); }
  return lua_error (L);
}

static int visible (lua_State *L)
{
  bar *b = check_bar (L);
  if (!lua_isnoneornil (L, 2))
    {
      luaL_checktype (L, 2, LUA_TBOOLEAN);
      bool shown = lua_toboolean (L, 2);
      if (b->shown != shown)
        {
          b->shown = shown;
          b->full ();
          if (!shown) b->cancel ("hidden");
        }
    }
  lua_pushboolean (L, b->shown);
  return 1;
}

static int geometry (lua_State *L)
{
  bar *b = check_bar (L);
  lua_pushinteger (L, b->x); lua_pushinteger (L, b->y);
  lua_pushinteger (L, b->width); lua_pushinteger (L, b->height);
  return 4;
}

static int capture (lua_State *L)
{
  bar *b = check_bar (L);
  luaL_checktype (L, 2, LUA_TTABLE);
  bool ok = false;
  if (b->press && b->shown)
    {
      const XButtonEvent &ev = b->press->xbutton;
      field (L, 2, "time"); field (L, 2, "button");
      bool matches = lua_type (L, -2) == LUA_TNUMBER && lua_type (L, -1) == LUA_TNUMBER
                     && lua_tonumber (L, -2) == ev.time && lua_tonumber (L, -1) == ev.button;
      lua_pop (L, 2);
      if (matches && ev.button < Button4)
        {
          b->manager->cancel ("replaced");
          int target = -1;
          for (size_t i = 0; i < b->nodes.size (); ++i)
            if (b->nodes[i].identity == b->press_target) { target = i; break; }
          if (b->term && b->shown && target >= 0)
            ok = XGrabPointer (b->term->dpy, b->window, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                               GrabModeAsync, GrabModeAsync, None, None, ev.time) == GrabSuccess;
          if (ok)
            {
              b->captured = b->press_target; b->captured_button = ev.button;
              b->capture_key = b->nodes[target].id;
            }
        }
    }
  lua_pushboolean (L, ok);
  return 1;
}

static int release (lua_State *L) { check_bar (L)->cancel (0); return 0; }

static int close (lua_State *L)
{
  auto *handle = (bar_handle *)luaL_checkudata (L, 1, "nrxvt.bar");
  handle->value->close ();
  return 0;
}

static int gc (lua_State *L)
{
  auto *handle = (bar_handle *)luaL_checkudata (L, 1, "nrxvt.bar");
  handle->~bar_handle ();
  return 0;
}

} // namespace

rxvt_bar_manager::rxvt_bar_manager (rxvt_term *term)
: term (term), s (new rxvt_bar_state), geometry_dirty (false)
{ }

rxvt_bar_manager::~rxvt_bar_manager ()
{
  s->shutting_down = true;
  // Keep the owning vector stable while cancellation callbacks run.
  for (size_t i = 0; i < s->bars.size (); ++i) s->bars[i]->close ();
  delete s;
}

int rxvt_bar_manager::inset (int edge) const { return s->insets[edge]; }

bool rxvt_bar_manager::pending () const
{
  return term->parent && term->mapped && (s->work || geometry_dirty);
}

bool rxvt_bar_manager::consume_pointer (XEvent &ev)
{
  if (ev.type != MotionNotify && ev.type != ButtonPress && ev.type != ButtonRelease) return false;
  bool consumed = false;
  if (ev.type == ButtonRelease && ev.xbutton.button >= 1 && ev.xbutton.button <= 5)
    {
      unsigned int mask = Button1Mask << (ev.xbutton.button - 1);
      consumed = s->pressed & mask;
      s->pressed &= ~mask;
    }
  // Outside releases may go to another application. Its next press must not
  // inherit a stale suppression flag when it returns to this terminal.
  s->pressed &= ev.xmotion.state;
  return consumed || (ev.type == MotionNotify && s->pressed);
}

void rxvt_bar_manager::dock ()
{
  if (s->shutting_down) return;
  geometry_dirty = false;
  // stable_sort preserves creation order for equal priorities. Remove closed
  // bars only here, outside callbacks and input iteration.
  s->bars.erase (std::remove_if (s->bars.begin (), s->bars.end (), [](const bar_ptr &b) { return !b->term; }), s->bars.end ());
  if (s->order_dirty)
    {
      std::stable_sort (s->bars.begin (), s->bars.end (), [](const bar_ptr &a, const bar_ptr &b) {
        return a->edge != b->edge ? a->edge < b->edge : a->priority < b->priority;
      });
      s->order_dirty = false;
    }
  int top = 0, bottom = term->szHint.height, left = 0, right = term->szHint.width;
  for (int phase = 0; phase < 2; ++phase)
    for (const auto &b : s->bars)
      {
        bool horizontal = b->edge == BAR_TOP || b->edge == BAR_BOTTOM;
        if (horizontal != (phase == 0)) continue;
        int x = left, y = top, w = max (0, right - left), h = max (0, bottom - top);
        if (b->edge == BAR_TOP) { h = min (h, b->thickness); top += h; }
        else if (b->edge == BAR_BOTTOM) { h = min (h, b->thickness); bottom -= h; y = bottom; }
        else if (b->edge == BAR_LEFT) { w = min (w, b->thickness); left += w; }
        else { w = min (w, b->thickness); right -= w; x = right; }
        if (x != b->x || y != b->y || w != b->width || h != b->height)
          {
            b->x = x; b->y = y; b->width = w; b->height = h;
            b->surface_changed = true; b->resize_event = true;
            b->damage.clear (); b->full ();
          }
      }
}

void rxvt_bar_manager::native_geometry (int &x, int &y, int &height) const
{
  x = term->option (Opt_scrollBar_right)
      ? term->szHint.width - inset (BAR_RIGHT) - term->scrollBar.total_width () : inset (BAR_LEFT);
  y = inset (BAR_TOP);
  height = max (1, term->szHint.height - y - inset (BAR_BOTTOM));
}

void rxvt_bar_manager::prepare ()
{
  s->servicing = true;
  if (geometry_dirty) term->resize_all_windows (term->szHint.width, term->szHint.height, 1);
  // Resize callbacks can register/close bars, so take a shared reference and
  // use indices rather than retaining vector iterators across Lua calls.
  size_t count = s->bars.size ();
  for (size_t i = 0; i < count && i < s->bars.size (); ++i)
    {
      bar_ptr b = s->bars[i];
      if (!b->term) continue;
      b->surface ();
      if (b->term && b->resize_event)
        { b->resize_event = false; b->event ("resize", 0); }
    }
}

void rxvt_bar_manager::paint ()
{
  if (!term->mapped) { s->servicing = false; return; }
  s->work = false;
  for (const auto &b : s->bars) if (b->term) b->paint ();
  // Layout can add damage while painting; only unconsumed work needs another
  // frame. Callback-created/closed bars are resolved on the following pass.
  s->work = geometry_dirty;
  for (const auto &b : s->bars)
    if (b->term && (b->surface_changed || !b->damage.empty () || !b->layouts.empty ())) s->work = true;
  s->servicing = false;
  if (s->work) term->refresh_check ();
}

void rxvt_bar_manager::invalidate (bool fonts)
{
  for (const auto &b : s->bars)
    if (b->term)
      {
        if (fonts)
          for (auto &n : b->nodes) { n.cells.clear (); n.fonts.clear (); }
        b->full ();
      }
}

void rxvt_bar_manager::cancel (const char *reason)
{
  size_t count = s->bars.size ();
  for (size_t i = 0; i < count && i < s->bars.size (); ++i)
    {
      bar_ptr b = s->bars[i];
      b->cancel (reason);
    }
}

void rxvt_bar_manager::x_cb (XEvent &ev)
{
  term->make_current ();
  for (size_t i = 0; i < s->bars.size (); ++i)
    {
      bar_ptr b = s->bars[i];
      if (!b->term || b->window != ev.xany.window) continue;
      if ((ev.type == ButtonPress || ev.type == ButtonRelease)
          && ev.xbutton.button >= 1 && ev.xbutton.button <= 5)
        {
          unsigned int mask = Button1Mask << (ev.xbutton.button - 1);
          if (ev.type == ButtonPress) s->pressed |= mask;
          else s->pressed &= ~mask;
        }
      if (ev.type == Expose)
        {
          // Expose repairs the last presented frame even during synchronized
          // output. It must not publish an unpainted replacement declaration.
          int w = min (ev.xexpose.width, b->surface_w - ev.xexpose.x);
          int h = min (ev.xexpose.height, b->surface_h - ev.xexpose.y);
          if (b->retained_valid && w > 0 && h > 0)
            XCopyArea (term->dpy, b->pixmap, b->window, b->gc,
                       ev.xexpose.x, ev.xexpose.y, w, h, ev.xexpose.x, ev.xexpose.y);
          else b->full ();
        }
      else if (b->shown && (ev.type == ButtonPress || ev.type == ButtonRelease || ev.type == MotionNotify))
        {
          b->ensure_layout ();
          int target;
          if (b->captured)
            {
              auto found = b->index.find (b->capture_key);
              target = b->capture_key.empty () ? 0 : found->second;
            }
          else target = b->hit (b->vertical () ? ev.xmotion.y : ev.xmotion.x);
          b->press = ev.type == ButtonPress ? &ev : 0;
          b->press_target = b->nodes[target].identity;
          b->event (ev.type == ButtonPress ? "button_press" : ev.type == ButtonRelease ? "button_release" : "motion", target, &ev);
          b->press = 0;
          if (b->term && ev.type == ButtonRelease && ev.xbutton.button == b->captured_button) b->cancel (0);
        }
      break;
    }
  term->refresh_check ();
}

int rxvt_bar_new (lua_State *L)
{
  luaL_checktype (L, 1, LUA_TTABLE);
  lua_rawgeti (L, 1, 0);
  rxvt_term *term = (rxvt_term *)lua_touserdata (L, -1);
  lua_pop (L, 1);
  if (!term) return luaL_error (L, "terminal has been destroyed");
  luaL_checktype (L, 2, LUA_TTABLE);
  try
    {
      static const char *const options[] = {"edge", "thickness", "priority", "visible", "on_event", 0};
      keys (L, 2, options);
      field (L, 2, "edge");
      require (lua_type (L, -1) == LUA_TSTRING, "bar edge is required");
      const char *value = lua_tostring (L, -1);
      require (lua_objlen (L, -1) == strlen (value), "invalid bar edge");
      int edge = !strcmp (value, "top") ? BAR_TOP : !strcmp (value, "right") ? BAR_RIGHT
                 : !strcmp (value, "bottom") ? BAR_BOTTOM : !strcmp (value, "left") ? BAR_LEFT : -1;
      require (edge >= 0, "invalid bar edge");
      lua_pop (L, 1);
      field (L, 2, "thickness"); int thickness = integer (L, -1, 1, 32767); lua_pop (L, 1);
      field (L, 2, "priority"); int priority = integer (L, -1, INT_MIN, INT_MAX); lua_pop (L, 1);
      field (L, 2, "visible");
      require (lua_isnil (L, -1) || lua_isboolean (L, -1), "bar visibility must be boolean");
      bool shown = lua_isnil (L, -1) || lua_toboolean (L, -1); lua_pop (L, 1);
      field (L, 2, "on_event");
      require (lua_isnil (L, -1) || lua_isfunction (L, -1), "bar on_event must be a function");
      if (!term->lua.bars) term->lua.bars = new rxvt_bar_manager (term);
      auto *manager = term->lua.bars;
      require (!manager->s->shutting_down, "terminal bars are being destroyed");
      int sum = thickness;
      for (int i = 0; i < 4; ++i) sum += manager->inset (i);
      require (sum <= 32767 - 2 * term->int_bwidth - max (term->fwidth, term->fheight), "bar reservations exceed X11 geometry limits");
      auto *handle = (bar_handle *)lua_newuserdata (L, sizeof (bar_handle));
      new (handle) bar_handle;
      luaL_getmetatable (L, "nrxvt.bar"); lua_setmetatable (L, -2);
      handle->value = std::make_shared<bar> (manager, edge, thickness, priority, shown);
      bar *b = handle->value.get ();
      lua_pushvalue (L, -2);
      b->callback = luaL_ref (L, LUA_REGISTRYINDEX);
      if (b->callback == LUA_REFNIL) b->callback = 0;
      lua_pushvalue (L, -1); b->self_ref = luaL_ref (L, LUA_REGISTRYINDEX);
      manager->s->bars.push_back (handle->value);
      manager->s->insets[edge] += thickness;
      manager->geometry_dirty = true;
      manager->s->order_dirty = true;
      b->wake ();
      return 1;
    }
  catch (const char *message) { lua_pushstring (L, message); }
  catch (const std::bad_alloc &) { lua_pushliteral (L, "bar allocation failed"); }
  return lua_error (L);
}

void rxvt_bar_register (lua_State *L)
{
  static const luaL_Reg methods[] = {
    {"submit", submit}, {"visible", visible}, {"geometry", geometry},
    {"capture", capture}, {"release", release}, {"close", close}, {0, 0}
  };
  luaL_newmetatable (L, "nrxvt.bar");
  lua_newtable (L); luaL_register (L, 0, methods); lua_setfield (L, -2, "__index");
  lua_pushcfunction (L, gc); lua_setfield (L, -2, "__gc");
  lua_pop (L, 1);
}
#endif
