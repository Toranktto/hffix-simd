import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain
from conan.tools.files import copy


class Hffix2Conan(ConanFile):
    name = "hffix2"
    version = "1.0.0"
    license = "BSD-2-Clause"
    author = "Lukasz Derlatka"
    url = "https://github.com/Toranktto/hffix2"
    homepage = url
    description = (
        "Header-only, in-place, no-alloc FIX parser. Ships generated tag "
        "and group tables for FIX 5.0 SP2 + FIXT 1.1; wire-compatible with "
        "older FIX 4.x. Bundles the fixspec-gen-fields code generator as a "
        "host-arch binary so downstream consumers can regenerate tables "
        "from custom FIX XML specs via hffix_generate_fields()."
    )
    topics = ("fix", "fix-protocol", "trading", "parser", "header-only")

    settings = "os", "arch", "compiler", "build_type"

    options = {
        "with_docs": [True, False],
    }
    default_options = {
        "with_docs": False,
    }

    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "include/*",
        "utils/*",
        "fixspec/*",
        "docs/*",
        "LICENSE",
        "README.md",
    )

    def requirements(self):
        self.requires("pugixml/1.15")

    def build_requirements(self):
        self.test_requires("gtest/1.15.0")
        self.test_requires("benchmark/1.9.5")
        if self.options.with_docs:
            self.tool_requires("doxygen/1.17.0")

    def layout(self):
        self.folders.source = "."
        self.folders.build = "."
        self.folders.generators = "."

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["HFFIX_BUILD"] = "OFF"
        tc.cache_variables["HFFIX_BUILD_FIXSPEC_GEN"] = "ON"
        if self.options.with_docs:
            tc.cache_variables["HFFIX_BUILD_DOCS"] = "ON"
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(
            self,
            "LICENSE",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "licenses"),
        )

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "hffix2")
        self.cpp_info.set_property("cmake_target_name", "hffix2::hffix")
        self.cpp_info.set_property(
            "cmake_build_modules",
            [os.path.join("share", "hffix2", "cmake", "hffix_generate_fields.cmake")],
        )
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.bindirs = ["bin"]
        self.cpp_info.libdirs = []
