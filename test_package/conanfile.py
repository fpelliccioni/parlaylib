from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout
from conan.tools.build import can_run
import os
from pathlib import Path


class Consumer(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeDeps", "CMakeToolchain"
    test_type = "explicit"

    def requirements(self):
        self.requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def build(self):
        dependency = self.dependencies["parlaylib"]
        root = Path(dependency.package_folder) / "include"
        # The package owns exactly one subtree of the common include root.
        if {path.name for path in root.iterdir()} != {"parlay"}:
            raise RuntimeError("Package adds unexpected entries to the include root")
        for header in ("parallel.h", "primitives.h", "sequence.h", "internal/work_stealing_job.h"):
            if not (root / "parlay" / header).is_file():
                raise RuntimeError(f"Missing required header: parlay/{header}")
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        if can_run(self):
            self.run(os.path.join(self.cpp.build.bindir, "consumer"), env="conanrun")
