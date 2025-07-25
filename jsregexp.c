#define LUA_LIB

#include <lauxlib.h>
#include <lua.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libregexp/cutils.h"
#include "libregexp/libregexp.h"

#define CAPTURE_COUNT_MAX 255 /* from libregexp.c */
#define JSREGEXP_MT "jsregexp_meta"
#define JSREGEXP_MATCH_MT "jsregexp_match_meta"
#define JSSTRING_MT "jsstring_meta"

#if LUA_VERSION_NUM >= 502
#define new_lib(L, l) (luaL_newlib(L, l))
#define lua_tbl_len(L, arg) (lua_rawlen(L, arg))
#define lua_set_functions(L, fs) luaL_setfuncs(L, fs, 0)
#else
#define new_lib(L, l) (lua_newtable(L), luaL_register(L, NULL, l))
#define lua_tbl_len(L, arg) (lua_objlen(L, arg))
#define lua_set_functions(L, fs) luaL_register(L, NULL, fs);
#endif

#if LUA_VERSION_NUM < 503
#define lua_pushinteger(L, n) lua_pushinteger(L, n)
#endif

#define streq(X, Y) ((*(X) == *(Y)) && strcmp(X, Y) == 0)

// these two functions need to be defined for libregexp
void *lre_realloc(void *opaque, void *ptr, size_t size) {
  return realloc(ptr, size);
}
bool lre_check_stack_overflow(void *opaque, size_t alloca_size) {
  return false;
}

struct regexp {
  char *expr;
  uint8_t *bc;
  uint32_t last_index;
};

struct jsstring {
  bool is_wide_char;
  uint32_t len;
  char *bstr;        // base string passed in
  uint32_t bstr_len; // base string length
  uint32_t *indices;
  uint32_t *rev_indices;
  union {
    uint8_t *str8; /* 8 bit strings will get an extra null terminator */
    uint16_t *str16;
  } u;
};

// check for bytes higher or equal to 0xf0
static inline bool utf8_contains_non_bmp(const char *s) {
  uint8_t *q = (uint8_t *)s;
  while (*q) {
    if ((*q++ & 0xf0) == 0xf0) {
      return true;
    }
  }
  return false;
}

static inline bool utf8_contains_non_ascii(const char *s) {
  while (*s) {
    if (*s++ & 0x80) {
      return true;
    }
  }
  return false;
}

// returns NULL when malformed unicode is encountered, otherwise returns the
// converted string. *utf16_len will contain the length of the string and
// *indices an (allocated) array mapping each utf16 code point to the utf8 code
// point in the input string.
static inline uint16_t *utf8_to_utf16(const uint8_t *input, uint32_t n,
                                      uint32_t *utf16_len, uint32_t **indices,
                                      uint32_t **rev_indices) {
  *indices = calloc((n + 1), sizeof **indices);
  // TODO: lazy way of doing it, later implement using binary search tree
  *rev_indices = calloc((n + 1), sizeof **indices);
  uint16_t *str = malloc((n + 1) * sizeof *str);
  uint16_t *q = str;
  const uint8_t *pos = input;
  while (*pos) {
    (*indices)[q - str] = pos - input;
    (*rev_indices)[pos - input] = q - str;
    uint32_t c = utf8_decode_len(pos, UTF8_CHAR_LEN_MAX, &pos);
    if (c == -1) {
      // malformed
      free(str);
      free(*indices);
      free(*rev_indices);
      return NULL;
    }
    if ((unsigned)c > 0xffff) {
      *q++ = (((c - 0x10000) >> 10) | (0xd8 << 8));
      *q++ = (c & 0xfffff) | (0xdc << 8);
    } else {
      *q++ = c & 0xffff;
    }
  }
  *q = 0;
  (*indices)[q - str] = n;
  (*rev_indices)[n] = q - str;

  *utf16_len = q - str;
  return str;
}

