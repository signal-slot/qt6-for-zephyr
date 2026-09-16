# Qt 6 on Zephyr with GPU rendering: YakoGL on the Verdin AM62P

How Qt Quick renders on the PowerVR BXS-4-64 of the TI AM62P (Toradex Verdin
AM62P) under Zephyr, through YakoGL, the OS-less OpenGL ES 2.0 + EGL library of
`github.com/signal-slot/yakogl` (a submodule of `powervr-bxs-am62p`, the GPU
driver repository).  The RT1170 port (`build-qt-zephyr-rt1170.sh`, raster only)
is unchanged; this is the second board, and the first with a GPU.

## Pieces

| Repository | What changed |
| --- | --- |
| `qtbase` (fork) | `mkspecs/zephyr-aarch64-g++` (Stage 1 spec for the AArch64 SDK, picolibc shims); the 64-bit `__atomic_*_8` stubs are compiled for 32-bit ARM only; the `qzephyr` QPA gets an EGL platform context (`qzephyrglcontext.*`), pbuffer offscreen surfaces, and a swap chain over the board's scan-out buffers in `QZephyrWindow`, all behind `QT_FEATURE_egl` (define `QZEPHYR_WITH_EGL`). |
| `qt6-for-zephyr` | `build-qt-zephyr-am62p.sh` (Stage 1 for AArch64 with `FEATURE_opengles2` + `FEATURE_egl` against YakoGL's headers); `zephyr-module`: `CONFIG_QT_OPENGL`, the `QT_DISPLAY_GLUE` choice with the AM62P DSS glue (`src/qzephyr_display_dss.cpp`), the Verdin board conf + overlay, `tools/qsb2glsl` (extracts Qt's GLSL ES 1.00 from `.qsb`). |
| `yakogl` | `glsl2usc.py` accepts Qt's struct-typed uniform instances (`struct buf {..}; uniform buf _16;` addressed as `_16.qt_Matrix`) and reports the dotted names to `glGetUniformLocation`; `struct shc_shader.uniforms` is a pointer to a per-shader table (no 8-uniform cap); EGL fence syncs are real (a fence records the last submitted kick and signals when the backend reports it complete); `tools/shaders/qt/` holds the Qt shader sources. |

## Stage 1: Qt for AArch64 with GL

```sh
ZEPHYR_SDK_INSTALL_DIR=... ZEPHYR_BASE=... QT_HOST_PATH=... \
YAKOGL_DIR=/path/to/powervr-bxs-am62p/yakogl ./build-qt-zephyr-am62p.sh
```

Differences from the RT1170 script: `aarch64-zephyr-elf` (gcc 14, **picolibc**:
the AArch64 SDK ships no newlib, so Stage 2 selects `CONFIG_PICOLIBC=y`),
`-mcpu=cortex-a53 -mabi=lp64` (no `--specs=picolibc.specs`: the SDK gcc
defaults to it and naming it twice breaks the architecture_test link),
`TEST_architecture_arch=arm64`, and the OpenGL features.  Qt's `FindGLESv2` /
`FindEGL` want library files to exist, so the script installs a stand-in SDK
under `$PREFIX/yakogl-sdk` (YakoGL's headers plus two **empty** archives
`libGLESv2.a` / `libEGL.a`) and points the find modules at it.  Qt's feature
tests compile but never link (`CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`),
and every `gl*` / `egl*` symbol is provided in Stage 2 by YakoGL's Zephyr module,
which Zephyr links whole.  `QtZephyrApp.cmake` re-points the find modules at the
same stand-in SDK so `find_package(Qt6 COMPONENTS Gui)` resolves the exported
`EGL::EGL` / `GLESv2::GLESv2` targets in Stage 2.

## Stage 2: the firmware

