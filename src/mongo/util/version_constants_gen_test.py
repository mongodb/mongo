import unittest
from unittest import mock

import version_constants_gen


class BuildInfoEnvironmentTest(unittest.TestCase):
    def setUp(self) -> None:
        self.extra_definitions = {
            "MONGO_DISTMOD": "test",
            "compile_variables": "",
            "linkflags": "",
            "cpp_defines": "",
        }
        self.machine = mock.patch.object(
            version_constants_gen.platform, "machine", return_value="aarch64"
        )
        self.toolchain_version = mock.patch.object(
            version_constants_gen, "get_toolchain_ver", return_value=""
        )
        self.machine.start()
        self.toolchain_version.start()
        self.addCleanup(self.machine.stop)
        self.addCleanup(self.toolchain_version.stop)

    def build_info(self, **definitions: str) -> dict[str, dict[str, str | bool]]:
        return version_constants_gen.default_buildinfo_environment_data(
            "tools/clang", self.extra_definitions | definitions, {}
        )

    def test_cross_build_uses_target_architecture(self) -> None:
        data = self.build_info(TARGET_ARCH="s390x")

        self.assertEqual(data["distarch"]["value"], "s390x")
        self.assertEqual(data["target_arch"]["value"], "s390x")

    def test_canonical_target_architectures(self) -> None:
        for architecture in ["x86_64", "aarch64", "ppc64le", "s390x", "arm64", "amd64", "wasm32"]:
            with self.subTest(architecture=architecture):
                data = self.build_info(TARGET_ARCH=architecture.upper())

                self.assertEqual(data["distarch"]["value"], architecture)
                self.assertEqual(data["target_arch"]["value"], architecture)

    def test_native_build_uses_host_architecture(self) -> None:
        data = self.build_info()

        self.assertEqual(data["distarch"]["value"], "aarch64")
        self.assertEqual(data["target_arch"]["value"], "aarch64")

    def test_empty_metadata_uses_host_architecture(self) -> None:
        data = self.build_info(TARGET_ARCH="")

        self.assertEqual(data["distarch"]["value"], "aarch64")
        self.assertEqual(data["target_arch"]["value"], "aarch64")


if __name__ == "__main__":
    unittest.main()
