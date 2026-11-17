"""Exercise Cargo manifest inputs and execroot cleanup through the real runner."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

RUNNER = Path(sys.argv.pop(1)).resolve()


class CargoRunnerExecrootTest(unittest.TestCase):
    def test_overlapping_inputs_survive_success_and_failure(self) -> None:
        for copy_runfiles in (False, True):
            for script_status in (0, 7):
                with self.subTest(copy_runfiles=copy_runfiles, script_status=script_status):
                    with tempfile.TemporaryDirectory() as directory:
                        root = Path(directory)
                        (root / "src").mkdir()
                        (root / "src/lib.rs").write_text("source input")
                        (root / "Cargo.toml").write_text("manifest input")
                        rustc = root / "rustc"
                        rustc.write_text("#!/bin/sh\nprintf 'target_os=\"windows\"\\n'\n")
                        rustc.chmod(0o755)
                        script = root / "script"
                        script.write_text(
                            "#!/bin/sh\n"
                            "test -f src/lib.rs && test -f Cargo.toml || exit 99\n"
                            'echo "fixture build script" >&2\n'
                            f"exit {script_status}\n"
                        )
                        script.chmod(0o755)
                        (root / "manifest.params").write_text(
                            "output/cargo_runfiles\nlib.rs,Cargo.toml\n"
                            "src/lib.rs=repo/src/lib.rs\nCargo.toml=repo/Cargo.toml\n"
                        )
                        result = subprocess.run(
                            [
                                str(RUNNER),
                                "--script=script",
                                "--links=",
                                "--out_dir=output/out",
                                "--env_out=output/env",
                                "--flags_out=output/flags",
                                "--link_flags=output/linkflags",
                                "--link_search_paths=output/linkpaths",
                                "--dep_env_out=output/depenv",
                                "--rundir=",
                                "--cargo_manifest_args=@manifest.params",
                            ],
                            cwd=root,
                            env={
                                **os.environ,
                                "CARGO_MANIFEST_DIR": "output/cargo_runfiles/repo",
                                "RUSTC": "./rustc",
                                "TARGET": "x86_64-pc-windows-msvc",
                                "RULES_RUST_CARGO_MANIFEST_COPY": str(int(copy_runfiles)),
                                "RULES_RUST_SYMLINK_EXEC_ROOT": "1",
                            },
                            capture_output=True,
                            text=True,
                            timeout=10,
                            check=False,
                        )
                        self.assertEqual(
                            result.returncode, 1 if script_status else 0, result.stderr
                        )
                        if script_status:
                            self.assertIn(
                                "Build script process failed with exit code 7", result.stderr
                            )
                            self.assertIn("fixture build script", result.stderr)
                        self.assertNotIn("Failed to remove", result.stderr)
                        manifest = root / "output/cargo_runfiles/repo"
                        self.assertEqual((manifest / "src/lib.rs").read_text(), "source input")
                        self.assertEqual((manifest / "Cargo.toml").read_text(), "manifest input")
                        self.assertFalse((manifest / "rustc").is_symlink())
                        self.assertFalse((manifest / "script").is_symlink())
                        self.assertEqual((root / "src/lib.rs").read_text(), "source input")
                        self.assertEqual((root / "Cargo.toml").read_text(), "manifest input")


if __name__ == "__main__":
    unittest.main()