```sh
export QT_ZEPHYR_PREFIX=$HOME/qt-zephyr-am62p
west build -p always -b verdin_am62p/am62p54/a53 -d build/qt \
    <qt6-for-zephyr>/zephyr-module/qt-app -- \
    -DZEPHYR_EXTRA_MODULES="<safeui>;<powervr-bxs-am62p>;<powervr-bxs-am62p>/yakogl;<qt6-for-zephyr>/zephyr-module" \
    -DQT_APP_DIR=/path/to/a/qt-app
BOARD_HOST=torizon@<board> <powervr-bxs-am62p>/tools/kexec-zephyr/kexec_run.sh build/qt/zephyr/zephyr.bin
```

`<safeui>` is `slint-safe-ui-zephyr-verdin-am62p` (the board, SoC, DSS/DSI/
SN65DSI83 drivers); the GPU driver and YakoGL are `CONFIG_SAFEUI_GPU_PVR` /
`CONFIG_YAKOGL`.  `qt-app/boards/verdin_am62p_am62p54_a53.{conf,overlay}` set
picolibc, the GPU, the display glue, the memory budget and the same init-order
and watchdog settings as the GPU driver's `gl_slint` sample.  The inherited RTI
watchdog feed is **bounded** (`CONFIG_SOC_AM62PX_A53_FEED_INHERITED_WDT_SECONDS`)
because nothing can reset the lab board remotely; raise it for a longer session,
never set it to 0 on a kexec image.

### Display glue (`src/qzephyr_display_dss.cpp`)

Qt owns `CONFIG_QT_SCANOUT_BUFFERS` (3) XRGB8888 buffers of the panel size in
`.noinit` and flips the DSS application plane to the one holding the latest
complete frame.  The raster path (QtWidgets) copies each `flush()` into the next
buffer; the GL path hands the buffers to the QPA as `struct gles_native_window`
(`qzephyr_gl_native_window`), flips on request (`qzephyr_gl_present`) and
exposes the vsync counter (`qzephyr_gl_vsync_count` / `qzephyr_gl_wait_vsync`).
Zephyr on the A53 maps RAM identity, so a buffer's address is its physical
address for both the GPU MMU and the DSS DMA.

### Swap chain (QPA, `qzephyrwindow.cpp`)

YakoGL's window surfaces are single-buffered and `eglSwapBuffers` returns when
the *previous* frame completes (the CPU records N+1 while the GPU renders N).
The QPA therefore creates one EGL surface per scan-out buffer and, per swap:
waits until the display has left the buffer about to be rendered into, calls
`eglSwapBuffers`, presents the previous (now complete) frame, marks the just
submitted one pending, rotates, and makes the next surface current.  A fence
sync taken after the swap is polled from the event loop so a scene that stops
animating still gets its last frame on screen; while animation is continuous the
next swap presents it first and the poll finds nothing to do.

## Shaders

YakoGL has no on-target compiler: `glShaderSource` looks the text up by SHA-256
in a table compiled on the host by `pco_shc` (`yakogl/tools/glsl2usc.sh`).  Qt's
GLES2 RHI backend passes the "GLSL 100 es" entry of each `.qsb` pack unchanged,
so the exact bytes come from the Stage 1 build:

```sh
cmake -S zephyr-module/tools/qsb2glsl -B build/qsb2glsl -DCMAKE_PREFIX_PATH=$QT_HOST_PATH
cmake --build build/qsb2glsl
build/qsb2glsl/qsb2glsl -o <yakogl>/tools/shaders/qt build-qt-zephyr-am62p/qtdeclarative
PCO_SHC=... <yakogl>/tools/glsl2usc.sh
```

Vertex shaders come in a Standard and a Batchable variant (the batch renderer
uses the latter for merged batches); both are extracted.  Blend states Qt uses
(`one,inv_src_alpha` premultiplied, no blend for opaque materials, every
channel masked off for stencil clips) are already in the `BLENDS` list.

Not covered: shaders that need `GL_OES_standard_derivatives` (Qt only selects
the `_fwidth` text variants when the extension is advertised, which YakoGL does
not), `samplerCube`, nested struct uniforms, and any `ShaderEffect` whose GLSL
is not in the table (add its `.qsb` output the same way).

## Status

See the git log of the three repositories; verification is on the board
(console + panel camera) as for every other Zephyr target here.
