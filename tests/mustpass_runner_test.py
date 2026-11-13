#!/usr/bin/env python3
"""Check that resumption retains aborted evidence and never drops its tail."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'mustpass', Path(__file__).resolve().parents[1] / 'scripts/run-mustpass.py')
mustpass = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mustpass)


class ResumeTests(unittest.TestCase):
    def test_incomplete_parent_reuses_children_and_retains_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            results = root / 'results'
            results.mkdir()
            inputs = root / 'inputs'
            inputs.mkdir()
            binary = root / 'build/cts/build/external/vulkancts/modules/vulkan/deqp-vk'
            binary.parent.mkdir(parents=True)
            paths = [inputs / 'libzvk.dylib', inputs / 'libvirglrenderer.1.dylib',
                     binary, root / 'loader']
            keys = ['driver_sha256', 'backend_sha256', 'binary_sha256', 'loader_sha256']
            for path in paths:
                path.write_bytes(path.name.encode())
            hashes = dict(zip(keys, map(mustpass.runner.digest, paths)))
            cases = ['dEQP-VK.first', 'dEQP-VK.second']
            selection = ''.join(case + '\n' for case in cases)
            casefile = root / 'cases.txt'
            casefile.write_text(selection)
            selection_hash = hashlib.sha256(selection.encode()).hexdigest()
            identity = hashlib.sha256(json.dumps([hashes, selection_hash, 2], sort_keys=True).encode()).hexdigest()
            name = 'mustpass-' + identity[:20]
            for tag, selected, complete in [('00000', cases, False),
                                             ('00000-a', cases[:1], True),
                                             ('00000-b', cases[1:], True)]:
                log, qpa = (results / (name + '-' + tag + ext) for ext in ('.log', '.qpa'))
                log.write_text('original log')
                qpa.write_text('original results')
                report = {**hashes, 'complete': complete, 'passed': complete,
                          'executed_cases': 1, 'counts': {'Pass': 1},
                          'caselist_sha256': hashlib.sha256(''.join(c + '\n' for c in selected).encode()).hexdigest(),
                          'failures': [] if complete else [{'case': cases[0], 'status': 'DeviceLost'}],
                          'log': str(log), 'qpa': str(qpa)}
                (results / (name + '-' + tag + '.json')).write_text(json.dumps(report))
            argv = ['run-mustpass.py', '--list', str(casefile), '--inputs', str(inputs), '--chunk-size', '2']
            with patch.object(mustpass, 'ROOT', root), patch.object(mustpass, 'RESULTS', results), \
                 patch.object(mustpass.runner, 'freeze_loader', return_value=paths[-1]), \
                 patch.object(mustpass.subprocess, 'run', side_effect=AssertionError('cached batch reran')), \
                 patch.object(sys, 'argv', argv):
                for _ in range(2):
                    self.assertEqual(mustpass.main(), 1)
                    summary = json.loads((results / (name + '.json')).read_text())
                    self.assertTrue(summary['complete'])
                    self.assertFalse(summary['passed'])
                    self.assertEqual(summary['executed_cases'], 2)
                    self.assertEqual(summary['failures'], [{'case': cases[0], 'status': 'DeviceLost'}])
                # A missing tail must execute once, without rerunning either
                # the aborted parent or its completed first child.
                tail = results / (name + '-00000-b.json')
                tail_data = tail.read_text()
                tail.unlink()
                def run_tail(command, **kwargs):
                    self.assertEqual(command[command.index('--name') + 1], name + '-00000-b')
                    tail.write_text(tail_data)
                with patch.object(mustpass.subprocess, 'run', side_effect=run_tail) as execute:
                    self.assertEqual(mustpass.main(), 1)
                    self.assertEqual(execute.call_count, 1)
                    summary = json.loads((results / (name + '.json')).read_text())
                    self.assertEqual(summary['executed_cases'], 2)

    def test_archiving_preserves_existing_attempt(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'case.log'
            previous = source.with_suffix('.log.gz')
            previous.write_bytes(gzip.compress(b'earlier attempt'))
            source.write_bytes(b'later attempt')
            archived = Path(mustpass.archive(source))
            self.assertNotEqual(archived, previous)
            self.assertEqual(gzip.decompress(previous.read_bytes()), b'earlier attempt')
            self.assertEqual(gzip.decompress(archived.read_bytes()), b'later attempt')


if __name__ == '__main__':
    unittest.main()
