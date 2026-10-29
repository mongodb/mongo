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
        mongo_config_header.CompilerSettings.compiler_args = ""
        mongo_config_header.CompilerSettings.env_vars = {}

    def test_compiler_launch_error_is_not_treated_as_missing_feature(self) -> None:
        for system in ("Linux", "Windows"):
            with self.subTest(system=system), tempfile.TemporaryDirectory() as temp_dir:
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