static int jsstring_new(lua_State *lstate) {
  if (lua_isuserdata(lstate, 1)) {
    luaL_checkudata(lstate, 1, JSSTRING_MT);
    lua_pushvalue(lstate, 1);
    return 1;
  }

  size_t input_len;
  const uint8_t *input = (uint8_t *)luaL_checklstring(lstate, 1, &input_len);
  struct jsstring *ud;
  if (utf8_contains_non_ascii((char *)input)) {
    uint32_t *indices;
    uint32_t *rev_indices;
    uint32_t input_utf16_len;
    uint16_t *input_utf16 = utf8_to_utf16(input, input_len, &input_utf16_len,
                                          &indices, &rev_indices);

    if (!input_utf16) {
      return luaL_error(lstate, "malformed unicode");
    }

    ud = lua_newuserdata(lstate, sizeof(*ud));
    ud->is_wide_char = true;
    ud->len = input_utf16_len;
    ud->u.str16 = input_utf16;
    ud->bstr = strdup((char *)input);
    ud->bstr_len = input_len;
    ud->indices = indices;
    ud->rev_indices = rev_indices;
  } else {
    ud = lua_newuserdata(lstate, sizeof(*ud));
    ud->is_wide_char = false;
    ud->len = input_len;
    ud->bstr_len = input_len;
    ud->u.str8 = (uint8_t *)strdup((char *)input);
    ud->bstr = (char *)ud->u.str8;
    ud->indices = NULL;
    ud->rev_indices = NULL;
  }
  luaL_getmetatable(lstate, JSSTRING_MT);
  lua_setmetatable(lstate, -2);
  return 1;
}

static int jsstring_gc(lua_State *lstate) {
  struct jsstring *s = lua_touserdata(lstate, 1);
  free(s->u.str8);
  free(s->indices);

  if (s->is_wide_char) {
    free(s->bstr);
    free(s->rev_indices);
  }
  return 0;
}

static struct luaL_Reg jsstring_meta[] = {{"__gc", jsstring_gc}, {NULL, NULL}};

static inline struct jsstring *lua_tojsstring(lua_State *lstate, int arg) {
  if (lua_isuserdata(lstate, arg)) {
    // already jsstring
    return (struct jsstring *)luaL_checkudata(lstate, arg, JSSTRING_MT);
  } else {
    // coerce to jsstring
    lua_pushcfunction(lstate, jsstring_new);
    lua_insert(lstate, arg);
    lua_call(lstate, 1, 1);
    return (struct jsstring *)luaL_checkudata(lstate, arg, JSSTRING_MT);
  }
}

static int regexp_gc(lua_State *lstate) {
  struct regexp *r = lua_touserdata(lstate, 1);
  free(r->bc);
  free(r->expr);
  return 0;
}

static void regexp_pushflags(lua_State *lstate, const struct regexp *r) {
  const int flags = lre_get_flags(r->bc);
  const char *indices = (flags & LRE_FLAG_INDICES) ? "d" : "";
  const char *ignorecase = (flags & LRE_FLAG_IGNORECASE) ? "i" : "";
  const char *global = (flags & LRE_FLAG_GLOBAL) ? "g" : "";
  const char *multiline = (flags & LRE_FLAG_MULTILINE) ? "m" : "";
  const char *named_groups = (flags & LRE_FLAG_NAMED_GROUPS) ? "n" : "";
  const char *dotall = (flags & LRE_FLAG_DOTALL) ? "s" : "";
  const char *utf16 = (flags & LRE_FLAG_UNICODE) ? "u" : "";
  const char *sticky = (flags & LRE_FLAG_STICKY) ? "y" : "";
  lua_pushfstring(lstate, "%s%s%s%s%s%s%s%s", indices, ignorecase, global,
                  multiline, named_groups, dotall, utf16, sticky);
}

static int regexp_tostring(lua_State *lstate) {
  const struct regexp *r = luaL_checkudata(lstate, 1, JSREGEXP_MT);
  lua_pushfstring(lstate, "/%s/", r->expr);
  regexp_pushflags(lstate, r);
  lua_concat(lstate, 2);
  return 1;
}

// automatic conversion to the global match string
static int match_tostring(lua_State *lstate) {
  // luaL_getmetatable(lstate, JSREGEXP_MATCH);
  // if (!lua_getmetatable(lstate, 1) || !lua_equal(lstate, -1, -2)) {
  //   return luaL_argerror(lstate, 1, "match object expected");
  // }
  lua_rawgeti(lstate, 1, 0);
  return 1;
}

