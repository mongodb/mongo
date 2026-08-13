from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).with_name("record_mongodb_server_version.sh")


class RecordMongoDBServerVersionTest(unittest.TestCase):
    def test_failure_reports_binary_diagnostics_and_preserves_exit_code(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            evergreen_dir = root / "src" / "evergreen"
            evergreen_dir.mkdir(parents=True)
            script = evergreen_dir / SCRIPT.name
            shutil.copy2(SCRIPT, script)
            (evergreen_dir / "prelude.sh").write_text("", encoding="utf-8")
            binary = root / "src" / "mongod"
            output = root / "src" / "version.txt"
            binary.write_text(
                "#!/bin/sh\n"
                'echo "startup rejected test binary"\n'
                'echo "loader rejected test binary" >&2\n'
                "exit 23\n",
                encoding="utf-8",
            )
            binary.chmod(0o755)

            result = subprocess.run(
                ["bash", str(script), str(binary), str(output)],
                cwd=root,
                check=False,
                capture_output=True,
                text=True,
            )
            output_text = output.read_text(encoding="utf-8")

        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertIn("startup rejected test binary", result.stderr)
        self.assertIn("loader rejected test binary", result.stderr)
        self.assertIn("MongoDB server version command failed", result.stderr)
        self.assertIn(str(binary), result.stderr)
        self.assertEqual(output_text, "startup rejected test binary\n")


if __name__ == "__main__":
    unittest.main()
