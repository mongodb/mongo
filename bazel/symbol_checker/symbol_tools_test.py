"""Exercise the symbol tools' command-line and response-file interfaces."""

import json
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


class SymbolToolsTest(unittest.TestCase):
    def run_tool(self, name: str, arguments: list[str]) -> None:
        script = Path(__file__).with_name(name)
        with patch.object(sys, "argv", [str(script), *arguments]):
            try:
                runpy.run_path(str(script), run_name="__main__")
            except SystemExit as exc:
                if exc.code != 0:
                    raise

    def test_extractor_arguments(self) -> None:
        for response_file in (False, True):
            with self.subTest(response_file=response_file), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / "extracted symbols.json"
                objects = [
                    f"objects with spaces/{'x' * 100}/{i}.o"
                    for i in range(20000 if response_file else 1)
                ]
                arguments = ["--nm", "nm", "--out", str(output)]
                for obj in objects:
                    arguments.extend(["--obj", obj])
                if response_file:
                    params = Path(tmp) / "extract.params"
                    params.write_text("\n".join(arguments) + "\n")
                    self.assertGreater(params.stat().st_size, 2 * 1024 * 1024)
                    arguments = [f"@{params}"]

                def nm(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
                    symbols = (
                        "0000 T mongo::defined()\n"
                        if "--defined-only" in command
                        else "U mongo::needed()\n"
                    )
                    return subprocess.CompletedProcess(command, 0, stdout=symbols)

                with patch("subprocess.run", side_effect=nm) as mock_nm:
                    self.run_tool("symbol_extractor.py", arguments)
                self.assertEqual(mock_nm.call_count, 2 * len(objects))
                self.assertEqual(mock_nm.call_args.args[0][-1], objects[-1])
                self.assertEqual(
                    json.loads(output.read_text()),
                    {"defined": ["mongo::defined()"], "undefined": ["mongo::needed()"]},
                )

    def test_checker_arguments(self) -> None:
        for response_file in (False, True):
            with self.subTest(response_file=response_file), tempfile.TemporaryDirectory() as tmp:
                symbols = Path(tmp) / "current symbols.json"
                dependency = Path(tmp) / ("dependency with spaces " + "x" * 100 + ".json")
                output = Path(tmp) / "checked.json"
                symbols.write_text(json.dumps({"defined": [], "undefined": ["mongo::needed()"]}))
                dependency.write_text(json.dumps({"defined": ["mongo::needed()"]}))
                arguments = [
                    "--sym",
                    str(symbols),
                    "--out",
                    str(output),
                    "--label=@@//test:target",
                ]
                for _ in range(20000 if response_file else 1):
                    arguments.extend(["--dep", str(dependency)])
                if response_file:
                    params = Path(tmp) / "check.params"
                    params.write_text("\n".join(arguments) + "\n")
                    self.assertGreater(params.stat().st_size, 2 * 1024 * 1024)
                    arguments = [f"@{params}"]
                self.run_tool("symbol_checker.py", arguments)
                self.assertEqual(json.loads(output.read_text())["status"], "ok")
                self.assertEqual(json.loads(output.read_text())["target"], "//test:target")

                # Response-file support must preserve the checker's failure behavior.
                dependency.write_text(json.dumps({"defined": []}))
                with patch("sys.stderr"), self.assertRaises(SystemExit) as failure:
                    self.run_tool("symbol_checker.py", arguments)
                self.assertEqual(failure.exception.code, 1)
                self.assertEqual(json.loads(output.read_text())["missing"], ["mongo::needed()"])


if __name__ == "__main__":
    unittest.main()
