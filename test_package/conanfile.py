import os
from conan import ConanFile
from conans import CMake
from conan.tools.build import cross_building

class TestPackageConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "cmake", "cmake_find_package"

    def build(self):
        cmake = CMake(self)
        cmake.configure(defs={'CONAN_CMAKE_SILENT_OUTPUT': 'ON'})
        cmake.build()

    def test(self):
        if not cross_building(self):
            bin_path = os.path.join("bin", "test_package")
            self.run(f"{bin_path} -c -h", run_environment=True)
