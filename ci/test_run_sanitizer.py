"""Exercise sanitizer mode selection and failure handling without compiling the library."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(__file__).resolve().with_name('run-sanitizer.sh')


class RunnerTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        tools = self.root / 'bin'
        tools.mkdir()
        self.log = self.root / 'calls.jsonl'
        program = '''import json, os, sys
tool = os.path.basename(sys.argv[0])
phase = 'test' if tool == 'ctest' else ('build' if '--build' in sys.argv else 'configure')
with open(os.environ['FIXTURE_LOG'], 'a') as log:
    log.write(json.dumps({'phase': phase, 'args': sys.argv[1:]}) + '\\n')
if os.environ.get('FAIL_PHASE') == phase: sys.exit(3)
'''
        for tool in ('cmake', 'ctest'):
            executable = tools / tool
            executable.write_text('#!' + sys.executable + '\n' + program)
            executable.chmod(0o755)
        self.libcxx = self.root / 'libcxx'
        (self.libcxx / 'include' / 'c++' / 'v1').mkdir(parents=True)
        (self.libcxx / 'lib').mkdir()
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                        FIXTURE_LOG=str(self.log))
        for variable in ('MSAN_LIBCXX_ROOT', 'ASAN_OPTIONS', 'TSAN_OPTIONS', 'UBSAN_OPTIONS'):
            self.env.pop(variable, None)

    def run_mode(self, *arguments, **overrides):
        if self.log.exists():
            self.log.unlink()
        result = subprocess.run(['bash', str(RUNNER), *arguments], cwd=self.root,
                                env=dict(self.env, **overrides),
                                capture_output=True, text=True, timeout=30)
        calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result, calls

    def test_dtest_flavours_build_and_run_only_their_instrumented_tests(self):
        for mode, option, flavour in (('address', 'ASAN', 'asan'), ('undefined', 'UBSAN', 'ubsan'),
                                      ('thread', 'TSAN', 'tsan')):
            with self.subTest(mode=mode):
                result, calls = self.run_mode(mode, 'out')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([c['phase'] for c in calls], ['configure', 'build', 'test'])
                configure, test = calls[0]['args'], calls[2]['args']
                self.assertIn(f'-DBUILD_{option}_TESTS=On', configure)
                self.assertIn('-DBUILD_ONLY_SANITIZED=On', configure)
                self.assertIn('-DCMAKE_CXX_STANDARD=23', configure)
                self.assertFalse(any(a.startswith('-DCMAKE_CXX_FLAGS') for a in configure))
                self.assertEqual(test[test.index('-R') + 1], f'[-]{flavour}$')
                self.assertIn('--no-tests=error', test)
                self.assertNotIn('-E', test)

    def test_leak_instruments_the_plain_flavour(self):
        result, calls = self.run_mode('leak', 'out')
        self.assertEqual(result.returncode, 0, result.stderr)
        configure, test = calls[0]['args'], calls[2]['args']
        self.assertIn('-DCMAKE_CXX_FLAGS=-fsanitize=leak -fno-omit-frame-pointer', configure)
        self.assertFalse(any('BUILD_ONLY_SANITIZED' in a for a in configure))
        self.assertEqual(test[test.index('-R') + 1], '[-]nosan$')
        self.assertEqual(test[test.index('-E') + 1], r'^TestUninitializedMemory\.')

    def test_memory_uses_the_instrumented_standard_library(self):
        result, calls = self.run_mode('memory', 'out', MSAN_LIBCXX_ROOT=str(self.libcxx))
        self.assertEqual(result.returncode, 0, result.stderr)
        configure, test = calls[0]['args'], calls[2]['args']
        flags = next(a for a in configure if a.startswith('-DCMAKE_CXX_FLAGS='))
        linker = next(a for a in configure if a.startswith('-DCMAKE_EXE_LINKER_FLAGS='))
        self.assertIn('-fsanitize=memory', flags)
        self.assertIn('-fno-sanitize-recover=all', flags)
        self.assertIn(f'-nostdinc++ -isystem {self.libcxx}/include/c++/v1', flags)
        self.assertIn(f'-L{self.libcxx}/lib', linker)
        self.assertIn(f'-Wl,-rpath,{self.libcxx}/lib', linker)
        self.assertEqual(test[test.index('-R') + 1], '[-]nosan$')
        self.assertEqual(test[test.index('-E') + 1], r'^TestUninitializedMemory\.')

    def test_memory_is_never_run_without_an_instrumented_standard_library(self):
        for overrides in ({}, {'MSAN_LIBCXX_ROOT': str(self.root / 'missing')}):
            with self.subTest(overrides=overrides):
                result, calls = self.run_mode('memory', 'out', **overrides)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('MSAN_LIBCXX_ROOT', result.stderr)
                self.assertEqual(calls, [])

    def test_unknown_or_missing_mode_is_rejected(self):
        for arguments in ((), ('none',), ('address,thread',)):
            with self.subTest(arguments=arguments):
                result, calls = self.run_mode(*arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Usage: run-sanitizer.sh', result.stderr)
                self.assertEqual(calls, [])

    def test_phase_failures_stop_the_run(self):
        for phase, expected in (('configure', ['configure']), ('build', ['configure', 'build']),
                                ('test', ['configure', 'build', 'test'])):
            with self.subTest(phase=phase):
                result, calls = self.run_mode('thread', 'out', FAIL_PHASE=phase)
                self.assertEqual(result.returncode, 3)
                self.assertEqual([c['phase'] for c in calls], expected)


if __name__ == '__main__':
    unittest.main()
