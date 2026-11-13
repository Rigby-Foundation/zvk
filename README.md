# zvk — Vulkan graphics for sic

A separate experimental Vulkan frontend alongside zgl. `libzvk` owns its
API objects, memory, descriptors, pipelines, synchronization, recorded
commands and SPIR-V compiler. It renders offscreen and into zwm windows,
sharing `libvirgl` for GPU resources and command encoding. On macOS a
private host bridge executes compute kernels on Metal; graphics remains on
virgl/OpenGL. Both shader frontends are independent. It does not call
libzgl's OpenGL API or use Mesa's Vulkan driver, NIR or SPIR-V compiler.

**This is a Vulkan 1.0 API subset, not a complete or conformant Vulkan 1.0
implementation.** All 137 core entry points have dispatch entries, but
unsupported capabilities return errors. Compute has tested storage-image and
texel-buffer paths, required format queries, BC textures and 1D/3D images;
robust buffer access is implemented. GPU color/depth multisample fetches and
input attachments, separate/immutable graphics samplers and larger compute
descriptor sets now work. Stencil sampling/inputs, bounded graphics storage
reads and compute multisample fetches now have GPU tests. Compute also supports
ordinary/filtered and comparison sampling, aggregate/pointer function arguments,
matrices, extended integer results and specialization expressions. Additional shader/API
behavior and a passing full Vulkan 1.0 mustpass result remain unfinished.
[The completion checklist](CONFORMANCE.md) tracks the Vulkan 1.0 target and the distinction between profile tests and conformance.

## Build and test

From the workspace root on the Mac:

```sh
make vulkan-tools                   # pinned Khronos headers and GLSL fixture compiler
make vulkan-test                    # API, compiler and headless swapchain tests on the host GPU
make vulkan-validation-tools        # optional: isolated Khronos loader and validation layer
make vulkan-validation-test         # real ICD loading, rendering and synchronization validation
make vulkan-profile-audit           # initial required-limit audit; fails while gaps remain
make vulkan-cts-tools               # pinned official CTS; substantial host build
make vulkan-cts-test CTS_CASE='dEQP-VK.compute.pipeline.basic.*' CTS_NAME=compute-basic
make ARCH=aarch64 vulkan-window     # SDK, examples and GPU-capable zwm in the ARM64 sysroot
make vulkan-window-test-native      # ARM64 VM: tests, native compilation, animation, resize and close
make ARCH=aarch64 vulkan-sdl        # shared loader/ICD, SDL2, demos and distributable SDK tar
make vulkan-sdl-test                # VM: native SDL compilation, loader failures, TLS and resize
```

`make ARCH=x86_64 vulkan-window` builds the corresponding x86 sysroot.
`make ARCH=aarch64 vulkan` installs just the SDK and examples, without
building/replacing the compositor. Windowed presentation needs the updated
zwm server built by `vulkan-window`; old servers do not advertise the
presentation acknowledgement protocol.

From this directory, the individual targets are:

```sh
make deps tools
make -C ../zgl/tests/vrend host       # isolated virglrenderer with existing host fixes
make HOST=1 check
make HOST=1 SAN=1 check              # separate ASan + UBSan build
make HOST=1 check-metal-graphics      # experimental Metal stages, GPU execution and draw packets
make HOST=1 SAN=1 check-metal-compiler # malformed-input checks for the Metal graphics compiler
make HOST=1 check-formats            # CPU conversions + 32768 randomized BC blocks
./build/host/zvktest --formats       # GPU formats, texels, vertices and dimensions
./build/host/zvktest --dimensions    # 1D/3D/cube storage, sampling, copies and blits
./build/host/zvktest --graphics      # graphics, structured control flow, UBO bits and queries
./build/host/zvktest --mutable       # typed view clears/draws/resolves and private query exclusion
./build/host/zvktest --robust        # descriptor bounds, guarded stores/atomics and vertex fetch
./build/host/zvktest --multisample   # per-sample color/depth, arrays, inputs, coverage and texture operands
./build/host/zvktest --atlas         # simultaneous graphics resource limits and large SSBOs
./build/host/zvktest --capacity      # 40 simultaneous descriptors, aliases and storage image copyback
./build/host/zvktest --textures      # separate/immutable samplers, offsets, LOD, bias and gradients
./build/host/zvktest --stencil       # all stencil values, samples, inputs, storage reads and query exclusion
./build/host/zvktest --samplers      # sixteen images per stage in one 48-sampler layout and queue priorities
make HOST=1 shared                  # build/host/libzvk.dylib
./build/host/zvktriangle build/triangle.ppm
make validation-tools
make HOST=1 check-loader            # driver through the Khronos loader
make HOST=1 check-validation        # core + synchronization validation
make ARCH=aarch64 install
make check-sic                      # native API/headless tests in a disk snapshot
make check-window                  # animated window and maximize/restore/close checks
make check-window-native           # also compile the window example using sic's clang
make check-sdl                     # shared loader/SDL2, native builds and installed SDK
```

Requirements: Python 3.12+, LLVM/Clang, `ld.lld`, SPIRV-Tools (`spirv-as`
and `spirv-val`), and the existing sic kernel/libc sysroot. Host rendering
uses macOS CGL/OpenGL and virglrenderer; its host build needs Meson, Ninja,
pkg-config and libepoxy. LLVM/LLD are discovered through Homebrew on this
Mac; command-line `CC`, `AR`, `LD`, `SYSROOT` and environment `GLSLANG`
overrides are supported. `HOST=1` currently targets macOS.

