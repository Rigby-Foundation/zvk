#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the complete official mustpass list in resumable, disk-bounded batches.

Every batch uses one frozen driver/backend pair. Only CTS support checks may
mark cases NotSupported. Crashes, timeouts, missing cases and unexpected cases
remain failures; successful results from a different build are never reused.
"""
import argparse
from collections import Counter
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

spec = importlib.util.spec_from_file_location('cts_runner', Path(__file__).with_name('run-cts.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
ROOT = runner.ROOT
RESULTS = ROOT / 'build/cts/results'


def archive(path):
    if not path.is_file() or path.suffix == '.gz':
        return str(path)
    target = path.with_suffix(path.suffix + '.gz')
    if target.exists():
        # Preserve an earlier attempt if a process was interrupted after
        # writing its replacement report but before archiving its output.
        target = path.with_suffix(path.suffix + '.' + runner.digest(path)[:16] + '.gz')
    with path.open('rb') as source, gzip.open(target, 'wb') as output:
        shutil.copyfileobj(source, output)
    path.unlink()
    return str(target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--list', type=Path, default=ROOT / 'build/cts/source/external/vulkancts/mustpass/main/vk-default.txt')
    parser.add_argument('--inputs', type=Path, help='frozen input directory from a previous CTS report')
    parser.add_argument('--loader', type=Path, help='alternate loader library')
    parser.add_argument('--chunk-size', type=int, default=512)
    parser.add_argument('--timeout', type=int, default=1800, help='per-batch timeout, seconds')
    parser.add_argument('--group', help='restrict to this case prefix for diagnostics; never a full run')
    args = parser.parse_args()
    if args.chunk_size <= 0 or args.timeout <= 0:
        parser.error('chunk-size and timeout must be positive')
    RESULTS.mkdir(parents=True, exist_ok=True)
    loader = runner.freeze_loader(RESULTS, args.loader.resolve() if args.loader else
                                  ROOT / 'build/tools/vulkan-sdk/lib/libvulkan.dylib')
    if args.inputs:
        inputs = args.inputs.resolve()
    else:
        inputs, _, _ = runner.freeze_inputs(RESULTS, ROOT / 'build/host/libzvk.dylib',
            ROOT.parent / 'zgl/tests/vrend/build/virglrenderer-host/lib/libvirglrenderer.1.dylib',
            ROOT / 'build/host/zvk_icd.json')
    hashes = {'driver_sha256': runner.digest(inputs / 'libzvk.dylib'),
              'backend_sha256': runner.digest(inputs / 'libvirglrenderer.1.dylib'),
              'binary_sha256': runner.digest(ROOT / 'build/cts/build/external/vulkancts/modules/vulkan/deqp-vk'),
              'loader_sha256': runner.digest(loader)}
    # Hash the ordered, fully expanded official selection. Nested list changes
    # invalidate resumptions even if the top-level file has stayed the same.
    selection = hashlib.sha256()
    expected = 0
    for case in runner.case_names(args.list):
        if args.group and not case.startswith(args.group):
            continue
        selection.update((case + '\n').encode())
        expected += 1
    identity = hashlib.sha256(json.dumps([hashes, selection.hexdigest(), args.chunk_size], sort_keys=True).encode()).hexdigest()
    name = 'mustpass-' + identity[:20]
    summary_path = RESULTS / (name + '.json')
    counts, failures, batches, covered = Counter(), [], [], 0
    attempts, scheduled = [], 0
    def summarize(finished=False):
        data = {'suite': 'vulkan-cts-1.4.7.0', **hashes, 'inputs': str(inputs),
                'official_list': str(args.list.resolve()), 'selection_sha256': selection.hexdigest(),
                'group': args.group, 'expected_cases': expected, 'executed_cases': covered,
                'counts': dict(counts), 'failures': failures, 'batches': batches,
                'attempts': attempts, 'scheduled_cases': scheduled,
                'complete': finished and covered == expected and all(b['complete'] for b in batches),
                'passed': False}
        data['passed'] = data['complete'] and counts['Pass'] > 0 and not failures
        official=ROOT/'build/cts/source/external/vulkancts/mustpass/main/vk-default.txt'
        data['full_mustpass_passed'] = data['passed'] and not args.group and args.list.resolve()==official.resolve()
        temporary = summary_path.with_suffix('.tmp')
        temporary.write_text(json.dumps(data, indent=2) + '\n')
        temporary.replace(summary_path)
        return data
    def run_batch(cases, tag):
        nonlocal covered, scheduled
        batch_name = name + '-' + tag
        report = RESULTS / (batch_name + '.json')
        casefile = RESULTS / (batch_name + '.txt')
        content = ''.join(c + '\n' for c in cases)
        casehash = hashlib.sha256(content.encode()).hexdigest()
        data = json.loads(report.read_text()) if report.is_file() else None
        if not (data and data['caselist_sha256'] == casehash
                and all(data.get(k) == v for k, v in hashes.items())):
            # Keep enough room for crash reports and avoid filling the user's disk.
            if shutil.disk_usage(RESULTS).free < 512 * 1024 * 1024:
                raise RuntimeError('less than 512 MiB free; CTS paused before writing another batch')
            casefile.write_text(content)
            command = [sys.executable, str(Path(__file__).with_name('run-cts.py')),
                       '--caselist', str(casefile), '--inputs', str(inputs), '--name', batch_name,
                       '--loader', str(loader),
                       '--timeout', str(args.timeout), '--allow-unsupported']
            subprocess.run(command, check=False)
            data = json.loads(report.read_text())
        # An incomplete parent is immutable evidence too. Reuse it on resume
        # and visit its children, retaining completed children and executing
        # only missing selections rather than repeating the original timeout.
        for key in ('log', 'qpa'):
            data[key] = archive(Path(data[key]))
        report.write_text(json.dumps(data, indent=2) + '\n')
        casefile.unlink(missing_ok=True)
        if not data['complete'] and len(cases)>1:
            # Keep the aborted evidence and isolate the whole selection. Never
            # drop the unexecuted tail of a crashed or timed-out batch.
            attempts.append({'report': str(report), 'expected_cases': len(cases),
                             'executed_cases': data['executed_cases'], 'complete': False})
            failures.extend(f for f in data['failures'] if f['status'] != 'Incomplete')
            middle=len(cases)//2
            run_batch(cases[:middle], tag+'-a')
            run_batch(cases[middle:], tag+'-b')
            return
        scheduled+=len(cases)
        counts.update(data['counts'])
        covered += data['executed_cases']
        failures.extend(data['failures'])
        if not data['complete']:
            failures.append({'batch': batch_name, 'status': 'Incomplete',
                             'message': 'not every selected case produced a complete result'})
        batches.append({'report': str(report), 'first_case': cases[0], 'last_case': cases[-1],
                        'expected_cases': len(cases), 'executed_cases': data['executed_cases'],
                        'complete': data['complete'], 'passed': data['passed']})
        summarize()
        print(f'{covered}/{expected} cases; {dict(counts)}; {len(failures)} failures', flush=True)
    summarize()
    chunk, index = [], 0
    try:
        for case in runner.case_names(args.list):
            if args.group and not case.startswith(args.group):
                continue
            chunk.append(case)
            if len(chunk) == args.chunk_size:
                run_batch(chunk,str(index).zfill(5))
                index+=1
                chunk = []
        if chunk:
            run_batch(chunk,str(index).zfill(5))
    except (KeyboardInterrupt, RuntimeError) as error:
        print('Mustpass interrupted: ' + str(error), file=sys.stderr)
        summarize()
        return 1
    data = summarize(True)
    print('Report: ' + str(summary_path))
    return 0 if data['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
