cmake_minimum_required(VERSION 3.15)

project(Abomerration
    VERSION 0.1.4
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h. Set
# globally rather than per-file: the next file to include windows.h hits it too,
# and the error gives no hint what happened.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(ABOMERRATION_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# The FFGL plugin, the FFGL SDK submodule and GLEW are one dependency group,
# and the OpenFX plugin belongs to none of it. Turning this OFF builds the OFX
# plugin alone, with no GL loader anywhere in the configure -- which is what
# the Linux job does, because the FFGL SDK has no Linux path, and GLEW would
# otherwise have to be installed to build something that never calls it.
option(ABOMERRATION_BUILD_FFGL "Build the FFGL plugin (needs the FFGL SDK and, off macOS, GLEW)" ON)

if(ABOMERRATION_BUILD_FFGL)
# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins. See AGENTS.md for what is wrong with it.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin and the harness share, as an OBJECT library.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor and nothing ever references that object by name, so in
# an archive the linker is entitled to drop the whole translation unit -- and the
# result is a bundle that loads, exports plugMain, and reports that it contains
# no plugins. tools/verify.sh checks both failure directions.
# ---------------------------------------------------------------------------
add_library(abomerration_core OBJECT
    source/Diag.h
    source/Diag.cpp
    source/Dispersion.h
    source/Dispersion.cpp
    source/Drive.h
    source/Drive.cpp
    source/Controls.h
    source/Controls.cpp
    source/PassBuffer.h
    source/PassBuffer.cpp
    source/Presets.h
    source/Shaders.h
    source/shaders/Vertex.cpp
    source/shaders/Copy.cpp
    source/shaders/Field.cpp
    source/shaders/Edge.cpp
    source/shaders/Disperse.cpp
    source/Abomerration.h
    source/Abomerration.cpp
)

target_include_directories(abomerration_core PUBLIC source)
target_link_libraries(abomerration_core PUBLIC ffgl::sdk)
target_compile_definitions(abomerration_core PUBLIC ABOMERRATION_VERSION="${PROJECT_VERSION}")
set_target_properties(abomerration_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers we include pull in <GL/glew.h>. The SDK links
# GLEW privately, so consumers need it too -- for the include path, and for the
# GLEW_STATIC define the static vcpkg triplet propagates. macOS uses the system
# OpenGL framework and needs none of this.
#
# GLEW arrives through vcpkg.json, which nothing in this file mentions. Deleting
# that manifest breaks the Windows job at CONFIGURE time while every macOS build
# stays green.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(abomerration_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin. One FFGL effect; PluginEntry.cpp is listed directly rather than in
# the core library so the harness does not carry the registration.
# ---------------------------------------------------------------------------
add_library(AbomerrationPlugin MODULE source/PluginEntry.cpp)
target_link_libraries(AbomerrationPlugin PRIVATE abomerration_core)

if(APPLE)
    set_target_properties(AbomerrationPlugin PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        OUTPUT_NAME "Abomerration"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Abomerration"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.abomerration"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
    target_link_libraries(AbomerrationPlugin PRIVATE
        "-framework OpenGL"
        "-framework Carbon"
        "-framework AppKit"
    )
else()
    set_target_properties(AbomerrationPlugin PROPERTIES OUTPUT_NAME "Abomerration")
endif()

# ---------------------------------------------------------------------------
# Offline harness. Where the plugin is actually verified rather than merely
# previewed: it drives the real plugin class through the real FFGL sequence in a
# headless core-profile context, including SetTime and a synthetic spectrum. See
# tools/abomtest.
# ---------------------------------------------------------------------------
if(ABOMERRATION_BUILD_TOOLS AND APPLE)
    add_executable(abomtest tools/abomtest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    target_link_libraries(abomtest PRIVATE abomerration_core z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# `cmake --install` drops the bundle straight into the user's Resolume plugin
# folder. Override with -DCMAKE_INSTALL_PREFIX=...
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # honour an explicit prefix
elseif(APPLE)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

endif()  # ABOMERRATION_BUILD_FFGL

# ---------------------------------------------------------------------------
# The OpenFX build, for Resolve/Nuke/Natron/Vegas. CPU render against the
# vendored OFX SDK subset under external/openfx (C headers + the official C++
# Support library, BSD-3). Dispersion.cpp, Drive.cpp and Controls.cpp link
# straight from source; only the per-pixel GPU machinery is mirrored in
# AbomerrationOFX.cpp.
#
# Drive.cpp links in even though OFX has neither beat information nor an FFT
# buffer. That is the point: the OFX build fills a zeroed drive::Input, gets
# scale 1 and no push back, and renders the manual lens through exactly the same
# arithmetic -- rather than carrying a second reduced copy of it that could drift.
# ---------------------------------------------------------------------------
option(BUILD_OFX "Build the OpenFX plugin" ON)

if(BUILD_OFX)
    set(OFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx")

    add_library(AbomerrationOFX MODULE
        source/ofx/AbomerrationOFX.cpp
        source/Dispersion.cpp
        source/Drive.cpp
        source/Controls.cpp
        ${OFX_SDK}/Support/Library/ofxsCore.cpp
        ${OFX_SDK}/Support/Library/ofxsImageEffect.cpp
        ${OFX_SDK}/Support/Library/ofxsInteract.cpp
        ${OFX_SDK}/Support/Library/ofxsLog.cpp
        ${OFX_SDK}/Support/Library/ofxsMultiThread.cpp
        ${OFX_SDK}/Support/Library/ofxsParams.cpp
        ${OFX_SDK}/Support/Library/ofxsProperty.cpp
        ${OFX_SDK}/Support/Library/ofxsPropertyValidation.cpp
    )
    target_include_directories(AbomerrationOFX PRIVATE
        source
        "${OFX_SDK}/include"
        "${OFX_SDK}/Support/include"
    )
    target_compile_definitions(AbomerrationOFX PRIVATE
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    )

    # Threads, explicitly. The OFX Support library's multi-thread suite calls
    # pthread_create, and on Rocky 8 -- glibc 2.28, the floor Resolve supports
    # -- that still lives in libpthread; libpthread was only folded into libc in
    # glibc 2.34. Leave it out and the plugin compiles, links, passes a symbol
    # check and a glibc-version check, and then fails at dlopen with "undefined
    # symbol: pthread_create". macOS and Windows never show it.
    find_package(Threads REQUIRED)
    target_link_libraries(AbomerrationOFX PRIVATE Threads::Threads)

    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Abomerration.ofx.bundle")
    set_target_properties(AbomerrationOFX PROPERTIES
        PREFIX ""
        SUFFIX ".ofx"
        OUTPUT_NAME "Abomerration"
        LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/MacOS"
    )
    if(APPLE)
        set(OFX_BUNDLE_IDENTIFIER "com.stoatworks.abomerration.ofx")
        configure_file(cmake/InfoOFX.plist.in
            "${OFX_BUNDLE_DIR}/Contents/Info.plist" @ONLY)
    elseif(WIN32)
        set_target_properties(AbomerrationOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            # MSVC's .exp/.lib for this target; its default directory collides
            # with the FFGL target's, which shares the output base name, and a
            # parallel build then races on the file (LNK1104).
            ARCHIVE_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/importlib"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/importlib")
    else()
        set_target_properties(AbomerrationOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Linux-x86-64")
    endif()
endif()

if(ABOMERRATION_BUILD_FFGL)
install(TARGETS AbomerrationPlugin
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)
endif()  # ABOMERRATION_BUILD_FFGL