The expanded `check-sdl` run allows 600 seconds. To diagnose ARM64 idle/timer
stalls, use `make check-sdl SIC_TEST_FLAGS=--idle-poll` (or pass `--idle-poll`
to `scripts/test-sic.py`). This uses the existing sic `idle=poll` option,
disables WFI and keeps CPUs busy. Record this mode when reporting results.
ARM64 monotonic time now follows the hardware counter, so delayed timer
interrupts do not stretch sleeps/timeouts. The SDL self-test independently
compares guest time with host time across its three-second compositor pause.

`make tools` checksums and builds glslang 15.1.0 under `build/tools`. If
CMake is missing it supplies a pinned local CMake wheel there. GLSL fixtures
are compiled for Vulkan 1.0 and embedded in the examples/tests. glslang is
only test/build tooling; no glslang code is linked into the driver.

`validation-tools` checksums Vulkan-Loader and Vulkan-ValidationLayers
1.3.296 archives and builds their known-good dependencies at pinned commits.
Everything installs under `build/tools/vulkan-sdk`. It does not change the
system's Vulkan loader, drivers, layers or Python installation.

## Windowed presentation

`include/zvk_zwm.h` declares the private, experimental
`VK_SIC_zwm_surface` platform API and `vkCreateZwmSurfaceSIC`. Its name and
structure number are local to sic, not registered Khronos identifiers.
Enable it and `VK_KHR_surface` at instance creation, and enable
`VK_KHR_swapchain` at device creation. After surface creation use the usual
surface queries, swapchain creation, image acquisition and queue presentation.
Applications can link directly to `libzvk`, or use the native Khronos loader
and SDL2 path below.

The application owns its zwm connection/window and must keep them alive
until the swapchains and surface are destroyed. Serialize libzwm event
handling and Vulkan WSI operations on the same connection. Surface queries
pump pending window messages while preserving application events in their
original queue; resizes are visible before the application consumes them.

The current presentation mode is FIFO, with two or three images and one
layer. Rendering finishes before submission returns. A window's displayed
image remains reserved until another image replaces it and the compositor
acknowledges a completed frame. Destroying a displayed swapchain first
asks zwm to detach the client resource and waits for that acknowledgement.
Presentation/detach wait for acknowledgement without a fixed frame timeout;
a slow frame does not imply surface loss. Disconnects/window destruction
still end the wait. This prevents reuse or destruction while the compositor
is sampling it.
Presentation uses a GPU blit to match zwm's row convention; it does not
read rendered frames back to the CPU.

A resized window makes acquisition/presentation return
`VK_ERROR_OUT_OF_DATE_KHR`; recreate with `oldSwapchain`. A disconnected
server or unavailable window reports `VK_ERROR_SURFACE_LOST_KHR`. The new
zwm protocol advertises `ZWM_HELLO_PRESENT`, with per-window completion
counts and GPU detach acknowledgement. Existing GL clients remain compatible.

With the installed programs and an updated GPU zwm already running, type
`zvkwindow` at the sic shell. Escape, Q or the close button exits. The
`--frames 120` exits after a bounded interactive run. Normal rendering
presents without CPU readback; the `--selftest` mode checks GPU pixels as
the triangle rotates, maximizes and
restores the window, recreates the swapchain, then requests/handles close.

The VM helpers require `../build/arm64-disk.img`, `../build/arm64-fat.img`,
the ARM64 kernel/initrd, and virgl-enabled QEMU at
`~/qemu-virgl/bin/qemu-system-aarch64`; override it with `QEMU_VIRGL_ARM64`.
They use `-snapshot`, verify the mounted root disk, supply the updated
programs on a generated test stick and propagate failures. They leave the
persistent disk unchanged. Window tests also capture the compositor through
its GPU screenshot path and save `../build/zvk-window-{8,24,48}.ppm`.

## Compile an application

The native SDK installs `vulkan/`, `vk_video/`, `zvk.h`, `zvk_zwm.h` and
`zwm.h` under the sysroot's `usr/include`, and `libzvk.a`, `libvirgl.a` and
`libzwm.a` under `usr/lib`. `zvktest`, `wsitest`, `zvktriangle` and
`zvkwindow` install into the `rootfs/bin` overlay. Installing a host sysroot
does not copy that SDK onto an existing persistent disk.

`examples/triangle.c` renders a standalone offscreen triangle to PPM.
`examples/window.c` creates the animated zwm window. Both use the
`shaders.h` produced by `make shaders` from checked-in GLSL/SPIR-V fixtures.
Applications may instead load their own SPIR-V binaries at runtime.

With the SDK installed under `/usr` on sic:

```sh
clang -O2 -I/path/to/generated/shaders /path/to/window.c \
  -lzvk -lvirgl -lzwm -lm -lpthread -o /tmp/zvkwindow
/tmp/zvkwindow
```

`check-window-native` supplies a small SDK and both example sources on the
snapshot test stick, then compiles and runs the window inside sic:

```sh
clang -O2 -I/mnt/nvme0n1/sdk/include -I/mnt/nvme0n1/sdk/src \
  /mnt/nvme0n1/sdk/src/window.c -L/mnt/nvme0n1/sdk/lib \
  -lzvk -lvirgl -lzwm -lm -lpthread -o /tmp/zvkwindow-native
/tmp/zvkwindow-native --selftest
```

## Loader and validation