// repeatedly running regexp:match(input) is not a good idea because we would
// convert the string (at least from last_ind) to utf16 every time (if it is
// needed)
static int regexp_exec(lua_State *lstate) {
  uint8_t *capture[CAPTURE_COUNT_MAX * 2];

  struct regexp *r = luaL_checkudata(lstate, 1, JSREGEXP_MT);
  const struct jsstring *input = lua_tojsstring(lstate, 2);

  const int global = lre_get_flags(r->bc) & LRE_FLAG_GLOBAL;
  const int sticky = lre_get_flags(r->bc) & LRE_FLAG_STICKY;
  uint32_t rlast_index = r->last_index;
  // translate wide char to correct index
  if (input->is_wide_char) {
    // only translate indices if possible
    if (rlast_index > 0 && rlast_index <= input->bstr_len) {
      // move to the next valid index, rlast_index might be somewhere within a
      // multibyte character
      while (rlast_index < input->bstr_len &&
             !input->rev_indices[rlast_index]) {
        rlast_index++;
      }
      rlast_index = input->rev_indices[rlast_index];
    }
  }

  if (!global && !sticky) {
    rlast_index = 0;
    r->last_index = 0;
  } else if (rlast_index > input->len) {
    r->last_index = 0;
    return 0;
  }

  const int capture_count = lre_get_capture_count(r->bc);
  const char *group_names = lre_get_groupnames(r->bc);
  const bool has_indices = lre_get_flags(r->bc) & LRE_FLAG_INDICES;

  const int ret =
      lre_exec(capture, r->bc, (uint8_t *)input->u.str8, rlast_index,
               input->len, input->is_wide_char ? 1 : 0, NULL);

  if (ret < 0) {
    return luaL_error(lstate, "out of memory in regexp execution");
  }

  if (ret == 0) {
    // no match
    if (global || sticky) {
      r->last_index = 0;
    }
    return 0;
  } else if (global || sticky) {
    // match found
    if (input->is_wide_char) {
      r->last_index = input->indices[(capture[1] - input->u.str8) / 2];
    } else {
      r->last_index = capture[1] - input->u.str8;
    }
  }

  lua_createtable(lstate, capture_count + 1, capture_count + 3);

  luaL_getmetatable(lstate, JSREGEXP_MATCH_MT);
  lua_setmetatable(lstate, -2);

  lua_pushstring(lstate, input->bstr);
  lua_setfield(lstate, -2, "input");

  lua_pushinteger(lstate, capture_count);
  lua_setfield(lstate, -2, "capture_count");

  if (input->is_wide_char) {
    lua_pushnumber(
        lstate,
        1 + input->indices[(capture[0] - input->u.str8) / 2]); // 1-based
  } else {
    lua_pushnumber(lstate, 1 + capture[0] - input->u.str8); // 1-based
  }
  lua_setfield(lstate, -2, "index");

  if (has_indices) {
    // [match]
    lua_createtable(lstate, capture_count + 1, 0); // match.indices
    // [match, indices]
    if (group_names) {
      // push indices.groups table, duplicate it and leave it below match
      lua_createtable(lstate, 0, capture_count); // match.indices.groups
      // [match, indices, groups]
      lua_pushvalue(lstate, -1);
      // [match, indices, groups, groups]
      lua_insert(lstate, -4);
      // [indices.groups, match, indices, groups]
      lua_setfield(lstate, -2, "groups");
      // [indices.groups, match, indices]
    }
    lua_pushvalue(lstate, -1);
    // [..., match, indices, indices]
    lua_setfield(lstate, -3, "indices");
    // [..., match, indices]
    lua_insert(lstate, -2); // leave table below the match table
    // [..., indices, match]
  }

  if (group_names) {
    // [..., match]
    lua_newtable(lstate); // match.groups
    // [..., match, groups]
    lua_pushvalue(lstate, -1);
    // [..., match, groups, groups]
    lua_setfield(lstate, -3, "groups"); // immediately insert into match
    // [..., match, groups]
    lua_insert(lstate, -2); // leave table below the match table
    // [..., groups, match]
  }

  // [groups.indices?, indices?, groups?, match]

  for (int i = 0; i < capture_count; i++) {
    uint32_t a, b;
    if (input->is_wide_char) {
      a = input->indices[(capture[2 * i] - input->u.str8) / 2];
      b = input->indices[(capture[2 * i + 1] - input->u.str8) / 2];
      lua_pushlstring(lstate, input->bstr + a, b - a);
    } else {
      a = capture[2 * i] - input->u.str8;
      b = capture[2 * i + 1] - input->u.str8;
      lua_pushlstring(lstate, (char *)capture[2 * i],
                      capture[2 * i + 1] - capture[2 * i]);
    }

    if (has_indices) {
      lua_createtable(lstate, 2, 0);
      lua_pushinteger(lstate, a + 1);
      lua_rawseti(lstate, -2, 1);
      lua_pushinteger(lstate, b);
      lua_rawseti(lstate, -2, 2);
      // [..., match, string, {a, b}]
      if (group_names) {
        // [indices.groups, indices, groups, match, string, {a, b}]
        if (i > 0 && *group_names) {
          // if the current group is named, duplicate and insert into the
          // correct table
          lua_pushvalue(lstate, -1);
          // [indices.groups, indices, groups, match, string, {a, b}, {a,b}]
          lua_setfield(lstate, -7, group_names);
        }
        // [indices.groups, indices, groups, match, string, {a, b}]
        lua_rawseti(lstate, -5, i);
      } else {
        // [indices, match, string, {a, b}]
        lua_rawseti(lstate, -4, i);
      }
    }

    if (i > 0 && group_names) {
      // [..., groups, match, string]
      // if the current group is named, duplicate and insert into the correct
      // table
      if (*group_names) {
        lua_pushvalue(lstate, -1);
        // [..., groups, match, string, string]
        lua_setfield(lstate, -4, group_names);
        group_names += strlen(group_names);
      }
      group_names++;
    }

    // [..., match, string]
    lua_rawseti(lstate, -2, i);
  }

  return 1;
}

