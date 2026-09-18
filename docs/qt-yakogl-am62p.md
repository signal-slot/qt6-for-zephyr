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
- Instanced draws run on the GPU when every per-instance array has divisor 1
  (Qt Quick 3D's instancing tables): the VDM index list carries the instance
  count and the PDS vertex fetch indexes those arrays by the instance number
  (driver `pvr_pds_vertex_stream.per_instance`,
  `pvr_vdm_emit_index_list_instanced`). Other divisors loop over the
  instances with the per-instance values fed as constants. No MSAA render
  buffers (`GL_MAX_SAMPLES` 1, so Qt never asks for a resolve blit), no
  program binaries.
- Float textures and render targets: RGBA16F, R16F and R32F are stored as
  such (TPU F16/F32 texel formats); a fragment shader that renders into one
  is a table variant compiled for that output class (blend key bits 30..31,
  `glsl2usc.py` specs `replace@f16x4` etc.), and the PBE packs with the
  matching mode. A float target is cleared by a full-screen quad through the
  clear shader: the background object leaves its pixels untouched on
  silicon. `glReadPixels` reads float targets as clamped RGBA8 or as
  RGBA/FLOAT.
- The GL scissor is pixel exact: each scissored object names an entry of the
  kick's ISP scissor table (`ISPCTL.scenable`, `CR_ISP_SCISSOR_BASE`) besides
  the tile-granular region clip.
- Dynamic indexing of uniform arrays compiles (the `pco_shc` harness sizes the
  push-constant range from the block), so Quick 3D's light loops stay loops;
  unrolled, the shadowed lights shader was 51 000 USC dwords, now 6 000.
- Program slots are 512 with the uniform blocks allocated per program at
  link: Quick 3D keeps a program per pipeline (cascaded shadow maps used up
  160).

`gl_test` cases 41 (`es3cube`), 42 (`es3array`), 43 (`es3vid`), 44 (`es3hdr`,
float targets, clear-only frames, scissor), 45 (`es3dyn`, dynamic uniform
indexing), 46 (`es3size`, `textureSize`) and 47 (`es3inst`, instancing)
cover this on the board.

The shader compiler harness lives in YakoGL's `tools/pco_shc/` (sources that
go into a Mesa 26.2.1 tree, plus the usclib patch that makes `textureSize()`
read the linear STRIDE image layout). Every machine that regenerates the table
must use the same harness build.

Bring-up knobs in our `qtbase`: `QT_RHI_GLES_CTX_MAJOR=2` makes the RHI behave
as on ES 2.0 whatever the driver reports, `QT_RHI_GLES_DISABLE_CAPS=a,b,c`
clears single capabilities. Both go through `CONFIG_QT_ENV` (a fresh Stage 2
build directory: `west build -p auto` keeps an existing `.config`).

Judge a frame by the panel camera or the `fbdump` PNG (`tools/fbdump2png.py`),
never by the six sampled pixels the display glue prints: on Coffee's start
screen all six sit on the background. With `CONFIG_QT_DEBUG_LOG` the glue
prints 128x75 thumbnails at presents 31 and 331, a full-resolution 256x150
crop at present 31 (`QZ_FBDUMP_CROP="x,y,w,h"` moves it), and both again as
tag 0 once the UI has not presented for 3 s (a static scene renders once).
The keep-alive beat adds the CPU load and the GPU kicks per 10 s. The lab
camera resets its exposure when a stream starts: set
`exposure_time_absolute` while streaming.

Picolibc lacks `fopen64`/`fseeko64`/`ftello64`/`chdir`/`getpwnam`, which the
Quick 3D asset importer plugin and `QFileDialog` reference; an undefined
function fails an AArch64 link even with `--unresolved-symbols=ignore-all`
(the call cannot reach address 0), so `src/qzephyr_libc_compat.c` provides
weak fallbacks.

### The parameter buffer a 3D scene fills

A tile based GPU tiles the whole frame before it shades any of it, and the
per-tile primitive lists go into the parameter buffer: pages the Parameter
Manager takes from a firmware freelist. A Qt Quick 3D scene is the first
content here big enough to run that buffer dry, and the firmware then posts
`FREELIST_GROW` on its own CCB and parks the render until the host answers on
the KCCB.

Two things were wrong. The answer carried KCCB command 110, the number
mainline Linux uses, while the DDK 25.2 firmware this driver boots numbers
`FREELIST_GROW_UPDATE` 108 -- 110 is `NOTIFY_WRITE_OFFSET_UPDATE`, so the
firmware consumed the answer, changed nothing, and stayed in SPM "wait for
grow" forever. That single number is what made about twenty of the examples
end in `fence N timeout`. And each freelist kept its own small reserve, so
even a correct answer ran out early; growth now comes from one pool shared by
every freelist, a quarter of the GPU carveout.

What is left is scenes whose tiling outgrows any pool this board can spare:
`simplefog` draws 2000 instances of `#Sphere`, about ten million triangles in
one kick, and spends 84 MiB of parameter buffer without finishing. The
firmware's own answer to that is a partial render (SPM), which this driver
does not yet set up.

## Status

See the git log of the three repositories; verification is on the board
(console + panel camera) as for every other Zephyr target here.