`HOST=1 shared` builds a development driver. The loader test targets write
`build/host/zvk_icd.json` pointing to that exact library and set
`VK_DRIVER_FILES` for each test. The manifest is never installed system-wide.
Driver/loader interface version 5 is negotiated; surface handles remain
owned by the driver. Extension procedures are filtered by enabled scopes.

The Mac loader tests use `VK_EXT_headless_surface` and
`VK_KHR_swapchain`, plus the Khronos validation layer with synchronization
validation enabled. They render/read back a triangle and submit six
acquire/clear/present cycles. The native ELF loader uses the same pinned
Khronos release with a local private zwm surface port. Its dispatch tables
are regenerated from the augmented registry; the Mac loader stays unmodified.

## ARM64 persistent VM

```sh
make run-gl-arm64-disk
```

The GL disk target and its `run-gl-arm64` alias build the SDK, update it on
`build/arm64-disk.img` when its contents change, then boot the current UEFI
kernel/initrd with that persistent root. Both GL and Vulkan are available.
With zwm or the desktop active, run `sdlwindow` (SDL2/loader) or `zvkwindow`
(direct zVk). Native compilation sources are in `/usr/share/zvk/examples`.

`make prepare-gl-arm64-disk` performs just the build/install preparation.
The installer uses sic's filesystem driver, saves replaced files in
`/var/backups/zvk/<version>-before.tar`, and stores its SDK digest under
`/var/lib/zvk`. It leaves the rest of the persistent filesystem in place.
The target keeps the FAT scratch disk on `nvme1n1`, puts the boot image on
`nvme2n1`, and prefers that fresh boot image over an older installed kernel.
It builds/uses the isolated patched virglrenderer for both graphics APIs.

For an automated check of that exact UEFI launch command:

```sh
make run-gl-arm64-disk QEMU_VIRGL_ARM64='python3 tools/vulkan/test-arm64-disk.py'
```

Preparation still installs the SDK persistently; the final verification boot
uses a snapshot, checks default ICD discovery, compiles the examples inside
sic and runs the SDL2 window. Its log is `build/vulkan-arm64-uefi-serial.log`
from the repository root.

## Native shared loader and SDL2

From the repository root:

```sh
make ARCH=aarch64 vulkan-sdl
make vulkan-sdl-test
```

This builds `libvulkan.so.1`, a separate `libzvk.so` ICD, PIC GPU/zwm/GL
libraries, and `libSDL2.so`. SDL retains its existing software and OpenGL
paths and gains `SDL_Vulkan_LoadLibrary`, extension enumeration, surface
creation and drawable-size queries. Vulkan functions in the SDL demo come
from `SDL_Vulkan_GetVkGetInstanceProcAddr`; the executable has no direct
zVk or Vulkan link. The ICD itself uses libvirgl/libzwm and has no libzgl
frontend dependency.

The native Clang driver still defaults to static linking. The supplied
`examples/Makefile.sic` compiles with native Clang and explicitly links a
PIE using native `ld.lld` and shared musl. Existing static builds keep their
defaults. To compile the examples **inside sic**, after installing the SDK:

```sh
cp -R /usr/share/zvk/examples /tmp/zvk-examples
cd /tmp/zvk-examples
make -f Makefile.sic SDK=/usr ARCH=aarch64
./sdlwindow
```

Run with the updated GPU zwm server active. Q/Escape or the close button
exits. The self-test's keyboard-close request is supplied by the VM harness;
use the ordinary interactive run when launching by hand.

The host build produces `zvk/build/zvk-sdk-aarch64.tar`. Copy it to a disk
visible to sic, then install **inside sic** with:

```sh
tar -xf /path/to/zvk-sdk-aarch64.tar -C /
```

The archive contains the runtime, headers, examples and updated zwm. It does
not include a compiler: the existing native Clang/LLD and make must already
be installed. Standalone SDK build/test targets do not modify the persistent disk; the
GL disk launch target above installs the SDK.
The sysroot also receives the shared libraries and a runtime overlay for
new root images. The manifest installs at `/usr/share/vulkan/icd.d/zvk.json`
and names `../../../lib/libzvk.so` relative to that directory. The native
loader uses normal ICD discovery; override it per process with
`VK_DRIVER_FILES=/absolute/path/to/zvk.json`. For a staged SDK, also set
`LD_LIBRARY_PATH=/path/to/sdk/lib`; SDL honors `SDL_VULKAN_LIBRARY`.

`make vulkan-sdl-test` uses a snapshot and stages its own manifest, libraries
and sources. It checks both cross and native builds. The existing sic ELF
interpreter and musl dynamic linker already support the needed shared
libraries, constructors and TLS; no kernel ABI change was needed. The slow
compositor test also fixed the kernel's default `SIGCONT` delivery: resuming
a task no longer terminates it. Custom signal handlers remain deliverable.

On the Mac test backend, multisampling needs the isolated virglrenderer
patch in `zgl/tests/patches/virglrenderer-multisample.patch`. Run
`make -C zgl/tests/vrend host` after updating it. The VM harness uses that
local library for QEMU; system virglrenderer is untouched. An unpatched
host may advertise only single-sample formats, which zVk respects.
Restricted texture views use GPU copies when the host lacks texture-view
support. This preserves view contents/ranges but costs extra GPU work.
Multisample sampling additionally needs
`zgl/tests/patches/virglrenderer-multisample-views.patch`: the isolated host
omits invalid GL sampler/LOD parameters for multisample texture targets.
zVk applies their view swizzles in its GPU shader and uses sample-preserving
blits for restricted array views. Rebuild the isolated host after updating it.

