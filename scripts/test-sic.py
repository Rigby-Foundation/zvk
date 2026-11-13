#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Boot sic with virgl and run the self-contained Vulkan tests in a snapshot.
The persistent root disk and FAT scratch image must already exist.
"""
import argparse
import os
import platform
import re
import json
import socket
import select
import shutil
import subprocess
import time
from pathlib import Path
from copyutil import copy2

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=ROOT/'zvk/build/sic-tests.img')
    parser.add_argument('--disk', type=Path, default=ROOT/'build/arm64-disk.img')
    parser.add_argument('--command', help='override the guest command')
    parser.add_argument('--idle-poll', action='store_true', help='diagnose ARM64 idle/timer stalls by disabling WFI (keeps CPUs busy)')
    parser.add_argument('--native-build', action='store_true', help='also compile the example with clang inside sic')
    parser.add_argument('--log', type=Path, default=ROOT/'build/zvk-sic-serial.log')
    parser.add_argument('--summary', type=Path, help='also save the readable command output here')
    parser.add_argument('--timeout', type=int, default=300)
    parser.add_argument('--window', action='store_true', help='run animated swapchain/resize tests against the updated zwm')
    parser.add_argument('--sdl', action='store_true', help='test shared Khronos loader and SDL2 Vulkan, including native compilation')
    parser.add_argument('--metal-graphics', action='store_true', help='also compile and upload experimental Metal vertex/fragment shaders inside sic')
    parser.add_argument('--sdk', type=Path, help='also install and test this SDK tar inside the snapshot')
    args = parser.parse_args()
    if args.sdl: args.window = True
    qemu = os.environ.get('QEMU_VIRGL_ARM64') or str(Path.home()/'qemu-virgl/bin/qemu-system-aarch64')
    if not Path(qemu).is_file(): raise SystemExit('virgl-enabled QEMU is required; set QEMU_VIRGL_ARM64')
    stage=ROOT/'zvk/build/sic-stick'
    if stage.exists():shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for name in ['zvktest','zvktriangle','wsitest']:
        binary=ROOT/'zvk/build/aarch64'/name
        if not binary.is_file():raise SystemExit('run make -C zvk ARCH=aarch64 first')
        copy2(binary,stage/name)
    if args.metal_graphics:
        for name in ('metalstages','metaltransport'):
            binary=ROOT/'zvk/build/aarch64'/name
            if not binary.is_file():raise SystemExit('run make -C zvk HOST=0 ARCH=aarch64 build/aarch64/metalstages build/aarch64/metaltransport')
            copy2(binary,stage/name)
        (stage/'check-metal-graphics.sh').write_text("""#!/bin/sh
set -e
mkdir -p /tmp/zvk-metal-stages
/mnt/nvme0n1/metalstages /tmp/zvk-metal-stages
/mnt/nvme0n1/metaltransport /tmp/zvk-metal-stages
echo ZVK_METAL_GRAPHICS_TRANSPORT_PASS
""")
    if args.window:
        copy2(ROOT/'zvk/build/aarch64/zvkwindow',stage/'zvkwindow')
        copy2(ROOT/'zwm/build/aarch64/zwm',stage/'zwm')
        (stage/'check-window.sh').write_text("""#!/bin/sh
set -e
/mnt/nvme0n1/zvktest
/mnt/nvme0n1/wsitest
ZWM_SHOT_FB=1 exec /mnt/nvme0n1/zwm &
server=$!
sleep 1
set +e
/mnt/nvme0n1/zvkwindow --selftest
status=$?
if test "$status" = 0; then
    /mnt/nvme0n1/zvkwindow --frames 12
    status=$?
