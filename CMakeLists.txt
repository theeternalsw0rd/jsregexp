cmake_minimum_required(VERSION 3.15)
project(jsregexp C)

# Source files
set(SOURCES
    jsregexp.c
    libregexp/cutils.c
    libregexp/libregexp.c
    libregexp/libunicode.c
)

# Build shared library (DLL)
add_library(jsregexp SHARED ${SOURCES})

# Assuming Lua is installed with headers and DLLs accessible via environment or standard path
# If the Lua headers are in a non-standard location, set LUA_INCLUDE_DIR manually
# Example:
include_directories("C:/Program Files/LuaRocks/include")

# Optional: Link against Lua if jsregexp uses any symbols from it
# Example:
# target_link_libraries(jsregexp PRIVATE lua51)

# MSVC-specific settings
if(MSVC)
    target_compile_definitions(jsregexp PRIVATE -D_CRT_SECURE_NO_WARNINGS)
endif()

# Output name (e.g., jsregexp.dll)
set_target_properties(jsregexp PROPERTIES OUTPUT_NAME "jsregexp")