# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# Independent Vulkan frontend; shared GPU encoder only, no libzgl or Mesa compiler.
HOST ?= 0
ARCH ?= aarch64
SYSROOT ?= $(abspath ../$(if $(filter x86_64,$(ARCH)),sysroot,sysroot-$(ARCH)))
ZGL := ../zgl
LLVM := $(shell brew --prefix llvm 2>/dev/null)
LLD := $(shell brew --prefix lld@21 2>/dev/null)
CC := $(if $(LLVM),$(LLVM)/bin/clang,clang)
AR := $(if $(LLVM),$(LLVM)/bin/llvm-ar,ar)
LD := $(if $(LLD),$(LLD)/bin/ld.lld,ld.lld)
DEPS := build/deps
VKH := $(DEPS)/vulkan-headers/include
SPVH := $(DEPS)/spirv-headers/include
INC := -Ibuild -Iinclude -Isrc -I$(VKH) -I$(SPVH) -I$(ZGL)/include -I../zwm/include
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror=implicit-function-declaration -D_GNU_SOURCE $(INC)
SOURCES := $(wildcard src/*.c)
TEST_HEADERS := $(wildcard tests/*.h)
ifeq ($(HOST),1)
B := build/host$(if $(SAN),-asan)
VIRGL := $(abspath $(ZGL)/tests/vrend/build/virglrenderer-host)
CFLAGS += -DZVK_HOST -fPIC -idirafter ../sic/include
HOSTLIBS := -L$(VIRGL)/lib -lvirglrenderer -Wl,-rpath,$(VIRGL)/lib -framework OpenGL -lpthread -lm
BACKEND := $(B)/virgl.o $(B)/vhost.o
LIBS := $(BACKEND) $(HOSTLIBS)
ifneq ($(SAN),)
CFLAGS += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
HOSTLIBS += -fsanitize=address,undefined
endif
else
B := build/$(ARCH)
ifeq ($(ARCH),aarch64)
TARGET := aarch64-linux-musl
EMUL := -m aarch64elf
else ifeq ($(ARCH),x86_64)
TARGET := x86_64-linux-musl
EMUL := -m elf_x86_64
else
$(error zvk currently supports aarch64 and x86_64)
endif
CFLAGS += --target=$(TARGET) -nostdinc -isystem $(SYSROOT)/usr/include -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables
BACKEND := $(ZGL)/build/$(ARCH)/libvirgl.a
ZWM_LIB := ../zwm/build/$(ARCH)/libzwm.a
LIBS := $(BACKEND) $(ZWM_LIB) $(SYSROOT)/usr/lib/libc.a $(wildcard $(SYSROOT)/usr/lib/libcompiler_rt.a)
endif
OBJECTS := $(patsubst src/%.c,$(B)/%.o,$(SOURCES))
.DEFAULT_GOAL := all
.PHONY: all deps check check-compiler check-sic check-sic-native shaders clean install tools shared
all: $(B)/libzvk.a $(B)/zvktest $(B)/zvktriangle $(B)/wsitest $(if $(filter 0,$(HOST)),$(B)/zvkwindow)

tools:
	python3 scripts/tools.py

.PHONY: cts-tools check-cts check-cts-mustpass
CTS_CASE ?= dEQP-VK.info.*
CTS_NAME ?= cts
cts-tools:
	python3 scripts/cts-tools.py
check-cts:
	$(MAKE) HOST=1 SAN= shared build/host/zvk_icd.json
	python3 scripts/run-cts.py --case '$(CTS_CASE)' --name '$(CTS_NAME)'
check-cts-mustpass:
	$(MAKE) HOST=1 SAN= shared build/host/zvk_icd.json
	python3 scripts/run-mustpass.py

shaders: build/shaders.h
build/shaders.h: $(wildcard tests/shaders/*) scripts/shaders.py
	python3 scripts/shaders.py

deps: $(DEPS)/.ready
$(DEPS)/.ready: scripts/deps.py
	python3 scripts/deps.py

build/spv_min_words.h: scripts/grammar.py $(DEPS)/.ready
	python3 scripts/grammar.py

$(B)/%.o: src/%.c src/zvk_internal.h include/zvk.h build/spv_min_words.h $(DEPS)/.ready
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
src/procs.inc: scripts/dispatch.py $(SOURCES)
	python3 scripts/dispatch.py
build/sampling_msl.h: src/sampling.msl scripts/embed.py
	python3 scripts/embed.py $< $@ zvk_sampling_msl

$(B)/compute.o: src/metal_flow.h src/metal_io.h build/sampling_msl.h src/texel_msl.h src/compute_math.h src/compute_values.h src/compute_memory.h src/compute_algebra.h src/compute_constants.h src/compute_sampling.h src/spirv_float.h
$(B)/execute.o: src/compute_images.h src/graphics_buffers.h src/mutable_views.h
$(B)/spirv.o: src/spirv_atlas.h src/spirv_flow.h src/graphics_constants.h src/spirv_float.h
$(B)/pipeline.o: src/pipeline_cache.h
$(B)/compression.o: vendor/bcdec/bcdec.h
$(B)/dispatch.o: src/procs.inc
$(B)/libzvk.a: $(OBJECTS)
	$(AR) rcs $@ $^

ifeq ($(HOST),1)
$(VIRGL)/include/virgl/virglrenderer.h:
	$(MAKE) -C $(ZGL)/tests/vrend host
$(B)/virgl.o: $(ZGL)/lib/virgl.c $(wildcard $(ZGL)/include/*.h $(ZGL)/lib/r300/*.h $(ZGL)/tests/vhost/*.h) $(DEPS)/.ready
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DZGL_VHOST -I$(ZGL)/lib -I$(ZGL)/tests/vhost -c $< -o $@
$(B)/vhost.o: $(ZGL)/tests/vhost/vhost.c $(wildcard $(ZGL)/include/*.h $(ZGL)/tests/vhost/*.h) $(VIRGL)/include/virgl/virglrenderer.h $(DEPS)/.ready
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -I$(ZGL)/lib -I$(VIRGL)/include -c $< -o $@
$(B)/zvktest: tests/zvktest.c $(TEST_HEADERS) build/shaders.h $(B)/libzvk.a $(BACKEND) $(ZWM_LIB)
	$(CC) $(CFLAGS) $< $(B)/libzvk.a $(LIBS) -o $@
shared: $(B)/libzvk.dylib
$(B)/zvktriangle: examples/triangle.c build/shaders.h $(B)/libzvk.a $(BACKEND) $(ZWM_LIB)
	$(CC) $(CFLAGS) $< $(B)/libzvk.a $(LIBS) -o $@
$(B)/libzvk.dylib: $(OBJECTS) $(BACKEND)
	$(CC) -dynamiclib -o $@ $^ $(HOSTLIBS)
check: all check-compiler check-wsi check-formats
	$(B)/zvktest
.PHONY: profile-audit
$(B)/profile: tests/profile.c $(B)/libzvk.a $(BACKEND)
	$(CC) $(CFLAGS) $< $(B)/libzvk.a $(LIBS) -o $@
profile-audit: $(B)/profile
	$(B)/profile
else
$(B)/wsitest: tests/wsitest.c $(B)/libzvk.a $(BACKEND) $(ZWM_LIB)
	$(CC) $(CFLAGS) -c $< -o $(B)/wsitest.o
	$(LD) $(EMUL) -static -nostdlib --image-base=0x8000000000 -z max-page-size=0x1000 -z noexecstack --defsym=_DYNAMIC=0x8000000000 -o $@ $(SYSROOT)/usr/lib/crt1.o $(SYSROOT)/usr/lib/crti.o $(B)/wsitest.o $(B)/libzvk.a $(LIBS) $(SYSROOT)/usr/lib/crtn.o
	@printf '\123' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none
$(ZWM_LIB): ../zwm/Makefile $(wildcard ../zwm/lib/*.[ch]) ../zwm/include/zwm.h
	$(MAKE) -C ../zwm ARCH=$(ARCH) SYSROOT=$(SYSROOT) build/$(ARCH)/libzwm.a
$(BACKEND): $(ZGL)/Makefile $(wildcard $(ZGL)/lib/*.c $(ZGL)/include/*.h $(ZGL)/lib/r300/*.[ch] $(ZGL)/lib/adreno/*.[ch])
	$(MAKE) -C $(ZGL) ARCH=$(ARCH) SYSROOT=$(SYSROOT) build/$(ARCH)/libvirgl.a
$(B)/metalstages $(B)/metaltransport: $(B)/%: tests/%.c build/shaders.h $(B)/libzvk.a $(BACKEND) $(ZWM_LIB)
	$(CC) $(CFLAGS) -c $< -o $(B)/$*.o
	$(LD) $(EMUL) -static -nostdlib --image-base=0x8000000000 -z max-page-size=0x1000 -z noexecstack --defsym=_DYNAMIC=0x8000000000 -o $@ $(SYSROOT)/usr/lib/crt1.o $(SYSROOT)/usr/lib/crti.o $(B)/$*.o $(B)/libzvk.a $(LIBS) $(SYSROOT)/usr/lib/crtn.o
	@printf '\123' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none
$(B)/zvktest: tests/zvktest.c $(TEST_HEADERS) build/shaders.h $(B)/libzvk.a $(BACKEND) $(ZWM_LIB)
	$(CC) $(CFLAGS) -c $< -o $(B)/zvktest.o
	$(LD) $(EMUL) -static -nostdlib --image-base=0x8000000000 -z max-page-size=0x1000 -z noexecstack --defsym=_DYNAMIC=0x8000000000 -o $@ $(SYSROOT)/usr/lib/crt1.o $(SYSROOT)/usr/lib/crti.o $(B)/zvktest.o $(B)/libzvk.a $(LIBS) $(SYSROOT)/usr/lib/crtn.o
	@printf '\123' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none
$(B)/zvktriangle: examples/triangle.c build/shaders.h $(B)/libzvk.a $(BACKEND) $(ZWM_LIB)
	$(CC) $(CFLAGS) -c $< -o $(B)/zvktriangle.o
	$(LD) $(EMUL) -static -nostdlib --image-base=0x8000000000 -z max-page-size=0x1000 -z noexecstack --defsym=_DYNAMIC=0x8000000000 -o $@ $(SYSROOT)/usr/lib/crt1.o $(SYSROOT)/usr/lib/crti.o $(B)/zvktriangle.o $(B)/libzvk.a $(LIBS) $(SYSROOT)/usr/lib/crtn.o
	@printf '\123' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none
check:
	@echo 'Run zvktest on sic with a virgl GPU; use HOST=1 check for Mac testing.'
endif
ifeq ($(HOST),0)
install: all
	mkdir -p $(SYSROOT)/usr/include/vk_video $(SYSROOT)/usr/include/vulkan $(SYSROOT)/usr/lib $(SYSROOT)/rootfs/bin
	cp $(VKH)/vulkan/*.h $(SYSROOT)/usr/include/vulkan/
	cp $(VKH)/vk_video/*.h $(SYSROOT)/usr/include/vk_video/
	cp ../zwm/include/zwm.h include/zvk.h include/zvk_zwm.h $(SYSROOT)/usr/include/
	cp $(B)/libzvk.a $(BACKEND) $(ZWM_LIB) $(SYSROOT)/usr/lib/
	cp $(B)/zvktest $(B)/zvktriangle $(B)/wsitest $(B)/zvkwindow $(SYSROOT)/rootfs/bin/
else
install:
	@echo "Use HOST=0 install for the sic sysroot; HOST=1 shared builds the Mac dylib."
	@exit 1
endif
clean:
	rm -rf build/host build/host-asan build/aarch64 build/x86_64

ifeq ($(HOST),1)
$(B)/spirvtest: tests/spirvtest.c build/shaders.h $(B)/libzvk.a $(BACKEND) $(ZWM_LIB)
	$(CC) $(CFLAGS) $< $(B)/libzvk.a $(LIBS) -o $@
check-compiler: $(B)/spirvtest
	$(B)/spirvtest
$(B)/metalstages: tests/metalstages.c build/shaders.h $(B)/libzvk.a $(BACKEND) $(ZWM_LIB)
	$(CC) $(CFLAGS) $< $(B)/libzvk.a $(LIBS) -o $@
$(B)/metalstages-gpu: tests/metalstages.m
	xcrun clang -fobjc-arc -Wall -Wextra -Wno-deprecated-declarations $< -framework Metal -framework Foundation -o $@
$(B)/metaltransport: tests/metaltransport.c $(BACKEND)
	$(CC) $(CFLAGS) $< $(LIBS) -o $@
.PHONY: check-metal-graphics
check-metal-graphics: $(B)/metalstages $(B)/metalstages-gpu $(B)/metaltransport
	mkdir -p $(B)/metal-stages
	$(B)/metalstages $(B)/metal-stages
	$(B)/metalstages-gpu $(B)/metal-stages
	$(B)/metaltransport $(B)/metal-stages
.PHONY: check-metal-compiler
check-metal-compiler: $(B)/metalstages
	mkdir -p $(B)/metal-stages
	$(B)/metalstages $(B)/metal-stages --fuzz
$(B)/formattest: tests/formattest.c $(B)/libzvk.a $(BACKEND)
	$(CC) $(CFLAGS) $< $(B)/libzvk.a $(LIBS) -o $@
.PHONY: check-formats
check-formats: $(B)/formattest
	$(B)/formattest
endif

# Run the native ARM64 tests without modifying the persistent disk.
check-sic:
	$(MAKE) HOST=0 ARCH=aarch64 all
	python3 -u scripts/test-sic.py

# Requires the sic-native Clang toolchain on the persistent root disk.
check-sic-native:
	$(MAKE) HOST=0 ARCH=aarch64 all
	python3 -u scripts/test-sic.py --native-build --log ../build/zvk-sic-native-serial.log

ifeq ($(HOST),1)
$(B)/wsitest: tests/wsitest.c $(B)/libzvk.a $(BACKEND)
	$(CC) $(CFLAGS) $< $(B)/libzvk.a $(LIBS) -o $@
check-wsi: $(B)/wsitest
	$(B)/wsitest
endif

ifneq ($(HOST),1)
$(B)/zvkwindow: examples/window.c build/shaders.h $(B)/libzvk.a $(BACKEND) $(ZWM_LIB)
	$(CC) $(CFLAGS) -c $< -o $(B)/zvkwindow.o
	$(LD) $(EMUL) -static -nostdlib --image-base=0x8000000000 -z max-page-size=0x1000 -z noexecstack --defsym=_DYNAMIC=0x8000000000 -o $@ $(SYSROOT)/usr/lib/crt1.o $(SYSROOT)/usr/lib/crti.o $(B)/zvkwindow.o $(B)/libzvk.a $(LIBS) $(SYSROOT)/usr/lib/crtn.o
	@printf '\123' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none
window: $(B)/zvkwindow
endif

.PHONY: validation-tools check-loader check-validation check-window check-window-native
validation-tools: deps tools
	python3 scripts/validation-tools.py
ifeq ($(HOST),1)
SDK := $(abspath build/tools/vulkan-sdk)
$(B)/zvk_icd.json: $(B)/libzvk.dylib scripts/icd.py
	python3 scripts/icd.py $(B)/libzvk.dylib $@
$(B)/loadertriangle: examples/triangle.c build/shaders.h $(SDK)/lib/libvulkan.dylib
	$(CC) $(CFLAGS) -DZVK_LOADER $< -L$(SDK)/lib -lvulkan -Wl,-rpath,$(SDK)/lib -o $@
$(B)/loaderwsi: tests/loaderwsi.c $(SDK)/lib/libvulkan.dylib
	$(CC) $(CFLAGS) $< -L$(SDK)/lib -lvulkan -Wl,-rpath,$(SDK)/lib -o $@
$(B)/loadersubpasses: tests/zvktest.c $(TEST_HEADERS) build/shaders.h $(SDK)/lib/libvulkan.dylib
	$(CC) $(CFLAGS) -DZVK_LOADER $< -L$(SDK)/lib -lvulkan -Wl,-rpath,$(SDK)/lib -o $@
$(SDK)/lib/libvulkan.dylib:
	@echo 'Run make validation-tools first to build the isolated Khronos loader and layers.'
	@exit 1
check-loader: $(B)/loadertriangle $(B)/loaderwsi $(B)/zvk_icd.json
	VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) $(B)/loadertriangle build/loader-triangle.ppm
	VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) $(B)/loaderwsi
check-validation: $(B)/loadertriangle $(B)/loaderwsi $(B)/loadersubpasses $(B)/zvk_icd.json
	ZVK_VALIDATE=1 VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) VK_LAYER_PATH=$(SDK)/share/vulkan/explicit_layer.d $(B)/loadertriangle build/validated-triangle.ppm
	ZVK_VALIDATE=1 VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) VK_LAYER_PATH=$(SDK)/share/vulkan/explicit_layer.d $(B)/loaderwsi
	ZVK_VALIDATE=1 VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) VK_LAYER_PATH=$(SDK)/share/vulkan/explicit_layer.d $(B)/loadersubpasses --graphics
	ZVK_VALIDATE=1 VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) VK_LAYER_PATH=$(SDK)/share/vulkan/explicit_layer.d $(B)/loadersubpasses --subpasses
	ZVK_VALIDATE=1 VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) VK_LAYER_PATH=$(SDK)/share/vulkan/explicit_layer.d $(B)/loadersubpasses --compute
	ZVK_VALIDATE=1 VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) VK_LAYER_PATH=$(SDK)/share/vulkan/explicit_layer.d $(B)/loadersubpasses --formats
	ZVK_VALIDATE=1 VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) VK_LAYER_PATH=$(SDK)/share/vulkan/explicit_layer.d $(B)/loadersubpasses --robust
	ZVK_VALIDATE=1 VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) VK_LAYER_PATH=$(SDK)/share/vulkan/explicit_layer.d $(B)/loadersubpasses --multisample
	ZVK_VALIDATE=1 VK_DRIVER_FILES=$(abspath $(B)/zvk_icd.json) VK_LAYER_PATH=$(SDK)/share/vulkan/explicit_layer.d $(B)/loadersubpasses --atlas
endif
check-window:
	$(MAKE) HOST=0 ARCH=aarch64 all window
	$(MAKE) -C ../zwm ARCH=aarch64 SYSROOT=$(abspath ../sysroot-aarch64) all
	python3 -u scripts/test-sic.py --window --log ../build/zvk-window-serial.log
check-window-native:
	$(MAKE) HOST=0 ARCH=aarch64 all window
	$(MAKE) -C ../zwm ARCH=aarch64 SYSROOT=$(abspath ../sysroot-aarch64) all
	python3 -u scripts/test-sic.py --window --native-build --log ../build/zvk-window-native-serial.log

.PHONY: native-loader native-shared sdl install-shared sdk check-sdl
native-loader: deps tools
	python3 scripts/native-loader.py --arch $(ARCH) --sysroot $(SYSROOT)
native-shared: deps build/spv_min_words.h src/procs.inc
	python3 scripts/shared.py --arch $(ARCH) --sysroot $(SYSROOT)
ifeq ($(HOST),0)
# Static libraries and native clang keep their existing defaults.
shared: native-shared
sdl: native-loader native-shared install-headers build/shaders.h
	$(MAKE) -C ../zde/ports/sdl2 ARCH=$(ARCH) SYSROOT=$(SYSROOT) ZVK_SHARED=$(abspath $(B)) install-shared
	$(CC) $(CFLAGS) -DZVK_SDL -c examples/window.c -o $(B)/sdlwindow.o
	$(LD) $(EMUL) -pie --dynamic-linker=/lib/ld-musl-$(ARCH).so.1 -z max-page-size=4096 -o $(B)/sdlwindow $(SYSROOT)/usr/lib/Scrt1.o $(SYSROOT)/usr/lib/crti.o $(B)/sdlwindow.o -L$(SYSROOT)/usr/lib -L$(B) -lSDL2 -lc $(SYSROOT)/usr/lib/libcompiler_rt.a $(SYSROOT)/usr/lib/crtn.o
	@printf '\123' | dd of=$(B)/sdlwindow bs=1 seek=7 count=1 conv=notrunc status=none
install-shared: install sdl
	mkdir -p $(SYSROOT)/usr/share/vulkan/icd.d $(SYSROOT)/rootfs/usr/lib $(SYSROOT)/rootfs/usr/share/vulkan/icd.d $(SYSROOT)/rootfs/lib
	cp $(B)/libzvk.so $(B)/libvirgl.so $(B)/libzgl.so $(B)/libzwm.so $(B)/libvulkan.so.1 $(SYSROOT)/usr/lib/
	ln -sf libvulkan.so.1 $(SYSROOT)/usr/lib/libvulkan.so
	cp $(B)/zvk_icd.json $(SYSROOT)/usr/share/vulkan/icd.d/zvk.json
	cp $(B)/libzvk.so $(B)/libvirgl.so $(B)/libzgl.so $(B)/libzwm.so $(B)/libvulkan.so.1 $(SYSROOT)/usr/lib/libSDL2.so $(SYSROOT)/usr/lib/libc.so $(SYSROOT)/rootfs/usr/lib/
	ln -sf libvulkan.so.1 $(SYSROOT)/rootfs/usr/lib/libvulkan.so
	cp $(SYSROOT)/usr/lib/libc.so $(SYSROOT)/rootfs/lib/ld-musl-$(ARCH).so.1
	cp $(B)/zvk_icd.json $(SYSROOT)/rootfs/usr/share/vulkan/icd.d/zvk.json
	cp $(B)/sdlwindow $(SYSROOT)/rootfs/bin/
sdk: install-shared
	$(MAKE) -C ../zwm ARCH=$(ARCH) SYSROOT=$(SYSROOT) all
	python3 scripts/package-sdk.py --arch $(ARCH) --sysroot $(SYSROOT)
endif
SIC_TEST_FLAGS ?=
check-sdl:
	$(MAKE) HOST=0 ARCH=aarch64 sdk
	$(MAKE) -C ../zwm ARCH=aarch64 SYSROOT=$(abspath ../sysroot-aarch64) all
	python3 -u scripts/test-sic.py --sdl --sdk build/zvk-sdk-aarch64.tar --timeout 600 $(SIC_TEST_FLAGS) --log ../build/zvk-sdl-serial.log

.PHONY: install-headers
install-headers: deps
	mkdir -p $(SYSROOT)/usr/include/vk_video $(SYSROOT)/usr/include/vulkan
	cp $(VKH)/vulkan/*.h $(SYSROOT)/usr/include/vulkan/
	cp $(VKH)/vk_video/*.h $(SYSROOT)/usr/include/vk_video/
	cp ../zwm/include/zwm.h include/zvk.h include/zvk_zwm.h $(SYSROOT)/usr/include/
