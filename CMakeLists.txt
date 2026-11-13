cmake_minimum_required(VERSION 3.16)
project(CraftNet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Install the matching raylib 6.0 package for the selected toolchain first.
if(PSP)
    find_path(RAYLIB_INCLUDE_DIR raylib.h REQUIRED)
    find_library(RAYLIB_LIBRARY raylib REQUIRED)
else()
    find_package(raylib 6.0 REQUIRED)
endif()
find_package(ZLIB REQUIRED)

add_executable(CraftNet
    src/main.cpp
    src/world.cpp
    src/protocol.cpp
    src/net.cpp
    src/offline_uuid.cpp
    src/chunk.cpp
    src/session.cpp
    src/render.cpp
    src/render_psp.cpp
    src/movement.cpp
    src/chat.cpp
    src/servers.cpp
    src/browser.cpp
    src/players.cpp
    src/skin_cache.cpp
    src/avatars.cpp
    src/inventory.cpp
    src/game_ui.cpp
    src/play_effects.cpp
    src/scoreboard.cpp
    src/pause_menu.cpp
)
target_include_directories(CraftNet PRIVATE src)
if(PSP)
    target_include_directories(CraftNet PRIVATE ${RAYLIB_INCLUDE_DIR})
    target_link_libraries(CraftNet PRIVATE ${RAYLIB_LIBRARY} png ZLIB::ZLIB glut GLU GL pspvfpu)
else()
    target_link_libraries(CraftNet PRIVATE raylib ZLIB::ZLIB)
    if(WIN32)
        target_link_libraries(CraftNet PRIVATE ws2_32)
    endif()
endif()
if(MSVC)
    target_compile_options(CraftNet PRIVATE /W4)
else()
    target_compile_options(CraftNet PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PSP)
    target_link_libraries(CraftNet PRIVATE pspdebug pspdisplay pspge pspctrl pspgu pspgum pspaudio pspaudiolib pspnet pspnet_inet pspnet_apctl pspnet_resolver psputility pspuser psppower)
    create_pbp_file(TARGET CraftNet TITLE "CraftNet" MEMSIZE 1 BUILD_PRX)
endif()
