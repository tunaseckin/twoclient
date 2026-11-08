cmake_minimum_required(VERSION 3.15)

project(twoclient_bedrock CXX)

set(CMAKE_CXX_STANDARD 20)

include(FetchContent)

FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9
        GIT_SHALLOW TRUE)
FetchContent_Declare(nanosvg
        GIT_REPOSITORY https://github.com/memononen/nanosvg.git
        GIT_TAG 239e102ec2c691f2902e20ace2ed36ee4a35cfe6
        # header-only here, so skip its own build
        SOURCE_SUBDIR none)
FetchContent_MakeAvailable(imgui nanosvg)

add_library(twoclient_bedrock SHARED
        src/main.cpp
        src/menu.cpp
        src/shell.cpp
        src/icons.cpp
        src/sound.cpp
        src/mods.cpp
        src/config.cpp
        src/ui.cpp
        src/pages.cpp
        src/gl.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)

target_include_directories(twoclient_bedrock PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${nanosvg_SOURCE_DIR}/src)
# The game ships its own libc++_shared, so keep ours static and our symbols private
# The name the in-game menu shows; a build for the fork passes -DCLIENT_NAME=TwoClient
set(CLIENT_NAME "OneClient" CACHE STRING "Name the in-game menu shows")
target_compile_definitions(twoclient_bedrock PRIVATE IMGUI_IMPL_OPENGL_ES3 IMGUI_DISABLE_DEMO_WINDOWS
        CLIENT_NAME="${CLIENT_NAME}"
        FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../twoclient_app/assets/fonts/Poppins"
        ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_compile_options(twoclient_bedrock PRIVATE -fvisibility=hidden)
# Only the two entry points leave the library, so our static libc++ never
# stands in for the game's own
# The launcher's linker skips packed RELR relocations, which leaves static
# constructors pointing nowhere, so keep relocations in the plain format
target_link_options(twoclient_bedrock PRIVATE
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map
        -Wl,--pack-dyn-relocs=none)
set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/gl.h")
target_link_libraries(twoclient_bedrock PRIVATE EGL log)
