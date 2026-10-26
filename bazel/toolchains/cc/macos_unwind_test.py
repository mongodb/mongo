"""Cross-link the empty-section unwind regression without a macOS SDK."""

import ast
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS: list[str] = []


def macos_link_flags() -> list[str]:
    module = ast.parse(Path(__file__).with_name("macos_link_flags.bzl").read_text())
    return ast.literal_eval(
        next(node.value for node in module.body if isinstance(node, ast.Assign))
    )


class MacosUnwindTest(unittest.TestCase):
    def test_empty_text_does_not_shadow_function_unwind_info(self) -> None:
        if not TOOLS:
            self.skipTest("requires clang, ld64.lld, and llvm-objdump arguments")
        # Preserve ld64.lld's basename: the lld multi-call binary selects its
        # Mach-O flavor from argv[0], including when the tool is a symlink.
        clang, linker, objdump = [str(Path(tool).absolute()) for tool in TOOLS]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # Data-only translation units still have an empty __text section
            # and local label. The exported data must survive dead stripping.
            (root / "empty.s").write_text(
                ".section __TEXT,__text,regular,pure_instructions\n"
                "ltmp0:\n"
                ".section __TEXT,__cstring,cstring_literals\n"
                '.globl _message\n_message:\n.asciz "data only"\n'
                ".subsections_via_symbols\n"
            )
            (root / "function.s").write_text(
                ".section __TEXT,__text,regular,pure_instructions\n"
                ".globl _foo\n_foo:\nret\nLend:\n"
                ".section __TEXT,__gcc_except_tab\nLexception:\n.byte 0xff\n"
                ".section __LD,__compact_unwind,regular,debug\n"
                ".quad _foo\n.long Lend-_foo\n.long 0x04000000\n"
                ".quad ___gxx_personality_v0\n.quad Lexception\n"
                ".subsections_via_symbols\n"
            )
            for name in ("empty", "function"):
                subprocess.run(
                    [clang, "-target", "arm64-apple-macos14", "-c", f"{name}.s", "-o", f"{name}.o"],
                    cwd=root,
                    capture_output=True,
                    text=True,
                    check=True,
                )
            # Pass the production flags through Clang, as real Bazel links do.
            subprocess.run(
                [
                    clang,
                    "-target",
                    "arm64-apple-macos14",
                    f"--ld-path={linker}",
                    "-nostdlib",
                    "-shared",
                    "-Wl,-undefined,dynamic_lookup",
                    "-Wl,-platform_version,macos,14.0,14.0",
                    *macos_link_flags(),
                    "empty.o",
                    "function.o",
                    "-o",
                    "out.dylib",
                ],
                cwd=root,
                capture_output=True,
                text=True,
                check=True,
            )
            output = subprocess.check_output(
                [objdump, "--unwind-info", root / "out.dylib"], text=True
            )
            entries = re.findall(
                r"function offset=(0x[0-9a-f]+), encoding\[\d+\]=(0x[0-9a-f]+)", output
            )
            self.assertEqual(1, len(entries), output)
            self.assertEqual("0x14000000", entries[0][1], output)
            symbols = subprocess.check_output([objdump, "--syms", root / "out.dylib"], text=True)
            self.assertIn("_message", symbols)
            self.assertIn("_foo", symbols)


if __name__ == "__main__":
    TOOLS = sys.argv[1:]
    unittest.main(argv=[sys.argv[0]])
