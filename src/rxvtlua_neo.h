/*
 * rxvtlua_neo.h - embedded LuaJIT plugin system
 *
 * Plugins are the *.lua files in $XDG_CONFIG_HOME/nrxvt/plugins (default
 * ~/.config/nrxvt/plugins), see doc/lua-plugins.md for the API.
 *
 * The hooks reuse the HOOK_INVOKE/SHOULD_INVOKE call sites of the perl
 * interface, so this header provides the same macros and enums as
 * rxvtperl.h does for perl. The two interfaces are mutually exclusive.
 */

#ifndef RXVTLUA_H_
#define RXVTLUA_H_

#if ENABLE_LUA

#include "rxvtbar_neo.h"

#if ENABLE_PERL
# error "the perl and the lua plugin interfaces are mutually exclusive"
#endif

// Only checks a global flag array, so call sites of hooks that no plugin
// implements cost next to nothing.
#define SHOULD_INVOKE(htype) (rxvt_lua.should_invoke [htype])
#define HOOK_INVOKE(args) (rxvt_lua.L && rxvt_lua.invoke args)

enum data_type {
  DT_END,     // no further arguments
  DT_INT,
  DT_LONG,
  DT_STR,     // 0-terminated string
  DT_STR_LEN, // string + length
  DT_WCS_LEN, // wchar_t* + length
  DT_LCS_LEN, // long* + length
  DT_XEVENT,
};

enum hook_type {
# define def(sym) HOOK_ ## sym,
# include "hookinc.h"
# undef def
  HOOK_NUM,
};

struct lua_State;
struct rxvt_selection;

// per terminal state, must be zero-initialised
struct rxvt_lua_term
{
  int ref; // registry reference of the lua terminal object, 0 if none
  rxvt_bar_manager *bars; // allocated lazily by nrxvt.api.new_bar
  XrmDatabase resources; // terminal-local Lua defaults, released at destruction
  XrmDatabase overrides; // explicit -xrm values, kept separate from defaults
};

struct rxvt_lua_interp
{
  lua_State *L; // 0 unless init.lua or at least one plugin was found
  uint8_t should_invoke[HOOK_NUM];

  // Loads init.lua and plugins on the first call, applies defaults, and
  // creates the terminal object. Returns whether Lua is active.
  bool init (rxvt_term *term);
  bool invoke (rxvt_term *term, int htype, ...);
  // Calls a prepared bar callback with the same context and error handling
  // as ordinary plugin hooks. Function and arguments are already on L.
  bool call (rxvt_term *term, int nargs, const char *what);
  // Runs the handlers of a name filtering hook (HOOK_TITLE_CHANGE or
  // HOOK_ICON_NAME_CHANGE) over str. Returns str itself when every handler
  // passed it through, otherwise the rewritten name, which stays valid
  // until the next call. Gate calls with SHOULD_INVOKE.
  const char *filter_name (rxvt_term *term, int htype, const char *str);
  // hands the text of a nrxvt.api.selection_fetch request to its lua function
  void selection_finish (rxvt_selection *sel, char *data, unsigned int len);
};

extern struct rxvt_lua_interp rxvt_lua;

#endif

#endif