## GPU compute on macOS

The isolated virglrenderer patch now has a private Metal compute transport.
It leaves standard virgl/OpenGL compute capabilities unchanged. zVk advertises
its compute queue only when that transport is present; an unpatched host keeps
the graphics-only profile. No CPU shader interpreter is used.

Rebuild the host bridge with `make -C zgl/tests/vrend host`. The normal
`make run-gl-arm64-disk` target builds and selects this library automatically.
Inside sic, `/bin/zvktest --compute` checks the GPU path. For a native application:

```sh
cp -R /usr/share/zvk/examples /tmp/zvk-compute
cd /tmp/zvk-compute
make -f Makefile.sic SDK=/usr compute
./compute
```

The application links only the public Khronos Vulkan loader. Its result is
`COMPUTE_DEMO_PASS 64 GPU results verified`. The updated SDK is installed by
the normal GL disk launch/preparation target; standalone SDK builds update
the sysroot/archive, not an existing persistent root.

Current compute limits are 128 local invocations, 16 KiB shared memory and
65535 workgroups per axis. Storage/uniform buffers, dynamic offsets and push
constants and fixed buffer descriptor arrays with constant indices are supported.
A compute kernel can now use all 40 descriptors allowed by the supported
classes together: 12 UBOs, 8 storage buffers, 16 uniform texel buffers and
4 storage images/texel buffers. Multisample sampled images can occupy the
sixteen sampled-image slots instead of uniform texel buffers. `texelFetch`,
size and sample-count queries support 2D and 2D-array multisample images.
A GPU fetch pass preserves each sample in a buffer snapshot before Metal
execution; its additional backing has the existing 256 MiB allocation limit.
Kernels above 28 descriptors pack unique
allocations into one GPU buffer and a range table, retaining descriptor bounds
and allocation aliases. The wire transport still uses at most 29 physical
buffers; a packed dispatch uses two. The staging arena has the existing
256 MiB allocation budget and introduces extra copies.
Nonrecursive function calls accept scalar/vector/aggregate values and memory
pointers, preserving private side effects. Recursive calls are rejected.
Allocation aliases share one Metal buffer. Execution
is synchronous: host-side staging copies data between OpenGL buffers and Metal
buffers, then makes writes visible in coherent guest mappings. This is GPU
execution with CPU transfers, rather than a zero-copy interop path.

Typed storage images and texel buffers support all 39 SPIR-V storage image
formats: required normalized, signed/unsigned integer, half/full float and
packed formats. The extended formats require enabling
`shaderStorageImageExtendedFormats`. Views include 1D/1D arrays, 2D/2D arrays,
3D volumes and cubes, with nonzero mip/base layer where applicable. R32 signed
and unsigned resources support relaxed 32-bit atomics. Uniform texel buffers
also support additional color formats, including BGRA8 and three-component
numeric formats, with offsets/ranges up to 65536 texels. Fixed descriptor
arrays use constant indices. Packed data travels through GPU buffers with
per-view extent, component widths and bit offsets; shader conversions, reads,
writes and atomics execute on Metal. Results are copied back into graphics
textures for subsequent transfers and rendering. Byte-aligned narrow-format mip
views retain their byte offsets through both direct and packed dispatches,
with neighboring mip data preserved.

Graphics sampling supports normalized, integer, float, sRGB and packed color
formats, plus D16/D32/D24S8 depth. Integer vertex attributes retain their signed
values and typed default components across 8/16/32-bit and packed formats.
1D arrays support 4096 texels by 256 layers; 3D images support 256 cubed.
Volume transfers, restricted mip views and nearest/linear blits cover every
Z slice, including reversed Z and self-image mip blits. Conservative slice
copies work around the pinned host's bulk volume-transfer limitations.

All 16 BC1–BC7 formats are available with `textureCompressionBC`. Transfers
retain the original compressed blocks; texture upload decodes them using the
vendored MIT-licensed bcdec, preserving signed/HDR values and sRGB sampling.
B4G4R4A4 similarly expands to RGBA8 on this host. These texture preparation
conversions run on the CPU; shader execution, filtering, 3D blits and rendering
run on the GPU. Expanded backing increases memory use. Format properties
report actual sampled/render/storage capabilities; compressed images cannot
be storage or rendering destinations.

The base bridge requires macOS 12+ and a Metal device. Memory-only fences use
Metal 3.2 on Apple silicon with macOS 15+; older hosts reject shaders requiring
these fences. Fences preserve divergent control flow without introducing an
execution rendezvous. Its sources, private wire ABI
and standalone GPU/bounds checks are documented in [zgl/host](../zgl/host/README.md).
`ZVK_DISABLE_METAL_COMPUTE=1` on the host disables the capability for fallback
checks. `ZVK_DISABLE_METAL_IMAGES=1` disables the newer image/texel path while
retaining buffer compute. Unsupported shader operations fail compilation. Compute sampling now includes
explicit LOD/gradients, filtering, comparison reads and sampler/image function
arguments; address calculations and filtering execute on Metal. Aggregate
values, matrix algebra, extended integer results, half quantization, memory
copies and specialization expressions are supported. Acquire/release atomics
use Metal 3.2 fences around native atomics. Complete memory-model coverage and
dynamic descriptor indexing still require further acceptance tests.

