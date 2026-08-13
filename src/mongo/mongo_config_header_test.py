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


if __name__ == "__main__":
    unittest.main()
