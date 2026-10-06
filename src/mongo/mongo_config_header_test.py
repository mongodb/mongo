"""Configure checks must describe the target, even on a different host."""

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


if __name__ == "__main__":
    unittest.main()
