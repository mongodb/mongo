"""Run the launcher's Bash discovery functions with real PowerShell and file fixtures."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


def powershell_binary() -> str | None:
    if configured := os.environ.get("POWERSHELL_TEST_BINARY"):
        return configured
    if os.name == "nt":
        return str(
            Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        )
    return shutil.which("pwsh")


POWERSHELL = powershell_binary()


@unittest.skipUnless(POWERSHELL, "requires Windows PowerShell or pwsh")
class WindowsBashSelectionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.script = self.root / "select.ps1"
        self.script.write_text(
            "$ErrorActionPreference = 'Stop'\n"
            "$tokens = $null; $errors = $null\n"
            "$ast = [Management.Automation.Language.Parser]::ParseFile(\n"
            "    $env:BASH_SELECTION_SOURCE, [ref]$tokens, [ref]$errors)\n"
            "if ($errors.Count) { throw ($errors | Out-String) }\n"
            "$functions = $ast.FindAll({ param($node)\n"
            "    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and\n"
            "    $node.Name -in @('Resolve-NativeWindowsBash', 'Find-NativeWindowsBash')\n"
            "}, $false)\n"
            "foreach ($function in $functions) { Invoke-Expression $function.Extent.Text }\n"
            "Find-NativeWindowsBash\n",
            encoding="utf-8",
        )
        self.environment = {
            **os.environ,
            "BASH_SELECTION_SOURCE": str(Path(__file__).with_name("windows_test_launcher.ps1")),
            "MONGO_BAZEL_BASH": "",
            "SystemDrive": str(self.root / "drive"),
            "ProgramFiles": str(self.root / "program files"),
            "ProgramFiles(x86)": str(self.root / "program files x86"),
            "LOCALAPPDATA": str(self.root / "local app data"),
            "XDG_DATA_HOME": str(self.root / "pwsh-data"),
            "XDG_CACHE_HOME": str(self.root / "pwsh-cache"),
            "XDG_CONFIG_HOME": str(self.root / "pwsh-config"),
        }

    def _bash(self, directory: Path, runtime: str = "cygwin1.dll") -> Path:
        directory.mkdir(parents=True, exist_ok=True)
        bash = directory / "bash.exe"
        bash.touch()
        (directory / runtime).touch()
        return bash

    def _select(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [POWERSHELL, "-NoLogo", "-NoProfile", "-NonInteractive", "-File", str(self.script)],
            env=self.environment,
            cwd=self.root,
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )

    def _assert_selected(self, expected: Path) -> None:
        result = self._select()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(Path(result.stdout.strip()), expected)

    def test_discovers_standard_cygwin_installation(self) -> None:
        bash = self._bash(Path(self.environment["SystemDrive"]) / "cygwin64/bin")
        self._assert_selected(bash)

    def test_discovers_git_bash_without_cygwin(self) -> None:
        roots = [
            Path(self.environment[name])
            for name in ("ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA")
        ]
        roots.append(Path(self.environment["LOCALAPPDATA"]) / "Programs")
        for root in roots:
            with self.subTest(root=root):
                directory = root / "Git/usr/bin"
                bash = self._bash(directory, "msys-2.0.dll")
                self._assert_selected(bash)
                shutil.rmtree(directory.parent.parent)

    def test_explicit_custom_installation_takes_priority(self) -> None:
        self._bash(Path(self.environment["SystemDrive"]) / "cygwin64/bin")
        bash = self._bash(self.root / "custom cygwin/bin")
        self.environment["MONGO_BAZEL_BASH"] = f'"{bash}"'
        self._assert_selected(bash)

    def test_git_bin_entry_point_finds_usr_bin_runtime(self) -> None:
        directory = self.root / "custom git/bin"
        directory.mkdir(parents=True)
        bash = directory / "bash.exe"
        bash.touch()
        runtime = directory.parent / "usr/bin/msys-2.0.dll"
        runtime.parent.mkdir(parents=True)
        runtime.touch()
        self.environment["MONGO_BAZEL_BASH"] = str(bash)
        self._assert_selected(bash)

    def test_ignores_relative_malformed_and_unrelated_path_entries(self) -> None:
        self._bash(self.root)
        unrelated = self._bash(self.root / "unrelated/bin")
        bash = self._bash(Path(self.environment["ProgramFiles"]) / "Git/usr/bin", "msys-2.0.dll")
        self.environment["PATH"] = os.pathsep.join(
            [".", '"invalid<path|"', str(unrelated.parent), os.environ.get("PATH", "")]
        )
        self._assert_selected(bash)

    def test_rejects_wsl_and_relative_override(self) -> None:
        wsl = self.root / "wsl/bash.exe"
        wsl.parent.mkdir()
        wsl.touch()
        self._bash(self.root)
        candidates = [str(wsl), "bash.exe", '"invalid<path|"']
        if os.name == "nt":
            candidates.extend([str(self.root / "bash.exe")[2:], self.root.drive + "bash.exe"])
        for candidate in candidates:
            with self.subTest(candidate=candidate):
                self.environment["MONGO_BAZEL_BASH"] = candidate
                result = self._select()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("MONGO_BAZEL_BASH must name an absolute native", result.stderr)


if __name__ == "__main__":
    # Evergreen's unit task supplies C++ reporter arguments to Python tests too.
    unittest.main(argv=[sys.argv[0]])
