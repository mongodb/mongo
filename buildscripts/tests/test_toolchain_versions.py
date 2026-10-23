import unittest

from buildscripts.package_test.toolchain_versions import (
    should_require_production_toolchain,
    validate_toolchain_versions,
)


class ToolchainVersionsTest(unittest.TestCase):
    def test_release_requirement_includes_explicit_patch_profiles(self) -> None:
        for config in ("public-release", "public-release-local", "public-release-rbe"):
            for form in ("--config=" + config, "--config " + config):
                with self.subTest(form=form):
                    self.assertTrue(
                        should_require_production_toolchain(
                            {"is_patch": "true", "patch_compile_flags": form}
                        )
                    )
        release = {"project": "mongodb-mongo-v9.0", "push_bucket": "downloads.10gen.com"}
        self.assertTrue(should_require_production_toolchain(release))
        self.assertFalse(should_require_production_toolchain(release | {"is_patch": "true"}))
        self.assertFalse(
            should_require_production_toolchain(release | {"compiling_for_test": "true"})
        )
        self.assertTrue(should_require_production_toolchain({"MONGO_VERSION_OVERRIDE": "9.0.0"}))
        self.assertTrue(should_require_production_toolchain({"release_rbe": "true"}))

    def test_shipped_policy_contains_exact_versions_for_every_entry(self):
        import json
        from pathlib import Path

        from buildscripts.package_test import toolchain_versions

        policy = json.loads(Path(toolchain_versions.__file__).with_suffix(".json").read_text())
        for platform, versions in policy.items():
            os_name, arch = platform.split("/")
            self.assertEqual(
                {"compiler", "compiler_version", "linker", "linker_version"}, set(versions)
            )
            for key in ("compiler_version", "linker_version"):
                self.assertRegex(versions[key], r"^\d+\.\d+\.\d+$")
            for mode in ("native", "cross"):
                validate_toolchain_versions(
                    versions | {"target_os": os_name, "target_arch": arch, "toolchain_mode": mode}
                )

    def setUp(self) -> None:
        self.environment = {
            "target_os": "linux",
            "target_arch": "aarch64",
            "toolchain_mode": "cross",
            "compiler": "clang",
            "compiler_version": "19.1.7",
            "linker": "lld",
            "linker_version": "19.1.7",
        }
        self.pins = {
            "linux/aarch64": {
                key: self.environment[key]
                for key in ("compiler", "compiler_version", "linker", "linker_version")
            }
        }

    def test_accepts_exact_cross_versions(self) -> None:
        validate_toolchain_versions(self.environment, self.pins)

    def test_native_and_cross_builds_use_the_same_pin(self) -> None:
        for mode in ("native", "cross", None):
            with self.subTest(mode=mode):
                validate_toolchain_versions(self.environment | {"toolchain_mode": mode}, self.pins)

    def test_accepts_gcc_with_gold_or_mold_when_pinned(self) -> None:
        for linker, version in (("gold", "1.16"), ("mold", "2.40.4")):
            with self.subTest(linker=linker):
                tools = {
                    "compiler": "gcc",
                    "compiler_version": "14.2.0",
                    "linker": linker,
                    "linker_version": version,
                }
                validate_toolchain_versions(self.environment | tools, {"linux/aarch64": tools})
                with self.assertRaisesRegex(RuntimeError, "linker_version"):
                    validate_toolchain_versions(
                        self.environment | tools | {"linker_version": "unknown"},
                        {"linux/aarch64": tools},
                    )

    def test_rejects_missing_unknown_or_mismatched_versions(self) -> None:
        for key in ("compiler", "compiler_version", "linker", "linker_version"):
            for value in (None, "unknown", "22.1.1", "19.1.7git"):
                with self.subTest(key=key, value=value):
                    environment = self.environment | {key: value}
                    with self.assertRaisesRegex(RuntimeError, key):
                        validate_toolchain_versions(environment, self.pins)

    def test_rejects_unmapped_platforms(self) -> None:
        for key, value in (("target_arch", "s390x"), ("target_os", "macOS")):
            with self.assertRaisesRegex(RuntimeError, "No production toolchain"):
                validate_toolchain_versions(self.environment | {key: value}, self.pins)


if __name__ == "__main__":
    unittest.main()
