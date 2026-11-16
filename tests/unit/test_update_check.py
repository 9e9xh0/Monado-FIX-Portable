"""GitHub #119: compile and run tests/unit/test_update_check.cpp (the launcher's update check logic)."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests/unit/test_update_check.cpp"


def find_compiler():
    for name in (os.environ.get("CXX"), "c++", "clang++", "g++"):
        if name and shutil.which(name):
            return [shutil.which(name)]
    try:
        import ziglang  # noqa: F401  (the toolchain the Windows build uses)
        import sys
        return [sys.executable, "-m", "ziglang", "c++"]
    except ImportError:
        return None


class UpdateCheck(unittest.TestCase):
    def test_update_check(self):
        cxx = find_compiler()
        if not cxx:
            self.skipTest("no C++ compiler")
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / ("t.exe" if os.name == "nt" else "t")
            build = subprocess.run(cxx + ["-std=c++17", str(SOURCE), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
