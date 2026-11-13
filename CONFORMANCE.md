# Vulkan completion target

The first target is complete Vulkan 1.0 core behavior and its applicable CTS
mustpass cases. zVk is currently experimental and incomplete. Dispatching all
137 core names and passing the supported-profile suite do not establish
conformance. New Vulkan versions and optional features follow that baseline.

## Current evidence

- 735 host API checks, including ASan/UBSan, cover offscreen graphics, formats,
  compute and image dimensions; 20 WSI checks cover presentation lifetimes.
  The ARM64 run also passes all 735 checks, native SDK compilation, and SDL
  animation/resize tests (`build/zvk-depth-barriers-native-host.log`).
- Five CPU format/decoder checks exercise 32768 random/reserved BC blocks;
  compiler checks cover 40 cases and 22,970 malformed instruction/ID mutations.
- Three-subpass tests, including inherited secondary command buffers, cover color/depth input attachments, preserved
  attachments, first-use clears, final layouts and command resubmission.
- The ARM64 hardware counter supplies monotonic time independently of delayed
  timer interrupts; SDL pause tests compare guest elapsed time with host time.
- Host loader tests run those passes with Khronos core and synchronization
  validation; supported inputs render without validation errors.
- `make HOST=1 SAN=1 check` covers memory/undefined-behavior errors in the
  API, WSI and compiler tests, including malformed input-image SPIR-V.
- `make HOST=1 profile-audit` reports an initial selection of mandatory
  limits and the graphics/compute queue requirement as JSON. It returns 1
  while gaps remain and 2 if no device is available. It checks advertised
  properties, not execution of every limit. It is not a complete feature,
  format or limits audit and is not CTS.

From the repository root, save that report with:

```sh
make -C zvk HOST=1 build/host/profile
zvk/build/host/profile > build/zvk-profile-audit.json
```

The current Mac backend passes all 60 checks with the Metal bridge, including
48 descriptor-set samplers, stencil sample counts 1 and 4, and two discrete
queue-priority hint classes. A GPU regression uses sixteen image descriptors
in each of the three stages under one 48-sampler layout. Per-stage
resource capacity now has a 52-entry logical table; 40 simultaneous compute
resources execute through two physical GPU bindings, with aliasing and guarded
writes checked. Aggregate UBO/sample-image limits now account for all three
supported stages (36 and 48). These numbers do not imply that every required
shader/resource-class combination has been implemented.
Full 4096-wide/256-layer 1D storage images and 256-cubed 3D
storage images now execute and verify every texel/voxel in host and sic tests.
Descriptor requirements account for the supported shader stages; the per-stage
resource requirement uses the specified minimum of 128 and the sum of individual
resource limits. Compute execution is independently checked on the M4, but
passing these numerical limits does not establish full compute shader/API
support. Raising a reported limit without implementing and testing its behavior
is not a fix.

### Official diagnostic runs

Official `vulkan-cts-1.4.7.0` now builds and runs through the isolated Mac loader
and zVk ICD. `make cts-tools` builds it; `make check-cts CTS_CASE='...' CTS_NAME=...`
selects cases and writes logs, QPA results and JSON summaries to
`build/cts/results`. Reports freeze and hash the driver, renderer and loader and reject incomplete
or entirely skipped diagnostic runs. `make check-cts-mustpass` expands the pinned
official default list (3,245,940 cases) into resumable batches, preserves failures,
and isolates aborted batches to avoid dropping their unexecuted tails.

Recorded on the Apple M4, 2026-11-02:

| Group | Pass | Fail | NotSupported |
| --- | ---: | ---: | ---: |
| `dEQP-VK.compute.pipeline.basic.*` | 51 | 0 | 30 |
| `dEQP-VK.api.info.*` | 2038 | 0 | 8452 |
| `dEQP-VK.image.store.with_format.*` | 819 | 0 | 819 |
| `dEQP-VK.texture.compressed.bc*` | 96 | 0 | 96 |
| `dEQP-VK.robustness.buffer_access.*` | 138 | 0 | 1074 |
| `dEQP-VK.robustness.robustness1_vertex_access.*` | 4 | 0 | 0 |
| `dEQP-VK.pipeline.monolithic.multisample.*` | 78 | 0 | 10558 |
| `dEQP-VK.info.*` | 18 | 0 | 5 |

Further host runs during the core-completion work:

| Group | Pass | Fail | NotSupported |
| --- | ---: | ---: | ---: |
| `graphicsfuzz.*` | 677 | 0 | 80 |
| `pipeline.monolithic.max_varyings.*` | 1 | 0 | 5 |
| `glsl.operator.*` | 3630 | 0 | 0 |
| `glsl.conversions.*` | 1590 | 0 | 0 |
| `glsl.matrix.*` | 1764 | 0 | 0 |
| `glsl.builtin.function.*` | 193 | 0 | 692 |
| `glsl.builtin.precision.*` | 504 | 0 | 160 |
| `glsl.derivate.*` | 1224 | 0 | 450 |
| `glsl.atomic_operations.*` | 32 | 0 | 1008 |
| `glsl.linkage.*` | 69 | 0 | 0 |
| `glsl.440.linkage.*` | 68 | 0 | 0 |
| `glsl.struct.*` | 80 | 0 | 0 |
| `spirv_assembly.instruction.compute.*` | 941 | 0 | 18965 |
| `texture.shadow.*` | 721 | 0 | 4656 |
| `api.object_management.*` | 364 | 0 | 93 |
| `api.command_buffers.*` | 49 | 0 | 82 |
| `api.buffer_view.*` | 182 | 0 | 822 |

The structured frontend passes all 757 graphicsfuzz cases: 677 Pass and 80
NotSupported for optional extensions (`core-fuzz-all-selection.json`). Parallel
phi copies run on incoming edges, nested returns unwind, and exits to an
outer switch skip intervening stores. The compiler also handles bounded
private arrays and guards undefined arithmetic operands. The original 32
shader regressions all pass.

Further targeted reports record 28 former uniform-buffer/dynamic-offset and
memory-requirement failures, nine sample-mask/query failures, and twelve sRGB
mipmap-blit failures passing (`core-uniform-atlas-regressions.json`,
`core-query-sample-mask.json`, `core-srgb-mips-images.json`). Uniform memory
requirements now match the advertised alignment, and all read-only graphics
buffers use the integer texture atlas. The backend retains interpolation and
invariance metadata for all 64 scalar varying components. Single-sample shader
mask outputs discard fragments with no surviving samples; application query
counts exclude private color blits.

These reports identify the exact input hashes; they come from successive
fixes and do not constitute a complete passing run of one final build. The ARM64 SDK, native compiler, patched Khronos loader and SDL path also pass:
704 API checks, 20 WSI checks and 64 animated frames through two resizes. Full
mustpass runs are being repeated as the remaining failures are resolved. The first full diagnostic sweep found
27,131 failed cases and 372,580 unexecuted cases following aborted batches;
those unexecuted cases are not recorded as passes.

The basic compute failures found in the first run were fixed: memory-only
fences, storage-buffer descriptor arrays, scalar/vector value function calls,
and the tested min/max/clamp operations now execute on the GPU. Local tests
also cover nested calls/private side effects and divergent fences inside sic.
Memory heap flags, storage image sample-count reporting and the queue priority
count were corrected; BGRA8 uniform texel reads gained real GPU support.

All 49 previous required-format query failures are resolved. Typed storage
cases cover 1D/1D arrays, 2D/2D arrays, 3D, cubes and texel buffers across all
39 SPIR-V storage formats. BC tests cover signed, unsigned, sRGB and HDR
sampling. Transfer-only linear tiling is supported; sparse resources and cube arrays are not advertised;
these account for many skips. Other current CTS image tests request newer
SPIR-V and skip on Vulkan 1.0. Passing format queries alone does not prove
all format semantics.

Mandatory device feature validation now passes, including `robustBufferAccess`.
Local GPU tests cover vertex fetches, negative/partial descriptor accesses,
dynamic UBOs, guarded SSBO/texel stores and atomics, and non-square row-major
matrix copies. Official legacy buffer robustness passes all 138 supported
cases; graphics storage stages and optional feature cases skip. Its separate
legacy vertex-access group is wholly skipped because it captures results with
unsupported graphics-stage storage writes; this is not recorded as a passing
run. The independent `robustness1_vertex_access` pixel group passes all four
cases, covering zero strides, partial attributes, offsets and indexed draws.

The recorded device-limit failures are resolved. Stencil reads cover all 256
values at 1, 2 and 4 samples, sampled images and input attachments, restricted
mip/layer views, depth preservation and exclusion of private extraction draws
from occlusion queries. Single-sample stencil preparation transfers its byte
aspect; multisample stencil evaluation and conversion execute on the GPU.

`sample_rate_a2c.static_a2c` now passes after implementing bounded read-only
graphics storage buffers and compute multisample texel fetches. All 78 supported
cases in that group pass; unsupported optional features, formats and sample
counts still skip. The independent core/synchronization validation suite uses
valid core sample masks to generate its per-sample stencil patterns. Optional
sample-rate shading remains disabled. No complete Vulkan 1.0 mustpass run or
certification is claimed.