static int regexp_test(lua_State *lstate) {
  if (lua_gettop(lstate) != 2) {
    return luaL_error(lstate, "expecting exactly 2 arguments");
  }
  lua_pushcfunction(lstate, regexp_exec);
  lua_insert(lstate, 1);
  lua_call(lstate, 2, 1);
  lua_pushboolean(lstate, !lua_isnil(lstate, -1));
  return 1;
}

// more gettable fields to be added here
static int regexp_index(lua_State *lstate) {
  struct regexp *r = luaL_checkudata(lstate, 1, JSREGEXP_MT);

  luaL_getmetatable(lstate, JSREGEXP_MT);
  lua_pushvalue(lstate, 2);
  lua_rawget(lstate, -2);

  if (lua_isnil(lstate, -1)) {
    const char *key = lua_tostring(lstate, 2);
    if (streq(key, "last_index")) {
      lua_pushnumber(lstate, r->last_index + 1);
    } else if (streq(key, "dot_all")) {
      lua_pushboolean(lstate, lre_get_flags(r->bc) & LRE_FLAG_DOTALL);
    } else if (streq(key, "global")) {
      lua_pushboolean(lstate, lre_get_flags(r->bc) & LRE_FLAG_GLOBAL);
    } else if (streq(key, "ignore_case")) {
      lua_pushboolean(lstate, lre_get_flags(r->bc) & LRE_FLAG_IGNORECASE);
    } else if (streq(key, "multiline")) {
      lua_pushboolean(lstate, lre_get_flags(r->bc) & LRE_FLAG_MULTILINE);
    } else if (streq(key, "sticky")) {
      lua_pushboolean(lstate, lre_get_flags(r->bc) & LRE_FLAG_STICKY);
    } else if (streq(key, "unicode")) {
      lua_pushboolean(lstate, lre_get_flags(r->bc) & LRE_FLAG_UNICODE);
    } else if (streq(key, "has_indices")) {
      lua_pushboolean(lstate, lre_get_flags(r->bc) & LRE_FLAG_INDICES);
    } else if (streq(key, "source")) {
      lua_pushstring(lstate, r->expr);
    } else if (streq(key, "flags")) {
      regexp_pushflags(lstate, r);
    } else {
      return 0;
    }
  }
  return 1;
}

// only last_index should be settable
static int regexp_newindex(lua_State *lstate) {
  struct regexp *r = luaL_checkudata(lstate, 1, JSREGEXP_MT);

  const char *key = lua_tostring(lstate, 2);
  if (streq(key, "last_index")) {
    const int ind = luaL_checknumber(lstate, 3);
    luaL_argcheck(lstate, ind >= 1, 3, "last_index must be positive");
    r->last_index = ind - 1;
  } else {
    return luaL_argerror(lstate, 2, "unrecognized key");
  }

  return 0;
}