Vertex and fragment resources use a packed RGBA32_UINT texture atlas. Native
GPU code performs descriptor-relative buffer reads, runtime-array lengths,
texel decoding, image queries, sampling/filtering and comparisons. The atlas
removes the host's 16 KiB storage-buffer window and texture-unit limit. A
regression executes 12 UBOs, 8 large runtime SSBOs, 16 sampled images with 16
independent samplers, 4 storage-image reads and 8 input attachments together.
Descriptor tables accept high binding numbers and zero-count bindings. The
same regression checks cached shader lifetime after its pipeline cache and an
original pipeline are destroyed. All read-only graphics buffers use this path;
integer UBO reads preserve arbitrary bit patterns and observe host rewrites.
Graphics storage writes remain disabled.

The aggregate sampler limit is 48: a regression uses sixteen distinct sampled
images in each of vertex, fragment and compute stages under one pipeline layout.
Queue priorities map to two hint classes, `[0, .5)` and `[.5, 1]`. The current
implementation exposes one queue per device and makes no scheduling/QoS guarantee.

## Robust buffer access

`robustBufferAccess` is available through `VkPhysicalDeviceFeatures`; enable
it when creating the device. Compute buffer/texel loads check descriptor ranges
on the GPU and return zero outside them; stores and relaxed atomics ignore
out-of-range addresses. Matrix buffer copies support column/row-major layouts,
array strides and component bounds, including non-square matrices.

Graphics UBO staging copies only complete 32-bit words inside the descriptor
range and zero-fills the rest of the shader's declared accesses. Dynamic array
loads outside the declared array return zero rather than retaining an earlier
loop value. Dynamic descriptor offsets apply before range checks.

Robust vertex input stages each attribute into bounded, zero-padded GPU storage.
It preserves the application's indices, base vertex and vertex/instance builtins,
including negative base vertices, first instances and zero strides. Integer and
packed formats retain their existing conversions. Entirely absent attributes
use a constant zero element even for very large indices. Mixed valid/invalid
index spans can require substantial staging memory; the existing 256 MiB
allocation budget applies. This is a conservative implementation, with extra
copies and memory use. Host OpenGL robustness extensions are not required.

## Implemented profile

| Area | Current behavior |
| --- | --- |
| Discovery | Vulkan 1.0 structures, limits, format/memory properties; all core procedure names and scoped extension lookup |
| Queues/memory | One ordered worker-backed graphics/transfer queue, also compute when the Metal bridge is available; submission returns before pending host events or semaphores resolve; one device-local, host-visible coherent/cached heap, mapped buffers and allocation callbacks |
| Buffers | Vertex/index/uniform/indirect buffers, copies, fills and copied update payloads; up to 256 MiB per allocation |
| Images | Transfer-only host-coherent linear 2D images and optimal 1D/1D arrays, 2D/2D arrays, 3D and six-face cubes; up to 13 mips and 256 array layers, 256-cubed volumes; normalized/integer/float/sRGB/packed color, D16/D32/D24S8 and BC1–BC7; typed storage formats, restricted views and swizzles |
| Multisampling | Backend-gated 2/4-sample attachments, color/depth per-sample fetches, image/sample-count queries, array views and subpass inputs; alpha-to-coverage, sample masks, fragment sample builtins, attachment-free rasterization, partial clears and resolves |
| Transfers | Buffer/image and image subrectangle copies; scaling/reversing color blits with nearest/linear filtering; independent packed depth/stencil transfers |
| Descriptors | Uniform/dynamic uniform buffers, combined or separate 1D/2D/3D/array/cube images/samplers, immutable samplers and color/depth/stencil input attachments; fixed descriptor arrays with constant indexing; four sets, twelve UBOs, eight SSBOs and sixteen sampled images/samplers per graphics stage; compute storage/dynamic storage buffers, storage images and uniform/storage texel buffers |
| Pipelines | Vertex/fragment stages, indexed/instanced draws with nonzero firstInstance, up to 65535 indirect draws with stride, primitive restart, culling, depth bias, rasterizer discard, blending and stencil operations |
| Render passes | Inline and inherited secondary command buffers, multiple subpasses, input/preserve attachments, first-use load clears, automatic layouts and per-subpass resolves; four color attachments and optional depth/stencil; 256 framebuffer layers, per-layer load/partial clears/resolves and view-relative base layers |
| Dynamic state | Viewport (including negative origins), scissor, unit line width, depth bias, blend constants, stencil reference/compare/write masks; 128-byte push-constant storage |
| Compute | Independent SPIR-V to Metal compiler; direct/indirect dispatch, scalar/vector arithmetic, branches/loops, scalar/vector/aggregate specialization, invocation/workgroup builtins, nonrecursive pointer/aggregate function calls, native scalar/vector and matrix math, sampled/storage image reads, filtering and depth comparisons, shared memory, workgroup barriers, memory-only fences and ordered 32-bit atomics |
| Queries | GPU occlusion counters, recorded and `VK_EXT_host_query_reset` resets, availability, 32/64-bit results and copies with stride; timestamps disabled because queue precision is zero |
| Synchronization | Fences, binary semaphore chains, host/recorded events and event waits, conservative GPU barriers, queue/device idle |
| Commands/WSI | Reset/resubmit, one-time and secondary transfer buffers; headless and zwm surfaces, FIFO swapchains, acquisition, retirement and presentation |

