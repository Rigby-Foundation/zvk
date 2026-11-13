#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run a selected official CTS group against only the local zVk ICD on macOS."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def case_names(path, base=None, active=()):
    """Expand official nested mustpass lists without changing their selection."""
    path = path.resolve()
    if path in active:
        raise ValueError('recursive CTS list: ' + str(path))
    base = base or path.parent
    for line in path.read_text().splitlines():
        name = line.strip()
        if not name or name.startswith('#'):
            continue
        if name.startswith('dEQP-'):
            yield name
        else:
            nested = base / name
            if not nested.is_file():
                nested = path.parent / name
            yield from case_names(nested, base, (*active, path))


def freeze_loader(results, source):
    directory = results / 'loader-inputs' / digest(source)
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / 'libvulkan.dylib'
    if not target.exists():
        temporary = directory / ('loader.tmp.' + str(os.getpid()))
        shutil.copyfile(source, temporary)
        if digest(temporary) != directory.name:
            temporary.unlink()
            raise RuntimeError('loader changed while freezing CTS inputs')
        temporary.replace(target)
    if digest(target) != directory.name:
        raise RuntimeError('frozen loader does not match its hash')
    return target


def freeze_inputs(results, driver, backend, icd):
    """Keep an in-progress CTS run independent of subsequent development builds."""
    hashes = {'driver_sha256': digest(driver), 'backend_sha256': digest(backend)}
    directory = results / 'inputs' / (hashes['driver_sha256'] + '-' + hashes['backend_sha256'])
    directory.mkdir(parents=True, exist_ok=True)
    for source in (driver, backend):
        target = directory / source.name
        if not target.exists():
            temporary = target.with_suffix('.tmp.' + str(os.getpid()))
            shutil.copyfile(source, temporary)
            if digest(temporary) != digest(source):
                temporary.unlink()
                raise RuntimeError('CTS input changed while copying: ' + str(source))
            temporary.replace(target)
        if digest(target) != hashes['driver_sha256' if source == driver else 'backend_sha256']:
            raise RuntimeError('CTS input snapshot does not match its hash: ' + str(target))
    manifest = json.loads(icd.read_text())
    manifest['ICD']['library_path'] = str(directory / driver.name)
    target_icd = directory / 'zvk_icd.json'
    temporary_icd = target_icd.with_suffix('.tmp.' + str(os.getpid()))
    temporary_icd.write_text(json.dumps(manifest, indent=2) + '\n')
    temporary_icd.replace(target_icd)
    return directory, target_icd, hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', default='dEQP-VK.info.*', help='CTS case name or wildcard')
    parser.add_argument('--caselist', type=Path, help='official CTS case-list file (including nested lists)')
    parser.add_argument('--inputs', type=Path, help='reuse an immutable input directory from an earlier report')
    parser.add_argument('--images', action='store_true', help='include diagnostic result images in QPA')
    parser.add_argument('--validation', action='store_true', help='audit test API usage with the pinned Khronos validation layer')
    parser.add_argument('--loader', type=Path, help='alternate loader library for isolated loader regression testing')
    parser.add_argument('--allow-unsupported', action='store_true', help='accept complete batches containing only NotSupported cases')
    parser.add_argument('--name', default='cts', help='output basename, inside zvk/build/cts/results')
    parser.add_argument('--timeout', type=int, default=1800, help='maximum run time in seconds')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name) or args.timeout <= 0:
        parser.error('name must use letters, numbers, underscores or hyphens; timeout must be positive')
    binary = ROOT / 'build/cts/build/external/vulkancts/modules/vulkan/deqp-vk'
    loader = args.loader.resolve() if args.loader else ROOT / 'build/tools/vulkan-sdk/lib/libvulkan.dylib'
    icd = ROOT / 'build/host/zvk_icd.json'
    driver = ROOT / 'build/host/libzvk.dylib'
    backend = ROOT.parent / 'zgl/tests/vrend/build/virglrenderer-host/lib/libvirglrenderer.1.dylib'
    for path in (binary, loader, icd, driver, backend):
        if not path.is_file():
            raise SystemExit('Missing ' + str(path) + '; run make HOST=1 shared validation-tools cts-tools')
    results = ROOT / 'build/cts/results'
    results.mkdir(parents=True, exist_ok=True)
    loader = freeze_loader(results, loader)
    if args.inputs:
        inputs = args.inputs.resolve()
        driver, backend, icd = inputs / driver.name, inputs / backend.name, inputs / icd.name
        hashes = {'driver_sha256': digest(driver), 'backend_sha256': digest(backend)}
        if inputs.name != hashes['driver_sha256'] + '-' + hashes['backend_sha256']:
            parser.error('input snapshot hashes no longer match its directory name')
        if json.loads(icd.read_text())['ICD']['library_path'] != str(driver):
            parser.error('input manifest does not point at the frozen driver')
    else:
        inputs, icd, hashes = freeze_inputs(results, driver, backend, icd)
    prefix = results / args.name
    qpa, log, report = (prefix.with_suffix(ext) for ext in ('.qpa', '.log', '.json'))
    # An interrupted or unmatched invocation must never inherit an earlier result.
    qpa.unlink(missing_ok=True)
    selection = '--deqp-caselist-file=' + str(args.caselist.resolve()) if args.caselist else '--deqp-case=' + args.case
    command = [str(binary), '--deqp-vk-library-path=' + str(loader), selection,
               '--deqp-log-filename=' + str(qpa), '--deqp-log-images=' + ('enable' if args.images else 'disable'),
               '--deqp-log-shader-sources=disable']
    env = os.environ.copy()
    env['VK_DRIVER_FILES'] = env['VK_ICD_FILENAMES'] = str(icd)
    # dyld searches this directory before absolute dependency install names.
    # Both the ICD and its private backend therefore come from this snapshot.
    env['DYLD_LIBRARY_PATH'] = str(inputs)
    env.pop('VK_INSTANCE_LAYERS', None)
    validation_hash = None
    if args.validation:
        layer = ROOT / 'build/tools/vulkan-sdk/lib/libVkLayer_khronos_validation.dylib'
        manifest = ROOT / 'build/tools/vulkan-sdk/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json'
        if not layer.is_file() or not manifest.is_file():
            parser.error('validation layer is missing; run make validation-tools')
        validation_hash = digest(layer)
        layer_directory = results / 'validation-inputs' / validation_hash
        layer_directory.mkdir(parents=True, exist_ok=True)
        frozen_layer = layer_directory / layer.name
        if not frozen_layer.exists():
            temporary = layer_directory / ('layer.tmp.' + str(os.getpid()))
            shutil.copyfile(layer, temporary)
            if digest(temporary) != validation_hash:
                temporary.unlink()
                raise RuntimeError('validation layer changed while freezing CTS inputs')
            temporary.replace(frozen_layer)
        if digest(frozen_layer) != validation_hash:
            raise RuntimeError('frozen validation layer does not match its hash')
        layer_manifest = json.loads(manifest.read_text())
        layer_manifest['layer']['library_path'] = str(frozen_layer)
        (layer_directory / manifest.name).write_text(json.dumps(layer_manifest, indent=2) + '\n')
        env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_KHRONOS_validation'
        env['VK_LAYER_PATH'] = str(layer_directory)
    timed_out = interrupted = False
    print('CTS selection: ' + selection, flush=True)
    with log.open('w') as output:
        process = subprocess.Popen(command, cwd=binary.parent, env=env, stdout=output,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        except KeyboardInterrupt:
            interrupted = True
            os.killpg(process.pid, signal.SIGTERM)
            process.wait()
    text = qpa.read_text(errors='replace') if qpa.is_file() else ''
    statuses, failures, executed = Counter(), [], []
    for case, block in re.findall(r'#beginTestCaseResult ([^\n]+)\n(.*?)#endTestCaseResult', text, re.S):
        executed.append(case)
        try:
            result = ET.fromstring(block).find('Result')
            if result is None:
                raise ValueError('missing result')
            status = result.attrib['StatusCode']
            message = ''.join(result.itertext())
        except (ET.ParseError, ValueError, KeyError) as error:
            status, message = 'InvalidResult', str(error)
        statuses[status] += 1
        if status not in ('Pass', 'NotSupported'):
            failures.append({'case': case, 'status': status, 'message': message})
    begun = re.findall(r'^#beginTestCaseResult ([^\n]+)', text, re.M)
    for case in begun[len(executed):]:
        failures.append({'case': case, 'status': 'Incomplete',
                         'message': 'test process terminated before producing a result'})
    expected = list(case_names(args.caselist)) if args.caselist else None
    coverage = expected is None or (len(executed) == len(expected) and set(executed) == set(expected))
    diagnostics = log.read_text(errors='replace')
    validation_errors = diagnostics.count('Validation Error:') if args.validation else 0
    complete = ('#endSession' in text and len(begun) == len(executed) and coverage
                and 'Test run was ABORTED' not in diagnostics)
    passed = (complete and not timed_out and not interrupted and process.returncode == 0
              and (statuses['Pass'] > 0 or (args.allow_unsupported and statuses['NotSupported'] > 0))
              and not failures and not validation_errors)
    data = {'suite': 'vulkan-cts-1.4.7.0', 'case_filter': args.case,
            'validation_enabled': args.validation, 'validation_layer_sha256': validation_hash,
            'validation_errors': validation_errors,
            **hashes, 'binary_sha256': digest(binary), 'loader_sha256': digest(loader),
            'inputs': str(inputs), 'caselist': str(args.caselist.resolve()) if args.caselist else None,
            'caselist_sha256': digest(args.caselist) if args.caselist else None,
            'expected_cases': len(expected) if expected is not None else None,
            'executed_cases': len(executed), 'exact_coverage': coverage,
            'returncode': process.returncode, 'timed_out': timed_out, 'interrupted': interrupted, 'complete': complete,
            'counts': dict(statuses), 'failures': failures, 'passed': passed,
            'log': str(log), 'qpa': str(qpa)}
    report.write_text(json.dumps(data, indent=2) + '\n')
    print(json.dumps({key: data[key] for key in ('counts', 'complete', 'timed_out', 'passed')}))
    for failure in failures:
        print(failure['case'] + ': ' + failure['status'] + ' (' + failure['message'] + ')')
    print('Report: ' + str(report))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