fi
kill $server
wait $server
exit $status
""")
    if args.native_build:
        sdk=stage/'sdk'
        for directory in ['include','lib','src']:(sdk/directory).mkdir(parents=True,exist_ok=True)
        for directory in ['vulkan','vk_video']:
            shutil.copytree(ROOT/'zvk/build/deps/vulkan-headers/include'/directory,sdk/'include'/directory,copy_function=copy2)
        copy2(ROOT/'zvk/include/zvk.h',sdk/'include/zvk.h')
        copy2(ROOT/'zvk/include/zvk_zwm.h',sdk/'include/zvk_zwm.h')
        copy2(ROOT/'zwm/include/zwm.h',sdk/'include/zwm.h')
        copy2(ROOT/'zwm/build/aarch64/libzwm.a',sdk/'lib/libzwm.a')
        copy2(ROOT/'zvk/build/aarch64/libzvk.a',sdk/'lib/libzvk.a')
        copy2(ROOT/'zgl/build/aarch64/libvirgl.a',sdk/'lib/libvirgl.a')
        copy2(ROOT/'zvk/examples/triangle.c',sdk/'src/triangle.c')
        copy2(ROOT/'zvk/examples/window.c',sdk/'src/window.c')
        copy2(ROOT/'zvk/build/shaders.h',sdk/'src/shaders.h')
        (stage/'check-native.sh').write_text("""#!/bin/sh
set -e
/mnt/nvme0n1/zvktest
/mnt/nvme0n1/wsitest
/mnt/nvme0n1/zvktriangle /tmp/zvktriangle-cross.ppm
echo 'Compiling Vulkan example with native sic clang...'
clang -O2 -I/mnt/nvme0n1/sdk/include -I/mnt/nvme0n1/sdk/src /mnt/nvme0n1/sdk/src/triangle.c -L/mnt/nvme0n1/sdk/lib -lzvk -lvirgl -lzwm -lm -lpthread -o /tmp/zvktriangle-native
/tmp/zvktriangle-native /tmp/zvktriangle-native.ppm
echo 'ZVK_NATIVE_COMPILE_PASS'
""")
    if args.window and args.native_build:
        check=stage/'check-window.sh'
        source=check.read_text().replace('kill $server',"""if test "$status" = 0; then
    echo 'Compiling windowed Vulkan example inside sic...'
    clang -O2 -I/mnt/nvme0n1/sdk/include -I/mnt/nvme0n1/sdk/src /mnt/nvme0n1/sdk/src/window.c -L/mnt/nvme0n1/sdk/lib -lzvk -lvirgl -lzwm -lm -lpthread -o /tmp/zvkwindow-native
    status=$?
    if test "$status" = 0; then
        /tmp/zvkwindow-native --selftest
        status=$?
    fi
fi
kill $server""")
        check.write_text(source)
    if args.sdl:
        sdk=stage/'sdk'
        for directory in ['include','lib','src','share/vulkan/icd.d']:(sdk/directory).mkdir(parents=True,exist_ok=True)
        for directory in ['vulkan','vk_video']:
            shutil.copytree(ROOT/'zvk/build/deps/vulkan-headers/include'/directory,sdk/'include'/directory,dirs_exist_ok=True,copy_function=copy2)
        shutil.copytree(ROOT/'zde/ports/sdl2/build/aarch64/include/SDL2',sdk/'include/SDL2',dirs_exist_ok=True,copy_function=copy2)
        for name in ['zvk.h','zvk_zwm.h']:copy2(ROOT/'zvk/include'/name,sdk/'include'/name)
        for name in ['libzwm.so','libvirgl.so','libzgl.so','libzvk.so','libvulkan.so.1']:
            copy2(ROOT/'zvk/build/aarch64'/name,sdk/'lib'/name)
        copy2(ROOT/'zde/ports/sdl2/build/aarch64/libSDL2.so',sdk/'lib/libSDL2.so')
        copy2(ROOT/'sysroot-aarch64/usr/lib/libc.so',sdk/'lib/libc.so')
        copy2(ROOT/'zvk/build/aarch64/zvk_icd.json',sdk/'share/vulkan/icd.d/zvk.json')
        for name in ['window.c','window_sdl.h','triangle.c','compute.c','Makefile.sic','stamp-elf.c']:
            copy2(ROOT/'zvk/examples'/name,sdk/'src'/name)
        copy2(ROOT/'zvk/build/shaders.h',sdk/'src/shaders.h')
        copy2(ROOT/'zvk/tests/loaderprobe.c',sdk/'src/loaderprobe.c')
        copy2(ROOT/'zvk/tests/dynamic/smoke.c',sdk/'src/dynamic-smoke.c')
        copy2(ROOT/'zvk/tests/dynamic/plugin.c',sdk/'src/plugin.c')
        (sdk/'share/vulkan/icd.d/broken.json').write_text(json.dumps({'file_format_version':'1.0.0','ICD':{'library_path':'/nonexistent-libzvk.so','api_version':'1.0.0'}}))
        copy2(ROOT/'zvk/build/aarch64/sdlwindow',stage/'sdlwindow')
        (stage/'check-sdl.sh').write_text("""#!/bin/sh
