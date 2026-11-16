#!/usr/bin/env python3
"""Build relocated consumers and check that installation preserves unrelated headers."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from check_installed_headers import check

SOURCE = Path(__file__).resolve().parents[1]


def run(*args):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180)
    if result.returncode:
        raise AssertionError(f"Command failed: {args!r}\n{result.stdout}")


class InstallationTests(unittest.TestCase):
    def verify(self, includedir):
        with tempfile.TemporaryDirectory(prefix="parlay-install-") as directory:
            work = Path(directory)
            prefix = work / "prefix"
            headers = prefix / includedir
            # An existing header models ownership by another installation sharing
            # the prefix. Installing Parlay must leave it untouched.
            sentinel = headers / "unrelated" / "sentinel.h"
            sentinel.parent.mkdir(parents=True)
            sentinel.write_text('#error "Unrelated header selected by a Parlay consumer"\n')
            digest = hashlib.sha256(sentinel.read_bytes()).digest()
            run("cmake", "-S", str(SOURCE), "-B", str(work / "build"),
                f"-DCMAKE_INSTALL_PREFIX={prefix}", f"-DCMAKE_INSTALL_INCLUDEDIR={includedir}")
            run("cmake", "--install", str(work / "build"))
            check(headers, SOURCE / "include")
            self.assertEqual({path.name for path in headers.iterdir()}, {"parlay", "unrelated"})
            self.assertEqual(hashlib.sha256(sentinel.read_bytes()).digest(), digest)
            if includedir != "include":
                self.assertFalse((prefix / "include").exists())
            relocated = work / "relocated"
            shutil.move(str(prefix), str(relocated))
            run("cmake", "-S", str(SOURCE / "test_package"), "-B", str(work / "consumer"),
                f"-DCMAKE_PREFIX_PATH={relocated}", "-DCMAKE_CXX_STANDARD=23")
            run("cmake", "--build", str(work / "consumer"), "--parallel", "2")
            executable = work / "consumer" / "consumer"
            if not executable.exists():
                executable = work / "consumer" / "consumer.exe"
            run(str(executable))

    def test_default_header_directory(self):
        self.verify("include")

    def test_custom_header_directory(self):
        self.verify("headers/custom")


if __name__ == "__main__":
    unittest.main()
