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
not), and any `ShaderEffect` whose GLSL is not in the table (add its `.qsb`
output the same way).

`GLSL2USC_CACHE=<dir>` keeps the compiler output keyed by the compiled text,
stage, blend variant and compiler binary: a regeneration after adding a few
shaders then takes seconds instead of the half hour the whole table costs.

### OpenGL ES 3.0 (Qt Quick 3D)

Since 2026-09-17 YakoGL reports `OpenGL ES 3.0` (an EGL client version 2 or 3
request gets the same context; Qt reads the version back from `GL_VERSION`).
Qt's RHI decides from the major version alone whether it may use instancing,
`texelFetch`, float/depth textures, texture arrays, mip levels as render
targets and `glMapBufferRange`, and Qt Quick 3D ships its skybox, image based
lighting and shadow shaders only as GLSL ES 3.00: on an ES 2.0 context those
demos cannot work. Qt Quick's own shaders stay GLSL ES 1.00 (the RHI tries the
300 es variant of a `.qsb` first and falls back), so the table holds both.

What the core provides on top of ES 2.0, and how:

- `qsb2glsl` extracts the 300 es variant when a `.qsb` has one; the run-time
  generated Quick 3D materials come from the Stage 2 shadergen pre-generation
  and from harvesting the console (`tools/harvest-shaders.py`) as before.
- `glsl2usc.py` translates GLSL ES 3.00 declarations (layout locations, in/out,
  flat, the ES 3.0 sampler and integer types) to the Vulkan GLSL `pco_shc`
  compiles; `gl_VertexID`/`gl_InstanceID` land in vtxin registers the PDS
  vertex fetch writes (driver `pvr_pds_render_vertex_program_ids`).
- Cube maps and 2D texture arrays are stored as one linear texture with the
  six faces / the layers stacked vertically (mip levels stacked the same way)
  and the translator rewrites the sampling into face/layer arithmetic
  (`pvr_cube*`, `pvr_arr*` helpers; an array's layer count is the hidden
  uniform `pvr_layers_<sampler>` the core fills at draw time). The TPU's real
  cube/array types need the twiddled memory layout this core does not write.
  `glFramebufferTexture2D` with a face target, `glFramebufferTextureLayer` and
  mip levels attach that sub-rectangle as the render target.
- Instanced draws loop over the instances feeding the divisor attributes as
  constants (fine for a few instances; thousands need a PDS instance-rate
  stream). No MSAA render buffers (`GL_MAX_SAMPLES` 1), no program binaries.
- Float texel formats (RGBA16F light probes, R16F shadow maps) are stored as
  RGBA8 for now: HDR values clamp, shadow depth has 8 bits.

`gl_test` cases 41 (`es3cube`), 42 (`es3array`) and 43 (`es3vid`) cover the
emulation and the vertex id on the board.

Bring-up knobs in our `qtbase`: `QT_RHI_GLES_CTX_MAJOR=2` makes the RHI behave
as on ES 2.0 whatever the driver reports, `QT_RHI_GLES_DISABLE_CAPS=a,b,c`
clears single capabilities. Both go through `CONFIG_QT_ENV` (a fresh Stage 2
build directory: `west build -p auto` keeps an existing `.config`).

Judge a frame by the panel camera or the `fbdump` PNG (`tools/fbdump2png.py`),
never by the six sampled pixels the display glue prints: on Coffee's start
screen all six sit on the background.

## Status

See the git log of the three repositories; verification is on the board
(console + panel camera) as for every other Zephyr target here.