set -e
mkdir -p /lib
cp /mnt/nvme0n1/sdk/lib/libc.so /lib/ld-musl-aarch64.so.1
export LD_LIBRARY_PATH=/mnt/nvme0n1/sdk/lib
export VK_DRIVER_FILES=/mnt/nvme0n1/sdk/share/vulkan/icd.d/zvk.json
export SDL_VIDEODRIVER=zwm
/mnt/nvme0n1/zvktest
/mnt/nvme0n1/wsitest
ZWM_SHOT_FB=1 exec /mnt/nvme0n1/zwm &
server=$!
export ZVK_TEST_COMPOSITOR_PID=$server
sleep 1
set +e
/mnt/nvme0n1/sdlwindow --selftest
status=$?
if test "$status" = 0; then
    mkdir -p /tmp/zvk-native
    cp /mnt/nvme0n1/sdk/src/* /tmp/zvk-native/
    cd /tmp/zvk-native
    make -f Makefile.sic SDK=/mnt/nvme0n1/sdk
    status=$?
    if test "$status" = 0; then
        ./dynamic-smoke ./libplugin.so && ./loaderprobe && VK_DRIVER_FILES=/nonexistent-icd.json ./loaderprobe --missing-driver && VK_DRIVER_FILES=/mnt/nvme0n1/sdk/share/vulkan/icd.d/broken.json ./loaderprobe --missing-driver && ./loadertriangle /tmp/loader-triangle.ppm
        status=$?
    fi
    if test "$status" = 0; then
        ./sdlwindow --selftest
        status=$?
    fi
fi
kill $server
wait $server
exit $status
""")
    if args.sdk:
        if not args.sdl:raise SystemExit('--sdk requires --sdl')
        copy2(args.sdk,stage/'zvk-sdk.tar')
        check=stage/'check-sdl.sh'
        check.write_text(check.read_text().replace('kill $server', """if test "$status" = 0; then
    tar -xf /mnt/nvme0n1/zvk-sdk.tar -C /
    status=$?
    unset LD_LIBRARY_PATH VK_DRIVER_FILES SDL_VULKAN_LIBRARY SDL_VIDEODRIVER
    if test "$status" = 0; then
        cd /tmp/zvk-native
        ./loaderprobe && ./compute && /bin/sdlwindow --frames 12
        status=$?
    fi
    if test "$status" = 0; then
        mkdir -p /tmp/zvk-installed
        cp /usr/share/zvk/examples/* /tmp/zvk-installed/
        cd /tmp/zvk-installed
        make -f Makefile.sic SDK=/usr && ./loaderprobe && ./compute && ./dynamic-smoke ./libplugin.so && ./sdlwindow --frames 12
        status=$?
    fi
    if test "$status" = 0; then echo ZVK_INSTALLED_SDK_PASS; fi
fi
kill $server"""))
    args.image.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run(['python3',str(ROOT/'sic/scripts/mkzaefs.py'),'-L','ZVK','-s',('128M' if args.sdl else '32M'),str(stage),str(args.image)],check=True)
    if platform.system() == 'Darwin' and platform.machine() == 'arm64':
        accel = ['-accel', 'hvf', '-cpu', 'host']
    elif platform.system() == 'Linux' and platform.machine() in ('arm64', 'aarch64'):
        accel = ['-accel', 'kvm', '-cpu', 'host']
    else:
        accel = ['-accel', 'tcg', '-cpu', 'cortex-a72']
    command = [qemu, '-snapshot'] + [
        '-M', 'virt', *accel, '-smp', '4', '-m', '8192',
        '-kernel', str(ROOT/'sysroot-aarch64/boot/sic.img'),
        '-initrd', str(ROOT/'sysroot-aarch64/boot/initrd.tar')]
    if args.idle_poll: command += ['-append', 'idle=poll']
    for i, disk in enumerate([args.image, ROOT/'build/arm64-fat.img', args.disk]):
        command += ['-drive', 'format=raw,discard=unmap,file=%s,if=none,id=d%d' % (disk.resolve(), i),
                    '-device', 'nvme,serial=sic%d,drive=d%d' % (i,i)]
    command += ['-device', 'virtio-gpu-gl-pci', '-display', 'sdl,gl=on', '-monitor', 'none', '-serial', 'stdio', '-no-reboot']
    qmp_path=ROOT/('zvk/build/window-qmp-%d.sock'%os.getpid())
    if args.window:command += ['-qmp','unix:%s,server=on,wait=off'%qmp_path,'-device','virtio-keyboard-pci','-device','virtio-mouse-pci']
    def send_close():
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as sock:
            sock.settimeout(10);sock.connect(str(qmp_path))
            with sock.makefile('rwb',buffering=0) as stream:
                stream.readline()
                for command in [{'execute':'qmp_capabilities'},{'execute':'human-monitor-command','arguments':{'command-line':'sendkey q'}}]:
                    stream.write((json.dumps(command)+'\n').encode())
                    while 'return' not in json.loads(stream.readline()):pass
    pending_capture=[]
    def capture(fields,save=False):
        frame,physical,width,height,pitch=map(int,fields)
        target=ROOT/('build/zvk-window-%s.ppm'%frame)
        raw_path=target.with_suffix('.raw')
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as sock:
            sock.settimeout(10);sock.connect(str(qmp_path))
            with sock.makefile('rwb',buffering=0) as stream:
                json.loads(stream.readline())
                def request(command):
                    stream.write((json.dumps(command)+'\n').encode())
                    while True:
                        reply=json.loads(stream.readline())
                        if 'error' in reply:raise RuntimeError('QMP screenshot failed: '+str(reply))
                        if 'return' in reply:return reply
                request({'execute':'qmp_capabilities'})
                # Wait for the compositor's explicit completion marker before
                # dumping fb RAM; GPU readback takes longer than a fixed delay.
                if not save:
                    request({'execute':'human-monitor-command','arguments':{'command-line':'sendkey ctrl-alt-p'}})
                    return
                request({'execute':'human-monitor-command','arguments':{'command-line':'pmemsave %d %d "%s"'%(physical,pitch*height,raw_path)}})
        raw=raw_path.read_bytes();packed=b''.join(raw[y*pitch:y*pitch+width*4] for y in range(height))
        rgb=bytearray(width*height*3);rgb[0::3]=packed[2::4];rgb[1::3]=packed[1::4];rgb[2::3]=packed[0::4]
        target.write_bytes(('P6\n%d %d\n255\n'%(width,height)).encode()+rgb)
        print('Screenshot: '+str(target),flush=True)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    if args.summary: args.summary.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('wb') as log, (args.summary or args.log.with_suffix('.summary.log')).open('w') as summary:
        def say(message):
            print(message, flush=True)
            summary.write(message+'\n'); summary.flush()
        env=os.environ.copy()
        virgl=ROOT/'zgl/tests/vrend/build/virglrenderer-host/lib'
        if virgl.is_dir():env['DYLD_LIBRARY_PATH']=str(virgl)
        proc = subprocess.Popen(command, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        buf = bytearray()
        live = bytearray()
        report = False
        stall_clock = []
        def wait(pattern, timeout):
            end = time.monotonic()+timeout
            while time.monotonic() < end:
                match = re.search(pattern, buf)
                if match: return match
                if proc.poll() is not None: break
                ready, _, _ = select.select([proc.stdout], [], [], 1)
                if ready:
                    chunk = os.read(proc.stdout.fileno(), 65536)
                    if not chunk: break
                    log.write(chunk); log.flush()
                    buf.extend(chunk)
                    if report:
                        live.extend(chunk)
                        while b'\n' in live:
                            endline = live.index(b'\n')
                            line = bytes(live[:endline]); del live[:endline+1]
                            line = re.sub(rb'\x1b\[[0-9;]*[A-Za-z]', b'', line)
                            clean=line.rstrip(b'\r').split(b'\r')[-1].decode(errors='replace')
                            say(clean)
                            if args.sdl and clean == 'SDL_SLOW_FRAME_BEGIN':
                                if stall_clock: raise RuntimeError('overlapping compositor pauses')
                                stall_clock.append(time.monotonic())
                            elapsed = re.fullmatch(r'SDL_SLOW_FRAME_PASS (\d+)ms', clean)
                            if args.sdl and elapsed:
                                if not stall_clock: raise RuntimeError('missing compositor pause start')
                                host_ms = round((time.monotonic() - stall_clock.pop()) * 1000)
                                guest_ms = int(elapsed[1])
                                if host_ms < 2000 or abs(host_ms - guest_ms) > 3000:
                                    raise RuntimeError('guest clock drift: host %d ms, guest %d ms' % (host_ms, guest_ms))
                                say('SDL_HOST_CLOCK_PASS host_ms=%d guest_ms=%d' % (host_ms, guest_ms))
                            if args.sdl and clean=='SDL_CLOSE_REQUEST':send_close()
                            if args.window and clean.startswith('WINDOW_CAPTURE '):
                                pending_capture[:]=clean.split()[1:];capture(pending_capture)
                            if args.window and clean=='ZWM_CAPTURE_READY' and pending_capture:
                                capture(pending_capture,True);pending_capture.clear()
            raise RuntimeError('VM timed out or exited; see '+str(args.log))
        try:
            wait(b'sic shell', 180)
            if b'persistent disk /dev/nvme2n1 mounted directly as /' not in buf:
                raise RuntimeError('unexpected root disk; no commands sent')
            say('Root: %s (%s)' % (args.disk, 'snapshot'))
            time.sleep(2)
            report = True
            guest = args.command or ('sh /mnt/nvme0n1/check-sdl.sh' if args.sdl else 'sh /mnt/nvme0n1/check-window.sh' if args.window else 'sh /mnt/nvme0n1/check-native.sh' if args.native_build else '/mnt/nvme0n1/zvktest && /mnt/nvme0n1/wsitest && /mnt/nvme0n1/zvktriangle /tmp/zvktriangle.ppm')
            if args.metal_graphics:guest='sh /mnt/nvme0n1/check-metal-graphics.sh && '+guest
            line = guest+'; echo __ZVK_STATUS__$?\n'
            for char in line.encode():
                proc.stdin.write(bytes([char])); proc.stdin.flush(); time.sleep(0.04)
            status = wait(rb'\r?\n__ZVK_STATUS__(\d+)\r?\n', args.timeout)
            if status[1] != b'0': raise SystemExit('Guest command failed: '+status[1].decode())
            say('Guest checks passed. Serial log: '+str(args.log))
        finally:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
            qmp_path.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
