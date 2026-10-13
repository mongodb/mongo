"""Configure checks must describe the target, even on a different host."""

import json
import os
import unittest
from unittest.mock import patch

from src.mongo import mongo_config_header as config


class ExtendedAlignmentTest(unittest.TestCase):
    def test_s390x_on_arm64_uses_target_alignment(self) -> None:
        with (
            patch.object(config.platform, "machine", return_value="aarch64"),
            patch.object(config, "log_check"),
            patch.object(config, "compile_check", return_value=True) as compile_check,
        ):
            definitions = config.extended_alignment_flag("s390x")
            self.assertEqual(definitions[0].value, 256)
            self.assertIn("alignas(256)", compile_check.call_args.args[0])

    def test_target_alignment_falls_back_when_unsupported(self) -> None:
        with (
            patch.object(config, "log_check"),
            patch.object(config, "compile_check", side_effect=[False, True]) as compile_check,
        ):
            definitions = config.extended_alignment_flag("s390x")
            self.assertEqual(definitions[0].value, 128)
            self.assertEqual(compile_check.call_count, 2)

    def test_native_host_default_is_preserved(self) -> None:
        with (
            patch.object(config.platform, "machine", return_value="aarch64"),
            patch.object(config, "log_check"),
            patch.object(config, "compile_check", return_value=True),
        ):
            self.assertEqual(config.extended_alignment_flag()[0].value, 64)


class AltivecTargetTest(unittest.TestCase):
    def test_ppc_on_arm64_uses_supported_output_lane(self) -> None:
        with (
            patch.object(config.platform, "machine", return_value="aarch64"),
            patch.object(config, "compile_check", return_value=False),
            patch.object(config, "log_check"),
        ):
            substitutions = config.generate_config_header(
                "compiler",
                "",
                "{}",
                os.devnull,
                extra_definitions=json.dumps({"TARGET_ARCH": "ppc64le"}),
            )
        self.assertEqual(
            substitutions["@mongo_config_altivec_vec_vbpermq_output_index@"],
            "#define MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX 1",
        )

    def test_non_ppc_target_on_ppc_host_skips_altivec(self) -> None:
        with (
            patch.object(config.platform, "machine", return_value="ppc64le"),
            patch.object(config, "compile_check", return_value=True),
            patch.object(config, "log_check"),
        ):
            substitutions = config.generate_config_header(
                "compiler",
                "",
                "{}",
                os.devnull,
                extra_definitions=json.dumps({"TARGET_ARCH": "s390x"}),
            )
        self.assertEqual(
            substitutions["@mongo_config_altivec_vec_vbpermq_output_index@"],
            "// #undef MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX",
        )

    def test_native_ppc_uses_same_supported_output_lane(self) -> None:
        with (
            patch.object(config.platform, "machine", return_value="ppc64le"),
            patch.object(config, "compile_check", return_value=True),
            patch.object(config, "log_check"),
        ):
            substitutions = config.generate_config_header(
                "compiler", "", "{}", os.devnull, extra_definitions="{}"
            )
        self.assertEqual(
            substitutions["@mongo_config_altivec_vec_vbpermq_output_index@"],
            "#define MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX 1",
        )


if __name__ == "__main__":
    unittest.main()