Additional recent diagnostic reports:

- `core-glsl-texture-all-atlas.json`: all 2487 applicable texture-function cases pass (5460 optional cases skip).
- `core-atlas-border-final.json`: all 1116 border regressions pass; `core-packed-border-atlas.json` also passes the ten packed A1R5G5B5 border cases.
- `core-cache-control-real.json`: 16 cache-control cases pass, two optional cases skip.
- `core-image-qualifiers.json`: 42 coherent/volatile image cases pass, 14 skip.
- `core-cubemap-view.json`: cubemap-face rendering/sampling passes after applying implicit layout transitions to the entire attachment view.

These reports have separate frozen build hashes and are diagnostics, not a
single complete mustpass result.

The next mutable-view diagnostic (`mustpass-cb1a69b17ca4115d446e.json`)
executes all 10,646 selected cases: 1,224 Pass, 9,214 NotSupported and 208 Fail.
Every failure rejects an integer multisample attachment. A native GL probe
confirms `GL_MAX_INTEGER_SAMPLES = 1` and allocation failure for two/four-sample
R32UI and RGBA32UI; float/normalized multisample allocation succeeds. This is an
implementation gap, not a conformance pass or a reclassified skip. A later local
regression caught and fixed fragment-constant clobbering by private conversions;
all 704 local checks pass with ASan/UBSan and on ARM64 after that fix.

The host-query-reset group (`core-host-query-reset-all.json`) has 384 Pass,
55 NotSupported and two Fail. Validation identifies static `lineWidth = 0`
without `wideLines` in those two attachment-free cases; they remain recorded
as failures. Feature auditing also records tests that use disabled graphics
storage writes or multiview. No official support checks, test selection or
expected results have been patched. These reports use different frozen hashes.
The independent exact-sampling group (`core-exact-sampling-stable.json`) has
222 Pass, 486 NotSupported and zero Fail after preserving identical linear taps.

Fresh follow-up reports preserve the original frozen sweep failures while
checking these fixes against unmodified official tests:

- `core-all-sampler-bias-fixed.json`: all 925 previously failing sampler cases
  pass after retaining sampler mip bias with explicit LOD in the graphics and
  Metal compiler paths. This CTS group exercises graphics sampling.
- `core-spirv-graphics-specialization-fixed.json`: 261 Pass and six Fail from
  the 267-case prior-failure selection. Dynamic vector insertion, half-float
  quantization (including specialization), debug instructions between function
  parameters, aggregate specialization expressions and unused runtime-array
  undef values are now handled. Those six early-depth override cases pass in `core-early-depth-override-fixed.json`.
- `core-renderpass-depth-only-fixed.json`: all 105 previous render-pass
  DeviceLost cases pass. Unused attachments still transition to final layout,
  unused color slots no longer make the physical framebuffer invalid, and
  attachment clears ignore unused references and absent depth/stencil aspects.
  Transient color/depth attachment usage is accepted; lazy-memory cases may
  still report NotSupported through unchanged CTS support checks.
- `build/zvk-renderpass-instructions-api-asan.log` records 711 Pass and zero
  Fail under ASan/UBSan. `build/zvk-renderpass-instructions-validation.log`
  records zero core/synchronization validation errors in the local GPU groups.

The resumable runner also retains aborted parent reports and completed child
selections after interruption, rather than repeating the original timed-out
parent. Its regression checks retain the original failures and execute missing
tails; archiving preserves earlier attempts.

An experimental independent Metal graphics compiler and color draw transport
are now tested separately (`make HOST=1 check-metal-graphics`). Hardware tests
cover integer MS4 compatible views, early/late depth-discard behavior, stage
attributes/varyings and first instance, component-packed blocks and arrays,
centroid interfaces, structured loops, outer-selection reconvergence, fine/coarse
derivatives, packed texture sampling and single/multisample input attachments.
Shader upload/status/destruction and color draws also pass through the actual
guest encoder. The draw packet covers four
base/view pairs at 1/2/4 samples, including exact NaN-representation bits through
an integer view and guards around the destination buffer.

