import json
import os
import pathlib
import tempfile
import unittest
from unittest import mock

import mongo_config_header


class CompileCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        mongo_config_header.CompilerSettings.compiler_path = "compiler"
        mongo_config_header.CompilerSettings.compiler_type = "gcc"
        mongo_config_header.CompilerSettings.compiler_args = ""
        mongo_config_header.CompilerSettings.env_vars = {}

    def test_msvc_drivers_compile_cpp_on_linux(self) -> None:
        for compiler_type in ("msvc-cl", "clang-cl"):
            with (
                self.subTest(compiler_type=compiler_type),
                tempfile.TemporaryDirectory() as temp_dir,
            ):
                mongo_config_header.logfile_path = str(pathlib.Path(temp_dir) / "checks.log")
                mongo_config_header.CompilerSettings.compiler_type = compiler_type
                source = "int probe() { return 0; }"

                def run(command: list[str], **kwargs) -> mock.Mock:
                    self.assertIn("/c", command)
                    self.assertIn("/TP", command)
                    self.assertNotIn("-x", command)
                    source_path = next(pathlib.Path(arg) for arg in command if arg.endswith(".cpp"))
                    self.assertEqual(source_path.read_text(), source)
                    self.assertNotIn("input", kwargs)
                    return mock.Mock(returncode=0)

                with (
                    mock.patch.object(mongo_config_header.platform, "system", return_value="Linux"),
                    mock.patch.object(mongo_config_header.subprocess, "run", side_effect=run),
                ):
                    self.assertTrue(mongo_config_header.compile_check(source))

    def test_gnu_driver_compiles_cpp_from_stdin_on_windows(self) -> None:
        mongo_config_header.logfile_path = os.devnull
        with (
            mock.patch.object(mongo_config_header.platform, "system", return_value="Windows"),
            mock.patch.object(
                mongo_config_header.subprocess, "run", return_value=mock.Mock(returncode=0)
            ) as run,
        ):
            self.assertTrue(mongo_config_header.compile_check("int probe() { return 0; }"))
        self.assertIn("-x", run.call_args.args[0])
        self.assertEqual(run.call_args.kwargs["input"], "int probe() { return 0; }")

    def test_compiler_launch_error_is_not_treated_as_missing_feature(self) -> None:
        for system in ("Linux", "Windows"):
            with self.subTest(system=system), tempfile.TemporaryDirectory() as temp_dir:
                mongo_config_header.CompilerSettings.compiler_type = (
                    "msvc-cl" if system == "Windows" else "gcc"
                )
                mongo_config_header.logfile_path = str(pathlib.Path(temp_dir) / "checks.log")
                with (
                    mock.patch.object(mongo_config_header.platform, "system", return_value=system),
                    mock.patch.object(
                        mongo_config_header.subprocess,
                        "run",
                        side_effect=OSError("compiler not found"),
                    ),
                ):
                    with self.assertRaisesRegex(OSError, "compiler not found"):
                        mongo_config_header.compile_check("int main() { return 0; }")


class TargetOperatingSystemCheckTest(unittest.TestCase):
    def test_windows_target_skips_strnlen_on_linux_host(self) -> None:
        with (
            mock.patch.object(mongo_config_header.platform, "system", return_value="Linux"),
            mock.patch.object(
                mongo_config_header, "compile_check", return_value=True
            ) as compile_check,
        ):
            substitutions = mongo_config_header.generate_config_header(
                "clang-cl",
                "",
                "{}",
                os.devnull,
                extra_definitions=json.dumps({"TARGET_OS": "windows", "COMPILER_TYPE": "clang-cl"}),
            )
        self.assertEqual(
            substitutions["@mongo_config_have_strnlen@"], "// #undef MONGO_CONFIG_HAVE_STRNLEN"
        )
        self.assertFalse(any("strnlen" in call.args[0] for call in compile_check.call_args_list))
        self.assertEqual(mongo_config_header.CompilerSettings.compiler_type, "clang-cl")

    def test_macos_target_skips_sdt_on_linux_host(self) -> None:
        with (
            mock.patch.object(mongo_config_header.platform, "system", return_value="Linux"),
            mock.patch.object(
                mongo_config_header, "compile_check", return_value=True
            ) as compile_check,
        ):
            substitutions = mongo_config_header.generate_config_header(
                "clang",
                "",
                "{}",
                os.devnull,
                extra_definitions=json.dumps({"TARGET_OS": "macOS", "COMPILER_TYPE": "clang"}),
            )
        self.assertEqual(
            substitutions["@mongo_config_usdt_enabled@"], "// #undef MONGO_CONFIG_USDT_ENABLED"
        )
        self.assertFalse(any("sys/sdt.h" in call.args[0] for call in compile_check.call_args_list))


class TargetArchitectureCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        mongo_config_header.logfile_path = os.devnull

    def test_extended_alignment_uses_target_architecture(self) -> None:
        with mock.patch.object(
            mongo_config_header, "compile_check", side_effect=[False, True]
        ) as compile_check:
            definitions = mongo_config_header.extended_alignment_flag("ppc64le")

        self.assertEqual(definitions[0].value, 64)
        self.assertIn("alignas(128)", compile_check.call_args_list[0].args[0])
        self.assertIn("alignas(64)", compile_check.call_args_list[1].args[0])

    def test_altivec_uses_supported_little_endian_lane(self) -> None:
        with (
            mock.patch.object(mongo_config_header.platform, "machine", return_value="x86_64"),
            mock.patch.object(mongo_config_header, "compile_check") as compile_check,
        ):
            definitions = mongo_config_header.altivec_vbpermq_output_flag("ppc64le")

        compile_check.assert_not_called()
        self.assertEqual(definitions[0].value, 1)

    def test_altivec_check_skips_non_ppc_target(self) -> None:
        with (
            mock.patch.object(mongo_config_header.platform, "machine", return_value="ppc64le"),
            mock.patch.object(mongo_config_header, "compile_check") as compile_check,
        ):
            definitions = mongo_config_header.altivec_vbpermq_output_flag("s390x")

        compile_check.assert_not_called()
        self.assertEqual(definitions, [])

    def test_generate_config_header_preserves_numeric_zero(self) -> None:
        with mock.patch.object(mongo_config_header, "compile_check", return_value=False):
            substitutions = mongo_config_header.generate_config_header(
                "compiler",
                "",
                "{}",
                os.devnull,
                extra_definitions=json.dumps(
                    {
                        "TARGET_ARCH": "s390x",
                        "MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX": 0,
                    }
                ),
            )

        self.assertEqual(
            substitutions["@mongo_config_altivec_vec_vbpermq_output_index@"],
            "#define MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX 0",
        )


if __name__ == "__main__":
    unittest.main()