The independent SPIR-V frontend supports 32-bit scalar/vector arithmetic,
explicit uniform offsets, array/matrix strides, row/column-major matrices,
1D/2D/3D/array/cube implicit sampling, explicit LOD/gradients, LOD bias,
constant texel offsets from -8 to +7, integer-coordinate fetches and size/level
queries, composites, scalar specialization and common GLSL.std.450 math. It lowers
structured branches, loops and switches to native GPU control flow. Parallel
phi copies run on incoming branches; selection merges reconverge before
subsequent derivatives. Early function returns unwind nested loops, and
branches to an enclosing switch merge skip the intervening selection's stores.
Private arrays use bounded native indirect temporary accesses. Integer
division, shifts and bitfield operations guard operands whose SPIR-V results
are undefined, preserving the shader's other defined values on both graphics
and Metal compute paths.
Function calls are inlined; recursive calls are rejected. Bounded dynamic
indexing supports one dynamic index per access chain. Vertex clip depth is
converted to the backend's range. Vertex/instance builtins include nonzero
`firstInstance`; instance-rate vertex buffers are rebased without requiring a
host base-instance extension. Input attachment reads use GPU blits to snapshot
color or depth attachments before integer-coordinate texel fetches.
Subpass dependencies currently use conservative GPU completion. Single- and
multisample 2D and layered color/depth input attachments are tested.
Multisample tests cover RGBA8/BGRA8 and D16/D32/D24S8 depth, individual samples,
view swizzles and the last two layers of a 256-layer array. Integer multisample
formats remain restricted by the backend's format capabilities. With
geometry/tessellation and layer output disabled, rasterization and input reads select framebuffer
layer zero; attachment load/clear/resolve operations cover their specified layers.
Integer color inputs have typed fetch support. Stencil sampling and inputs
are tested with 1, 2 and 4 samples, every stencil byte, nonzero mip/layer views
and depth preservation. Single-sample stencil preparation transfers its byte
aspect; multisample stencil uses eight read-only GPU stencil-test passes and
a GPU integer fetch pass. Private extraction and color-blit draws are excluded from application occlusion
queries. Fragment sample-mask inputs include the pipeline mask. Outputs with
no surviving samples discard the fragment even on single-sample framebuffers. Secondary render-pass command buffers inherit the render pass,
subpass, render area and optional framebuffer; their pipeline and descriptor
state remains local to each command buffer. Broader integer color input
coverage still needs acceptance testing. Fragment sample ID, sample position and sample mask builtins compile;
full optional sample-rate shading and interpolation remain unfinished.

Unsupported shader capabilities, execution modes, image/pipeline features
and recorded operations produce checked errors. Recorded errors reach
`vkEndCommandBuffer`. `zvkGetLastError()` describes the last compiler or
selected API/profile failure on the calling thread; successful calls can
leave an earlier diagnostic in place.

Separate image/sampler descriptors are paired per static shader use, so one
image can use different sampler objects in the same shader. Immutable sampler
handle arrays are copied into the layout, initialized during set allocation
and retained during descriptor writes/copies. Keep the sampler objects alive
through the last use of the layout and its sets, as required by Vulkan.
Pipeline derivative/optimization flags are accepted as hints; pipelines compile
independently. Single-subpass compatibility ignores resolve references and
unused attachment descriptions. Ignored depth-bounds state and disabled depth
bias state do not invalidate command recording.

Compatible mutable color views now preserve canonical texel bits across typed
attachment clears, rendering, sampling and resolves. Single-sample views use
byte transfers; normalized/float multisample views preserve each sample with
GPU pack/unpack shaders. Private conversions do not contribute to application
occlusion queries. Integer multisample views remain unfinished: Apple's GL
backend reports `GL_MAX_INTEGER_SAMPLES = 1`, so those attachments need another
backend path. `EarlyFragmentTests` now works for coverage-preserving shaders:
`FragDepth` writes are ignored as required. Early tests combined with discard,
SampleMask output or alpha-to-coverage remain unfinished on this path.

Host query reset is exposed through its feature structure and extension-scoped
device dispatch. Resetting a subset preserves other queries and leaves result
bytes untouched when availability is zero. Cache-data buffers shorter than the
complete header are left untouched and report zero bytes. Linear GPU sampling
also preserves identical texels exactly, including at unnormalized edges.

Explicit LOD sampling now retains sampler mip bias in both shader compilers;
925 previously failing official sampler cases pass. The graphics compiler also
supports dynamic vector insertion, half-float quantization and aggregate
specialization expressions, and skips debug instructions between function
parameters. The six early-depth override failures in the earlier 267-case
SPIR-V follow-up now pass, as do fragment output initializers. All 105 prior render-pass device-loss
cases pass after fixing unused attachment transitions, physical framebuffer
selection and ignored attachment clears. Transient color/depth attachment
usage is also accepted. Depth-only formats ignore stencil load operations;
a lone fragment output at a nonzero color location retains its location.
Combined sampler/shader LOD bias clamps before adding the base LOD. Filtering
reports eight bits of subtexel/mipmap precision; independently measured Apple M4
raster coverage also establishes eight subpixel bits despite GL reporting four.
The eight former explicit NPOT LOD failures and nine subpass depth comparisons
now pass. Projected mipmap sampling failures remain unresolved.

Single-sample triangles with D16/D24 attachments use GPU-computed fixed-point
depth bias when depth-bias clamping is disabled, retaining FragCoord and explicit
depth-replacement semantics. All 13 official depth-bias cases pass. Multisample
and other primitive bias paths still use native rasterization. Compute modules
with workgroup execution barriers now preserve structured control flow through
divergent branches and function calls, fixing the line-continuity CTS verifier.
A 128-invocation regression checks repeated barriers and guarded shared-memory
results; modules without execution barriers retain the dispatcher.