These are backend tests, **not** completed Vulkan draw integration. The packet
supports typed clear/load and scissors, stage buffer bindings and vertex
layouts (including per-instance and zero-stride inputs). The 21 shader transport
cases and 204 draw cases pass on the host. Raw-byte transfers preserve NaN
payloads, signed zero and infinities through compatible native views at every
sample count, including clear/load operations. D32 depth attachments cover all
eight comparisons, writes, clear/load, per-sample preservation and early/late
discard at 1/2/4 samples. Stencil, MRT, query handling,
remaining fixed-function state and Vulkan descriptor/command playback are
still unfinished. The ARM64 snapshot also passes shader generation,
21 shader upload/status/lifetime cases and 204 draw cases through virtio
(`build/zvk-metal-depth-native-host.log`). The earlier full runtime run
(`build/zvk-query-dispatch-native-host.log`) covers 706 API checks, 20 WSI checks,
native compilation, loader/TLS and SDL/installed-SDK tests.
The graphics compiler passes 11,144 malformed-word mutations under ASan/UBSan.
LOD queries also cover sampler bias/clamps, mip rounding and cube-projected
derivatives. Sample interpolation and additional builtin/interface coverage
remain unfinished in the experimental compiler.
The public Vulkan graphics path still uses GL, and the 208 integer-multisample
failures remain unresolved.

A four-case follow-up (`core-four-shader-feature-audit.json`) retains failures
for the divide-by-zero vertex crash test, both logical-copy Amber tests and the
graphics push-constant overwrite test. Each writes graphics storage resources
while its required feature is disabled; validation records
`VUID-RuntimeSpirv-NonWritable-06340`/`06341`. These failures are not relabeled as
passes and their official support checks are unchanged.

The full frozen sweep (`mustpass-25251c8b0b9afaea1bbd.json`) attempted all
3,245,940 selected cases: 119,372 Pass, 3,124,555 NotSupported, 1,908 Fail and
105 DeviceLost. All selected leaves were attempted, but the report remains
`complete: false` because 105 singleton cases aborted. This is not a passing
conformance report. It found a dispatch generator
bug: six wrapped C declarations were misclassified because their first parameter
began on a new line. Dispatch scope inference now ignores leading whitespace,
and an independent comparison with the pinned Vulkan registry finds no scope
mismatches. The official `api.version_check.entry_points` case passes after
the fix (`core-entry-points-fixed.json`). The original frozen failure remains
in the sweep; its immutable binaries are retained.

The complete 2,013-case failure follow-up before the latest fixes recorded
1,292 Pass, 227 NotSupported and 494 Fail (`mustpass-b79a008a137ce045d73a.json`).
The current follow-up (`mustpass-94172531934e66237e97.json`) records 1,332 Pass,
227 NotSupported and 454 Fail, with exact coverage and no device loss.
These diagnostic runs freeze different builds; they do not replace the original
full-sweep result or establish conformance. Remaining failures include 208
integer multisample mutable-view cases, 42 projected mipmap cases and two
texture-controlled discard cases. Other failures include cases using graphics
storage writes or optional extensions without enabling their required features;
validation audits retain that distinction without changing CTS support checks.

Additional fixed groups and local proofs:

- `core-absent-stencil-load-fixed.json`: 11 attachment-result cases pass after
  preserving D32 LOAD data when the absent stencil aspect requests CLEAR,
  and binding a single fragment output at its declared nonzero location.
- `core-explicit-npot-precision-fixed.json`: eight explicit NPOT LOD cases pass;
  subtexel/mipmap precision is eight bits. Native M4 coverage measurements
  establish eight raster subpixel bits (`make -C zgl/tests/vrend check-raster-precision`),
  fixing nine subpass depth comparisons. Projected sampling remains unresolved.
- `core-fragment-output-initializers-fixed.json`: six Pass and 48 NotSupported;
  color/depth output initializers survive and discarded invocations preserve depth.
- `core-early-depth-override-fixed.json`: all six cases pass. Early tests ignore
  FragDepth writes; discard, SampleMask output and alpha-to-coverage combinations
  remain unsupported in the public GL path.
- `core-fixed-point-depth-bias.json`: all 13 cases pass. Single-sample D16/D24
  triangles use a GPU fixed-unit bias when bias clamping is disabled; native
  floating-point-backed GL D24 offsets otherwise vary with depth. Regressions
  cover static/dynamic negative bias, slope, FragCoord and depth replacement.
- `core-structured-barrier-line.json`: line continuity passes after preserving
  structured compute reconvergence at workgroup execution barriers. The new
  128-invocation repeated-barrier regression fails against the previous driver
  and passes after the fix. `core-structured-compute-regression.json` executes
  67,553 official compute cases: 86 Pass, 67,467 NotSupported, zero Fail.
- `core-combined-sampler-bias-regression.json`: all 925 previous sampler failures
  still pass after clamping the combined shader/sampler bias before adding LOD.
