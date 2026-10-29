import subprocess
import unittest
from unittest import mock

import version_constants_gen


class ToolchainVersionTest(unittest.TestCase):
    def test_clang_cl_uses_same_executable_for_cxx(self):
        compiler_path = "tools/clang-cl"

        with mock.patch.object(
            version_constants_gen.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                args=compiler_path,
                returncode=0,
                stderr="clang version 22.1.1",
            ),
        ) as run:
            version = version_constants_gen.get_toolchain_ver("CXX", compiler_path, {})

        self.assertEqual(version, "tools/clang-cl: clang version 22.1.1")
        self.assertEqual(run.call_args.args[0], compiler_path)


class BuildInfoEnvironmentTest(unittest.TestCase):
    def test_uses_target_platform_metadata(self):
        extra_definitions = {
            "MONGO_DISTMOD": "test",
            "MONGO_DISTARCH": "x86_64",
            "TARGET_ARCH": "x86_64",
            "TARGET_OS": "windows",
            "compile_variables": "",
            "linkflags": "",
            "cpp_defines": "",
        }

        with (
            mock.patch.object(version_constants_gen.platform, "machine", return_value="aarch64"),
            mock.patch.object(version_constants_gen, "get_running_os_name", return_value="linux"),
            mock.patch.object(version_constants_gen, "get_toolchain_ver", return_value=""),
        ):
            data = version_constants_gen.default_buildinfo_environment_data(
                "tools/clang", extra_definitions, {}
            )

        self.assertEqual(data["distarch"]["value"], "x86_64")
        self.assertEqual(data["target_arch"]["value"], "x86_64")
        self.assertEqual(data["target_os"]["value"], "windows")


if __name__ == "__main__":
    unittest.main()
