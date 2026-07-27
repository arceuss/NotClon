cmake_minimum_required(VERSION 3.16)
project(NotClon C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# NOTCLON_STATIC_RUNTIME -- link the C/C++ runtime in rather than depend on it.
# Without this the editor needs MSVCP140.dll, present only if the user happens
# to have a Visual C++ redistributable installed; a release is expected to run
# on a machine that has never had a compiler on it.
#
# Set GLOBALLY, before any target exists, and that is not a style choice: the
# static and dynamic CRTs disagree about whether every libc symbol is
# dllimport, so a build where some objects got the flag and some did not fails
# to link with a page of "undefined symbol: __declspec(dllimport) fopen".
#
# SDL3 is NOT covered: upstream ships a DLL and an import library with no
# static lib, so SDL3.dll travels in the bundle. That is SDL's normal
# distribution shape, and it is zlib-licensed, so shipping it is fine.
# ---------------------------------------------------------------------------
option(NOTCLON_STATIC_RUNTIME "Link the C/C++ runtime statically" OFF)
if(NOTCLON_STATIC_RUNTIME)
  # CMAKE_MSVC_RUNTIME_LIBRARY, not a raw -fms-runtime-lib flag: CMake emits
  # its own `-D_DLL -Xclang --dependent-lib=msvcrt` AFTER whatever you put in
  # CMAKE_CXX_FLAGS, so the raw flag is silently overridden and the build fails
  # at link with "undefined symbol: __declspec(dllimport) fopen". This variable
  # is the knob CMake actually honours, for clang-on-MSVC as well as MSVC.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# ---------------------------------------------------------------------------
# Lua 5.1.5, vendored from the upstream tarball at lua.org (MIT).
#
#   https://www.lua.org/ftp/lua-5.1.5.tar.gz
#   sha256 2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333
#
# Upstream on purpose. StepMania-4's src/lua-5.1 is 5.1.1 and 53 of its files
# differ from 5.1.5 -- mostly four upstream point releases, but the point is
# that you cannot tell those from an engine patch by looking, and nothing here
# needs SM's variant. COPYRIGHT ships alongside.
#
# NOT NotITG/Program/lua50.dll: every binary in that folder is x86 and NotClon
# is x86-64, so it cannot be loaded in-process at all -- an ABI wall, not a
# preference. 5.1 is also the right language level; NotITG ships a patched
# 5.0.3 that has `#` and `%`, which vanilla 5.0.3 rejects, and 5.1 has both
# natively so modfile syntax parses unpatched.
#
# lua.c, luac.c and print.c are excluded: the first two carry main() and
# print.c belongs to luac.
# ---------------------------------------------------------------------------
file(GLOB NOTCLON_LUA_SRC third_party/lua/*.c)
add_library(notclon_lua STATIC ${NOTCLON_LUA_SRC})
target_include_directories(notclon_lua PUBLIC third_party/lua)
if(MSVC OR WIN32)
  target_compile_definitions(notclon_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(notclon_lua PROPERTIES C_STANDARD 99)

# ---------------------------------------------------------------------------
# The shared render core. Both binaries link this so a frame has exactly one
# implementation -- the editor must preview what the encoder will write.
# ---------------------------------------------------------------------------
add_library(notclon_core STATIC
  src/gl.cpp
  src/renderer.cpp
  src/modfile.cpp
  src/smimport.cpp
  src/actor.cpp
  src/video.cpp
  src/background.cpp
)
target_include_directories(notclon_core PUBLIC src third_party)
# gl.h drags in windows.h, whose min/max macros collide with <algorithm>.
target_compile_definitions(notclon_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX)

target_link_libraries(notclon_core PUBLIC notclon_lua)

if(WIN32)
  target_link_libraries(notclon_core PUBLIC opengl32 gdi32 user32)
endif()

# Assets ship beside the executables: the binaries resolve <exe>/assets/ at
# runtime, so build/ is a complete, movable install -- zip it and it runs.
function(nc_copy_assets tgt)
  add_custom_command(TARGET ${tgt} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:${tgt}>/assets)
endfunction()

# ---------------------------------------------------------------------------
# notclon -- offline renderer, pipes to ffmpeg
# ---------------------------------------------------------------------------
add_executable(notclon src/main.cpp)
nc_copy_assets(notclon)
target_link_libraries(notclon PRIVATE notclon_core)

# ---------------------------------------------------------------------------
# notclon-editor -- SDL3 + Dear ImGui modchart editor
# ---------------------------------------------------------------------------
set(SDL3_ROOT ${CMAKE_SOURCE_DIR}/third_party/SDL3)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${SDL3_ROOT}/include)

add_executable(notclon-editor editor/main.cpp editor/audio.cpp)
nc_copy_assets(notclon-editor)
target_link_libraries(notclon-editor PRIVATE notclon_core imgui
                      ${SDL3_ROOT}/lib/x64/SDL3.lib)
target_include_directories(notclon-editor PRIVATE ${SDL3_ROOT}/include)

# SDL3 is shipped as a DLL; put it next to the exe so the editor just runs.
add_custom_command(TARGET notclon-editor POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${SDL3_ROOT}/lib/x64/SDL3.dll $<TARGET_FILE_DIR:notclon-editor>)

foreach(t notclon_core notclon notclon-editor imgui notclon_lua)
  if(MSVC)
    target_compile_options(${t} PRIVATE /O2)
  else()
    target_compile_options(${t} PRIVATE -O2 -Wno-deprecated-declarations)
  endif()
endforeach()