- `build/zvk-depth-barriers-api-asan.log`: 735 API Pass, 20 WSI Pass,
  40 compiler Pass and 22,970 malformed mutations. The corresponding
  `build/zvk-depth-barriers-validation.log` has zero core/synchronization errors.

## Work still required

| Area | Remaining implementation and acceptance evidence |
| --- | --- |
| Compute | GPU execution covers direct/indirect dispatch, storage images/texels, static descriptor arrays, value function calls, workgroup memory/barriers, memory-only fences and relaxed atomics. Multisample 2D/array fetches and size/sample queries now work. Ordinary/filtered/comparison sampling, pointer/aggregate arguments, matrix algebra, extended integer results, specialization expressions and ordered atomics now have CTS coverage. Finish remaining shader/memory-model behavior and the full applicable CTS selection. Unpatched hosts keep compute disabled. |
| Storage | Compute storage/dynamic storage buffers, all 39 typed storage image/texel formats, 1D/3D/cube access, aliases, transfers and relaxed atomics have GPU tests. Graphics reads and runtime-array lengths now use a descriptor-bounded GPU texture atlas. A mixed-resource regression covers 12 UBOs, 8 large SSBOs, 16 images and independent samplers, 4 storage-image reads and 8 input attachments together. Graphics writes remain disabled; arbitrary image/buffer aliasing and cross-stage visibility remain unfinished. |
| Images/formats | Integer multisample mutable views remain unfinished on the GL backend; compatible single-sample views and normalized/float multisample views now have GPU tests. 1D/3D sampling/storage/transfers/blits and BC1–BC7 textures now have GPU tests. Color/depth multisample fetches and input attachments, restricted array views, swizzles, alpha-to-coverage and attachment-free sample counts now have GPU tests. Stencil sampling/input tests now cover all byte values and 1/2/4 samples. Full sample shading/interpolation and complete attachment/format edge behavior remain unfinished. Layered framebuffer load/partial clears/resolves now cover 256 layers and nonzero view bases; normal rasterization/input reads use layer zero with layer-output features disabled. |
| Descriptors | Separate and immutable graphics samplers have GPU/copy tests; all 48 aggregate sampler descriptors now have execution tests. High binding numbers, zero-count bindings and the advertised simultaneous resource counts now have execution tests. Complete pipeline-layout compatibility and supported indexing behavior still need acceptance coverage. |
| SPIR-V | Coverage-preserving `EarlyFragmentTests` now works; early tests with discard or coverage changes remain unfinished. Complete remaining required Shader capability operations and execution modes, derivatives, image operations, interface/interpolation rules, specialization, structured control flow and remaining bounds/interface rules. Test with independent SPIRV-Tools validation and CTS shaders. |
| Render passes | Secondary render-pass command buffers and inheritance are implemented and tested. Attachment aliasing and remaining compatibility rules still need completion. Multiple inline subpasses and color/depth/stencil inputs now have execution tests. |
| Pipeline/commands | Complete required state behavior, ignored-state handling, pipeline derivatives and remaining query/transfer rules. Pipeline cache control now reuses exact-key compiler results and retained native shader objects; cache merging preserves independent object lifetimes. Core derivative/optimization hints and the tested render-pass compatibility rules now work. Base-instance and multiple indirect draws, unit line width, depth bias and rasterizer discard now have pixel tests. |
| Memory/synchronization | Robust buffer access has descriptor and vertex tests. Remaining work includes required limits, binding/map/aliasing semantics, host/device synchronization, command/object lifetimes, aliasing and device-loss recovery. Pending semaphore/event waits now execute on an ordered queue worker, and host transport contexts are isolated and serialized across concurrent devices. |
| Conformance | Official CTS is built and the pinned default mustpass list has been selected. More diagnostic groups now pass; complete the official selection with exact coverage and resolve its remaining failures. Diagnostic groups and local regression tests do not establish conformance. |

Additional native GPU backends and a registered sic window-system extension
are platform goals, rather than prerequisites for Vulkan 1.0 core conformance.
Optional features may remain disabled; features reported as supported must
work when enabled.

## Source of requirements

- [Khronos required limits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-required): use the Vulkan Core baseline, without later-version or Roadmap increases.
- [Khronos devices and queues](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-queueprops): an implementation exposing graphics must provide a graphics-and-compute queue on at least one physical device.
- [Official Vulkan CTS instructions](https://github.com/KhronosGroup/VK-GL-CTS/blob/main/external/vulkancts/README.md): CTS is the acceptance suite beyond the local regression and loader/validation tests.