The experimental Metal graphics compiler now emits vertex/fragment MSL from
SPIR-V, with structured loops/selections, edge phi copies, function returns,
stage attributes/varyings, component packing (including blocks and arrays),
first instance, centroid inputs, fine/coarse derivatives, implicit-LOD packed
texture sampling, sampled LOD queries and single/multisample input-attachment reads.
`check-metal-graphics` executes those shaders on the GPU and tests their upload/status/lifetime
protocol. An initial color draw packet also renders through the guest encoder,
including integer MSAA and native compatible texture views at 1/2/4 samples.
It preserves integer-view bit patterns and output-buffer guards. Typed clears,
framebuffer loading and partial/empty scissors also pass at every sample count.
The versioned buffer extension supplies stage resources and vertex attributes,
including first vertex, per-instance inputs and constant zero-stride inputs.
Packed texture and input-attachment draws run through the same transport.
An ABI-negotiated raw-byte mode also preserves compatible-view NaN payloads,
signed zero and infinities on the GPU, including clear/load and MSAA. It avoids
round-tripping those image bytes through CPU floating-point conversion.
LOD query tests cover sampler bias, min/max clamps, nearest/linear mip selection
and cube projection, including a change in direction length with no footprint
change.

This does **not** switch Vulkan draw commands to Metal. The packet currently
supports color targets, typed clear/load, scissoring, push constants, vertex
layouts, packed shader buffers and D32 depth attachments. Depth tests cover
all eight comparisons, writes, clear/load, per-sample preservation and early/late
discard at 1/2/4 samples. The host and ARM64 transport suites pass 21 shader
and 204 draw cases. Stencil, MRT, queries, the remaining
fixed-function state and Vulkan descriptor/command playback still need
integration. The standalone GPU test exercises early depth tests directly in
Metal. Sample interpolation and additional builtin/interface coverage remain
unfinished in this experimental compiler.
The public Vulkan path continues to use virgl/OpenGL graphics and retains its documented gaps.

To exercise the graphics compiler and color draw packets inside ARM64 sic:

```sh
make -C ../zgl/tests/vrend host
make HOST=0 ARCH=aarch64 all build/aarch64/metalstages build/aarch64/metaltransport
python3 scripts/test-sic.py --metal-graphics
```

This runs in the normal VM snapshot. `--metal-graphics` can also be combined
with `--sdl --sdk build/zvk-sdk-aarch64.tar` to check the shared loader and
installed SDK in the same run. The host ASan/UBSan compiler target currently
passes 6,524 deterministic malformed-word mutations.

## Verification

The current suites pass on the Mac and ARM64 sic under QEMU/virgl:

- 735 host and ARM64 sic API/render/compute checks (including host ASan/UBSan), including GPU pixels, sRGB, multiple color outputs,
  array/cube/mip sampling and transfers, descriptor arrays, multisample
  coverage/resolves, stencil/occlusion queries, threaded event waits, three
  subpasses, color/depth inputs, base-instance and multiple indirect draws,
  depth bias, rasterizer discard, mapped-memory alignment, twelve UBOs and
  storage images/texels and atomics, numeric clears, aliased mip/layer views,
  static descriptor arrays, nested function calls, divergent memory-only fences,
  the 65536-texel limit and 256-layer framebuffer clears/resolves. The format
  suite adds normalized/integer/float/packed GPU sampling and texel reads, BC
  transfers/sampling, integer vertex fetches, half-float edge values, full-size
  1D arrays/256-cubed storage, cube writes and 3D nearest/linear/self blits.
  Robustness tests cover descriptor boundaries inside larger allocations,
  negative indices, guarded stores/atomics, row-major matrix strides, partial
  UBOs, dynamic offsets and direct/indexed/instance vertex bounds.
- 20 headless WSI checks: acquisition/exhaustion, finite timeout, semaphore
  handoff, image enumeration, retirement and allocation-failure rollback.
- 40 host compiler checks plus 22,970 malformed instruction/ID mutations,
  including rejected recursion. Five CPU conversion/decoder checks exercise
  32768 random/reserved BC blocks, clipped edges and guarded output buffers.
- The Khronos loader's rendering, WSI and color/depth subpass tests report
  zero core or synchronization validation errors. The loader subpass suite
  runs 42 checks; graphics runs 85, compute runs 49 and formats/dimensions
  run 206; robustness runs 30, multisample/texture runs 320 and atlas/vertex
  pulling runs 17, including validation. These omit
  invalid-usage rejection cases.
- Both cross-built and native-clang-built window programs pass 64 animated
  frames, maximize/restore, two recreations, changing pixel samples and close.
- Cross-built and native-clang-built SDL2 programs load the driver through
  the native Khronos loader, render 64 frames, and handle resize/input.
  Three-second compositor pauses recover without losing the surface.
  Missing loader/manifest/driver failures and native shared-library
  constructors, `dlopen`/`dlsym`, and per-thread TLS are checked.
- The packaged SDK installs in a snapshot, discovers its ICD without
  environment overrides, and builds/runs its examples with `SDK=/usr`.
  Both ARM64 and x86_64 SDKs cross-build; runtime checks use ARM64.

ASan/UBSan run the host API, WSI and compiler suites. `ZVK_DEBUG_SHADERS=1`
prints generated TGSI; `VHOST_DEBUG=1` prints Mac resource/transfer details.
VM serial logs and summaries are saved under `../build/zvk-*`. These checks
establish the supported path; they are not Vulkan CTS or certification.

