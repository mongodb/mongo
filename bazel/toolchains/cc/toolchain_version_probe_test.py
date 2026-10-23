import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

from bazel.toolchains.cc import toolchain_version_probe as probe


class VersionProbeTest(unittest.TestCase):
    def test_clang_uses_dumpversion(self) -> None:
        with patch.object(probe, "run_tool", return_value="19.1.7\n") as run:
            self.assertEqual("19.1.7", probe.compiler_version("clang", "tools/clang++", {}))
        run.assert_called_once_with(["tools/clang++", "-dumpversion"], {})

    def test_gcc_uses_full_version(self) -> None:
        with patch.object(probe, "run_tool", return_value="14.2.0\n") as run:
            self.assertEqual("14.2.0", probe.compiler_version("gcc", "tools/g++", {}))
        run.assert_called_once_with(["tools/g++", "-dumpfullversion"], {})

    def test_rejects_incomplete_or_malformed_compiler_version(self) -> None:
        for version in ("19", "19.1", "", "19.1.7.2", "19.1.7junk"):
            with patch.object(probe, "run_tool", return_value=version):
                with self.assertRaisesRegex(RuntimeError, "compiler version"):
                    probe.compiler_version("clang", "clang", {})

    def test_parses_linker_banners(self) -> None:
        for banner, expected in (
            ("MongoDB LLD 19.1.7 (compatible with GNU linkers)", ("lld", "19.1.7")),
            ("LLD 22.1.1", ("lld", "22.1.1")),
            ("mold 2.40.4 (compatible with GNU ld)", ("mold", "2.40.4")),
            ("@(#)PROGRAM:ld PROJECT:ld-1115.7.3", ("apple", "1115.7.3")),
            ("GNU ld (GNU Binutils) 2.43.1", ("bfd", "2.43.1")),
            ("GNU gold (GNU Binutils 2.43.1) 1.16", ("gold", "1.16")),
            ("GNU gold (GNU Binutils for Ubuntu 2.38) 1.16\nCopyright 2022", ("gold", "1.16")),
        ):
            with self.subTest(banner=banner):
                self.assertEqual(expected, probe.parse_linker_version(banner))

    def test_driver_trace_selects_linker_instead_of_compiler_or_registered_ld(self) -> None:
        trace = '\n "/tools/clang++" "-cc1" "source.c"\n "/actual tools/ld.lld" "-o" "a.out"\n'
        self.assertEqual("/actual tools/ld.lld", probe.linker_from_trace(trace))

    def test_missing_linker_trace_fails(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "linker command"):
            probe.linker_from_trace('clang version 19.1.7\n"/tools/clang" "-cc1"')

    def test_clang_captures_standalone_and_cross_prefixed_gold(self) -> None:
        for linker in ("gold", "aarch64-linux-gnu-gold", "s390x-linux-gnu-gold"):
            with (
                self.subTest(linker=linker),
                patch.object(
                    probe,
                    "run_tool",
                    side_effect=[
                        f'\n "/tools/{linker}" "-o" "a.out"\n',
                        "GNU gold (GNU Binutils 2.43.1) 1.16",
                    ],
                ) as run,
            ):
                flags = [f"--ld-path=/tools/{linker}"]
                result = probe.probe_linker("tools/clang++", flags, {})
                self.assertEqual("gold", result["linker"])
                self.assertEqual("1.16", result["linker_version"])
                self.assertTrue(result["ld"].startswith(f"/tools/{linker}:"))
                self.assertEqual(flags[0], run.call_args_list[1].args[0][1])

    def test_gcc_probes_collect2_selected_linker(self) -> None:
        for linker, version, banner in (
            ("mold", "2.40.4", "mold 2.40.4 (compatible with GNU ld)"),
            ("gold", "1.16", "GNU gold (GNU Binutils 2.43.1) 1.16"),
            ("lld", "19.1.7", "MongoDB LLD 19.1.7 (compatible with GNU linkers)"),
        ):
            with (
                self.subTest(linker=linker),
                patch.object(
                    probe, "run_tool", return_value=banner + "\ncollect2 version 14.2.0"
                ) as run,
            ):
                flags = [f"-fuse-ld={linker}", "-Btools"]
                result = probe.probe_linker("tools/g++", flags, {}, "gcc")
                self.assertEqual(linker, result["linker"])
                self.assertEqual(version, result["linker_version"])
                self.assertIn(banner, result["ld"])
                self.assertIn("-Wl,--version", run.call_args.args[0])
                self.assertEqual(flags, run.call_args.args[0][1:3])
                self.assertNotIn("-###", run.call_args.args[0])

    def test_clang_captures_gold_and_mold_selected_by_driver(self) -> None:
        for linker, version, banner in (
            ("gold", "1.16", "GNU gold (GNU Binutils 2.43.1) 1.16"),
            ("mold", "2.40.4", "mold 2.40.4 (compatible with GNU ld)"),
        ):
            with (
                self.subTest(linker=linker),
                patch.object(
                    probe,
                    "run_tool",
                    side_effect=[f'\n "/tools/ld.{linker}" "-o" "a.out"\n', banner],
                ) as run,
            ):
                flags = [f"-fuse-ld={linker}", "-Btools"]
                result = probe.probe_linker("tools/clang++", flags, {})
                self.assertEqual(linker, result["linker"])
                self.assertEqual(version, result["linker_version"])
                self.assertTrue(result["ld"].startswith(f"/tools/ld.{linker}:"))
                self.assertEqual(flags, run.call_args_list[1].args[0][1:3])

    def test_clang_probes_driver_selected_linker_with_target_flags(self) -> None:
        flags = ["--target=s390x-linux-gnu", "-fuse-ld=lld", "--sysroot=target"]
        with patch.object(
            probe, "run_tool", side_effect=['\n "/tools/ld.lld" "-o" "a.out"\n', "LLD 19.1.7"]
        ) as run:
            result = probe.probe_linker("tools/clang++", flags, {})
        self.assertEqual("19.1.7", result["linker_version"])
        self.assertEqual(flags, run.call_args_list[0].args[0][1:4])
        self.assertEqual("tools/clang++", run.call_args_list[1].args[0][0])
        self.assertEqual(flags, run.call_args_list[1].args[0][1:4])
        self.assertIn("-Wl,--version", run.call_args_list[1].args[0])

    def test_macos_cross_probes_linker_through_container_driver(self) -> None:
        driver = "external/cross/wrappers/clang++"
        flags = ["--target=arm64-apple-darwin", "-fuse-ld=lld"]

        def run(arguments, environment):
            self.assertEqual(driver, arguments[0])
            self.assertTrue(Path(arguments[-1]).is_relative_to(Path.cwd()))
            self.assertTrue(Path(arguments[-1]).is_file())
            if "-###" in arguments:
                return '\n "/linux/tools/ld64.lld" "-o" "a.out"\n'
            self.assertIn("-Wl,--version", arguments)
            return "LLD 22.1.1"

        with patch.object(probe, "run_tool", side_effect=run):
            result = probe.probe_linker(driver, flags, {})
        self.assertEqual("22.1.1", result["linker_version"])

    def test_apple_system_linker_uses_its_version_option(self) -> None:
        with (
            patch.object(probe.os.sys, "platform", "darwin"),
            patch.object(
                probe,
                "run_tool",
                side_effect=['\n "/usr/bin/ld" "-o" "a.out"\n', "PROJECT:ld-1115.7.3"],
            ) as run,
        ):
            result = probe.probe_linker("clang++", [], {})
        self.assertEqual("apple", result["linker"])
        self.assertEqual(["/usr/bin/ld", "-v"], run.call_args_list[1].args[0])

    def test_version_query_does_not_require_linker_order_files(self) -> None:
        flags = [
            "-fuse-ld=lld",
            "--target=s390x-linux-gnu",
            "-Btools",
            "-Wl,--symbol-ordering-file=missing.orderfile",
            "-Xlinker",
            "--version-script=missing.map",
        ]
        with patch.object(probe, "run_tool", return_value="LLD 19.1.7") as run:
            probe.linker_banner("clang++", flags, {})
        self.assertEqual(flags[:3], run.call_args.args[0][1:4])
        self.assertFalse(any("missing" in flag for flag in run.call_args.args[0]))

    def test_probe_failure_is_not_reported_as_unknown(self) -> None:
        with patch.object(subprocess, "run", side_effect=OSError("Exec format error")):
            with self.assertRaisesRegex(RuntimeError, "Exec format error"):
                probe.run_tool(["foreign/clang", "-dumpversion"], {})


if __name__ == "__main__":
    unittest.main()