static struct luaL_Reg jsregexp_meta[] = {{"exec", regexp_exec},
                                          {"test", regexp_test},
                                          {"__gc", regexp_gc},
                                          {"__tostring", regexp_tostring},
                                          {"__index", regexp_index},
                                          {"__newindex", regexp_newindex},
                                          {NULL, NULL}};

static int jsregexp_compile(lua_State *lstate) {
  char error_msg[64];
  int len, re_flags = 0;

  const char *regexp = luaL_checkstring(lstate, 1);

  // lre_compile can segfault if the input contains 0x8f, which
  // indicated the beginning of a six byte sequence, but is now illegal.
  if (strchr(regexp, 0xfd)) {
    return luaL_argerror(lstate, 1, "malformed unicode");
  }

  if (utf8_contains_non_bmp(regexp)) {
    // bmp range works fine without utf16 flag
    re_flags |= LRE_FLAG_UNICODE;
  }

  if (!lua_isnoneornil(lstate, 2)) {
    const char *flags = luaL_checkstring(lstate, 2);
    while (*flags) {
      switch (*(flags++)) {
      case 'd':
        re_flags |= LRE_FLAG_INDICES;
        break;
      case 'i':
        re_flags |= LRE_FLAG_IGNORECASE;
        break;
      case 'g':
        re_flags |= LRE_FLAG_GLOBAL;
        break;
      case 'm':
        re_flags |= LRE_FLAG_MULTILINE;
        break;
      case 'n':
        re_flags |= LRE_FLAG_NAMED_GROUPS;
        break;
      case 's':
        re_flags |= LRE_FLAG_DOTALL;
        break;
      case 'u':
        re_flags |= LRE_FLAG_UNICODE;
        break;
      case 'y':
        re_flags |= LRE_FLAG_STICKY;
        break;
      default: /* unknown flag */;
      }
    }
  }

  uint8_t *bc = lre_compile(&len, error_msg, sizeof error_msg, regexp,
                            strlen(regexp), re_flags, NULL);

  if (!bc) {
    return luaL_argerror(lstate, 1, error_msg);
  }

  struct regexp *ud = lua_newuserdata(lstate, sizeof *ud);
  ud->bc = bc;
  ud->expr = strdup(regexp);
  ud->last_index = 0;

  luaL_getmetatable(lstate, JSREGEXP_MT);
  lua_setmetatable(lstate, -2);

  return 1;
}

static int jsregexp_compile_safe(lua_State *lstate) {
  // invalid arg types should still error
  luaL_checkstring(lstate, 1);
  luaL_optstring(lstate, 2, NULL);

  lua_pushcfunction(lstate, jsregexp_compile);
  lua_insert(lstate, 1); // insert func before args
  if (lua_pcall(lstate, lua_gettop(lstate) - 1, 1, 0) == 0) {
    return 1;
  } else {
    lua_pushnil(lstate);
    lua_insert(lstate, -2); // add nil before error message
    return 2;
  }
}

static const struct luaL_Reg jsregexp_lib[] = {
    {"compile", jsregexp_compile},
    {"compile_safe", jsregexp_compile_safe},
    {"to_jsstring", jsstring_new},
    {NULL, NULL}};

int luaopen_jsregexp_core(lua_State *lstate) {
  luaL_newmetatable(lstate, JSREGEXP_MATCH_MT);
  lua_pushcfunction(lstate, match_tostring);
  lua_setfield(lstate, -2, "__tostring");

  luaL_newmetatable(lstate, JSREGEXP_MT);
  lua_pushvalue(lstate, -1);
  lua_setfield(lstate, -2, "__index"); // meta.__index = meta
  lua_set_functions(lstate, jsregexp_meta);

  luaL_newmetatable(lstate, JSSTRING_MT);
  lua_set_functions(lstate, jsstring_meta);

  new_lib(lstate, jsregexp_lib);
  luaL_getmetatable(lstate, JSREGEXP_MT);
  lua_setfield(lstate, -2, "mt");

  return 1;
}
