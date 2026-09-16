# qsb2glsl -- extract the GLSL ES 1.00 variants Qt's GLES2 RHI backend hands
# to glShaderSource() from compiled .qsb shader packs, byte for byte, so
# YakoGL's offline shader table (tools/glsl2usc.py, keyed by SHA-256 of the
# source text) can be built from them.  Host tool: build against the host
# Qt (QT_HOST_PATH, the same one Stage 1 used for qsb).
#
#   cmake -S zephyr-module/tools/qsb2glsl -B build/qsb2glsl \
#         -DCMAKE_PREFIX_PATH=$HOME/qt-x11-sim && cmake --build build/qsb2glsl
#   build/qsb2glsl/qsb2glsl -o yakogl/tools/shaders/qt build-qt-zephyr-am62p/qtdeclarative
cmake_minimum_required(VERSION 3.16)
project(qsb2glsl LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
# A host Qt built with the qzephyr plugin's SDL simulator exports a
# PkgConfig::SDL2 link dependency; provide the target so Qt6Gui's config loads.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
endif()
find_package(Qt6 REQUIRED COMPONENTS Core Gui)
add_executable(qsb2glsl main.cpp)
target_link_libraries(qsb2glsl PRIVATE Qt6::Core Qt6::Gui)
# Qt Quick 3D's shadergen writes .qsbc collections; reading them needs
# Quick3DUtils (private API). Optional: without Quick 3D on the host the
# tool still handles .qsb files.
find_package(Qt6 QUIET COMPONENTS Quick3DUtils Quick3DUtilsPrivate)
if(TARGET Qt6::Quick3DUtilsPrivate)
    target_link_libraries(qsb2glsl PRIVATE Qt6::Quick3DUtilsPrivate)
    target_compile_definitions(qsb2glsl PRIVATE HAVE_QSBC)
endif()
# QShader lives in the RHI headers, which Qt ships as "semi-public" under
# the private include directories (rhi/qshader.h).
if(TARGET Qt6::GuiPrivate)
    target_link_libraries(qsb2glsl PRIVATE Qt6::GuiPrivate)
else()
    # <prefix>/include/QtGui/<version>/QtGui/rhi/qshader.h
    foreach(_dir IN LISTS Qt6Gui_INCLUDE_DIRS)
        target_include_directories(qsb2glsl PRIVATE
            "${_dir}/${Qt6Gui_VERSION}" "${_dir}/${Qt6Gui_VERSION}/QtGui")
    endforeach()
endif()