## Official CTS

Run the complete pinned official default selection from the repository root:

```sh
make -C zvk HOST=1 check-cts-mustpass
```

The runner freezes the ICD, renderer and loader, hashes the expanded nested
case lists, and writes resumable batch reports under `zvk/build/cts/results`.
It compresses raw logs, keeps crash/timeout evidence, and rejects missing cases.
Only CTS support checks may classify cases as `NotSupported`. A passing
diagnostic group is not a complete mustpass result.

```sh
make tools validation-tools
make cts-tools
make check-cts CTS_CASE='dEQP-VK.compute.pipeline.basic.*' CTS_NAME=compute-basic
make check-cts CTS_CASE='dEQP-VK.api.info.*' CTS_NAME=api-info
make check-cts CTS_CASE='dEQP-VK.info.*' CTS_NAME=device-info
make check-cts CTS_CASE='dEQP-VK.image.store.with_format.*' CTS_NAME=image-store-formats-all
make check-cts CTS_CASE='dEQP-VK.texture.compressed.bc*' CTS_NAME=texture-bc
make check-cts CTS_CASE='dEQP-VK.robustness.buffer_access.*' CTS_NAME=robust-buffers
make check-cts CTS_CASE='dEQP-VK.robustness.robustness1_vertex_access.*' CTS_NAME=robust-vertex-pixels
make check-cts CTS_CASE='dEQP-VK.pipeline.monolithic.multisample.*' CTS_NAME=multisample
```

`cts-tools` checksums the official `vulkan-cts-1.4.7.0` source archive and
its pinned dependencies, then builds `deqp-vk` for macOS using local CMake,
Ninja and Homebrew LLVM. Android packaging files and Vulkan video tests are
excluded. A build-only `<cmath>` include fixes compilation with modern libc++;
test logic and expectations remain unchanged. Allow several GiB of disk space
and time for the initial build. No CTS code is linked into zVk.

The runner uses only the isolated loader and zVk ICD, saves `.log`, `.qpa` and
JSON reports under `build/cts/results`, and returns failure for failed, empty,
incomplete, timed-out or wholly skipped runs. The default group is `dEQP-VK.info.*`.
Direct use supports `python3 scripts/run-cts.py --case '...' --name run --timeout 1800`.
Add `--validation` to freeze and enable the pinned Khronos validation layer;
its hash and error count are recorded, and validation errors make the report
fail even if CTS reports passing cases. Test logic and expectations stay unchanged.

On the M4, the selected basic compute group passes 51 cases, skips 30 and fails
zero. API information passes 2038 with 8452 skips and no failures, resolving
all 49 previous required-format query failures. Typed image/texel stores pass
819 cases with 819 skips; BC texture tests pass 96 with 96 sparse-related skips.
Buffer robustness passes 138 with 1074 skips; the independent vertex robustness
pixel group passes all four cases. Device information now passes
18 with five skips and no failures, including required limits and features.
The multisample group passes 78 with 10558 skips and no failures, including
the storage-buffer/compute multisample alpha-to-coverage case. Its sampled-image, input, coverage and compatibility cases run on
the GPU. Some current CTS image tests require newer SPIR-V
and skip on this Vulkan 1.0 driver. These are selected diagnostic runs, **not a
passing full Vulkan 1.0 mustpass result or a conformance claim**. See
[CONFORMANCE.md](CONFORMANCE.md) for remaining requirements.

The frozen full sweep attempted all 3,245,940 selected cases: 119,372 Pass,
3,124,555 NotSupported, 1,908 Fail and 105 DeviceLost. Its report remains
incomplete because those singleton device-loss cases aborted. A later, complete
2,013-case prior-failure follow-up records 1,332 Pass, 227 NotSupported and
454 Fail with no device loss (`mustpass-94172531934e66237e97.json`). Reports
identify different immutable input builds; their totals cannot be combined
into a passing full run. Official support checks and expectations are unchanged.

## Remaining work

- Arbitrary buffer/image aliasing and cross-stage visibility, plus further
  acceptance coverage for the graphics resource atlas.
  Graphics storage writes and dynamic descriptor indexing remain disabled
  optional features.
- Finish integer multisample mutable views and the remaining `EarlyFragmentTests`
  semantics. Complete image/API edge behavior, transfer/format reinterpretation rules,
  shader control flow and interpolation semantics, and command/object lifetimes.
  Full optional sample-rate shading, cube arrays, depth bounds, pipeline
  statistics and timestamps remain backend-dependent future features.
- Complete Vulkan 1.0 CTS coverage and resolution of its remaining failures.
  Selected passing groups and local API checks are not certification. The
  diagnostic runner preserves build hashes, records interrupted cases and
  isolates aborted batches so that their unexecuted tails are still attempted.
- Registered sic window-system integration and additional native GPU backends.
- Additional native GPU backends. PowerPC, R300 and the native Adreno path do
  not execute this Vulkan frontend yet. The virtual GPU host still uses
  virglrenderer and its host driver, as zgl does.

## License

Library, public headers, examples and tests: LGPL-2.1-or-later. Build scripts:
GPL-2.0-only. Khronos headers, loader, validation and build tools retain their
upstream licenses. The vendored bcdec texture decoder uses its MIT license.
See `LICENSE`, `NOTICE` and `vendor/bcdec/README.md`.
