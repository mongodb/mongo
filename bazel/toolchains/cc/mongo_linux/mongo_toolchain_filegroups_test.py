"""Check action input closures against representative native and cross archives."""

import unittest
from pathlib import Path, PurePosixPath
from types import SimpleNamespace
from typing import Any


def bazel_glob_match(path: str, pattern: str) -> bool:
    """Match Bazel wildcards without interpreting Python's ? or bracket classes."""
    pattern = pattern.replace("[", "[[]").replace("?", "[?]")
    return PurePosixPath(path).full_match(pattern)


def filegroups(
    files: set[str],
    support_files: list[str] | None = None,
    host_runtime_files: list[str] | None = None,
) -> dict[str, set[str]]:
    """Evaluate the filegroup macro with a small archive instead of downloading one."""
    groups: dict[str, list[str]] = {}

    def glob(patterns: list[str]) -> list[str]:
        return sorted(path for path in files if any(bazel_glob_match(path, p) for p in patterns))

    namespace: dict[str, Any] = {
        "native": SimpleNamespace(
            glob=glob, filegroup=lambda name, srcs: groups.update({name: srcs})
        ),
    }
    source = Path(__file__).with_name("mongo_toolchain_filegroups.bzl")
    exec(compile(source.read_text(), str(source), "exec"), namespace)
    namespace["mongo_toolchain_filegroups"](
        "v5", support_files=support_files or [], host_runtime_files=host_runtime_files or []
    )

    def expand(name: str) -> set[str]:
        result: set[str] = set()
        for path in groups[name]:
            result.update(expand(path[1:]) if path.startswith(":") else [path])
        return result

    return {name: expand(name) for name in groups}


class ToolchainFilegroupsTest(unittest.TestCase):
    def test_glob_matches_bazel_wildcards_and_literal_metacharacters(self) -> None:
        self.assertFalse(bazel_glob_match("clang-19", "clang-[0-9]*"))
        self.assertTrue(bazel_glob_match("clang-[0-9]suffix", "clang-[0-9]*"))
        self.assertFalse(bazel_glob_match("clang-x", "clang-?"))
        self.assertTrue(bazel_glob_match("clang-?", "clang-?"))
        self.assertFalse(bazel_glob_match("nested/clang-19", "*"))
        self.assertTrue(bazel_glob_match("clang-19", "**/clang-1*"))
        self.assertTrue(bazel_glob_match("nested/bin/clang-19", "**/clang-1*"))

    def test_versioned_clang_binaries_are_compile_and_link_inputs(self) -> None:
        binaries = {
            f"{directory}/clang-{version}{suffix}"
            for directory in ("v5/bin", "stow/llvm-v5/bin")
            for version in ("0", "9", "19", "20")
            for suffix in ("", ".exec", ".native")
        }
        unrelated = {
            f"{directory}/{tool}"
            for directory in ("v5/bin", "stow/llvm-v5/bin")
            for tool in ("clang-format", "clang-tidy", "clang-scan-deps")
        }
        groups = filegroups(binaries | unrelated)
        for name in ("compiler_files", "linker_tool_files"):
            self.assertTrue(binaries <= groups[name], name)
            self.assertFalse(unrelated & groups[name], name)

    def test_profiles_denylists_and_sysroot_are_only_compile_and_link_inputs(self) -> None:
        support = ["@profiles//:pgo", "@profiles//:cspgo", "@etc//:denylist", "@sysroot//:files"]
        host_runtime = ["@native//:runtime"]
        groups = filegroups(set(), support, host_runtime)
        for name in ("compiler_files", "linker_tool_files"):
            self.assertTrue(set(support) <= groups[name])
        for name in ("ar_files", "objcopy_files", "dwp_files", "strip_files"):
            self.assertFalse(set(support) & groups[name])
            self.assertFalse(set(host_runtime) & groups[name])
            self.assertTrue(set(host_runtime) <= groups["host_" + name])

    def test_native_compilation_and_linking(self) -> None:
        for arch in ("x86_64", "aarch64", "ppc64le", "s390x"):
            with self.subTest(arch=arch):
                gcc = f"stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0"
                libexec = f"stow/gcc-v5/libexec/gcc/{arch}-mongodb-linux/14.2.0"
                headers = {
                    "v5/include/openssl/ssl.h",
                    "stow/openssl/include/openssl/ssl.h",
                    "stow/libedit-v5/include/histedit.h",
                    "stow/gcc-v5/include/c++/14.2.0/vector",
                    f"{gcc}/include/stddef.h",
                    f"{gcc}/include-fixed/limits.h",
                    "stow/llvm-v5/lib/clang/19/include/stdint.h",
                }
                runtime = {
                    "v5/lib/libLLVM.so.19.1",
                    "stow/llvm-v5/lib/libclang-cpp.so.19.1",
                    "stow/gcc-v5/lib64/libstdc++.so.6.0.33",
                    "stow/libedit-v5/lib/libedit.so.0",
                    "stow/gcc-v5/lib/libbfd-2.43.so",
                }
                compile_tools = {
                    "v5/clang.cfg",
                    "stow/llvm-v5/clang++.cfg",
                    "v5/bin/clang",
                    "stow/llvm-v5/bin/clang++",
                    "stow/llvm-v5/bin/clang-19",
                    "v5/bin/gcc",
                    f"stow/gcc-v5/bin/{arch}-mongodb-linux-g++",
                    "v5/bin/as",
                    f"{libexec}/cc1plus",
                    "v5/bin/llvm-cov",
                    "v5/bin/llvm-profdata",
                    "v5/bin/gcov",
                }
                link_tools = {
                    "v5/bin/clang++",
                    "v5/bin/g++",
                    "v5/bin/ld",
                    "stow/llvm-v5/bin/ld.lld",
                    "stow/llvm-v5/bin/lld",
                    f"{libexec}/collect2",
                    f"{libexec}/lto-wrapper",
                    f"{libexec}/lto1",
                    f"{libexec}/liblto_plugin.so",
                    f"mold-2.40.4-{arch}-linux/bin/mold",
                }
                link_runtime = {
                    "stow/openssl/lib/libcrypto.a",
                    f"{gcc}/crtbeginS.o",
                    f"{gcc}/libgcc.a",
                    f"{gcc}/specs",
                    "v5/lib64/libstdc++.a",
                    "stow/llvm-v5/lib/clang/19/lib/linux/libclang_rt.asan.a",
                    "stow/llvm-v5/lib/clang/19/share/asan_ignorelist.txt",
                }
                unused = {
                    "v5/lib/libLLVMCore.a",
                    "v5/lib/libclangAST.a",
                    "stow/llvm-v5/lib/libLLVMCore.a",
                    "stow/llvm-v5/include/llvm/IR/Module.h",
                    "stow/llvm-v5/bin/lldb",
                    "stow/gcc-v5/share/man/man1/gcc.1",
                }
                groups = filegroups(
                    headers | runtime | compile_tools | link_tools | link_runtime | unused
                )
                self.assertTrue((headers | runtime | compile_tools) <= groups["compiler_files"])
                self.assertTrue(
                    (runtime | link_tools | link_runtime) <= groups["linker_tool_files"]
                )
                self.assertFalse(headers & groups["linker_tool_files"])
                self.assertNotIn(f"{gcc}/libgcc.a", groups["compiler_files"])
                self.assertNotIn(f"{gcc}/crtbeginS.o", groups["compiler_files"])
                for name in ("compiler_files", "linker_tool_files", "ar_files", "objcopy_files"):
                    self.assertFalse(unused & groups[name], name)

    def test_output_tools_include_dispatch_siblings_and_runtime_without_headers(self) -> None:
        for name, tool in (
            ("ar_files", "ar"),
            ("objcopy_files", "llvm-objcopy"),
            ("dwp_files", "llvm-dwp"),
            ("strip_files", "strip"),
        ):
            with self.subTest(tool=tool):
                tools = {f"v5/bin/{tool}{suffix}" for suffix in ("", ".exec", ".native")}
                host_tools = {
                    f"host_tools/v5/bin/{tool}{suffix}" for suffix in ("", ".exec", ".native")
                }
                runtime = {"stow/gcc-v5/lib/libbfd-2.43.so", "v5/lib/libLLVM.so.19.1"}
                unrelated = {
                    "v5/include/stdio.h",
                    "v5/bin/clang",
                    "v5/bin/lldb",
                    "target/v5/lib64/libstdc++.a",
                }
                groups = filegroups(tools | host_tools | runtime | unrelated)
                self.assertTrue((tools | runtime) <= groups[name])
                self.assertTrue((host_tools | runtime) <= groups[f"host_{name}"])
                self.assertFalse(unrelated & groups[name])
                self.assertFalse(unrelated & groups[f"host_{name}"])

    def test_composed_cross_target_headers_and_runtimes(self) -> None:
        headers = {
            "target/stow/gcc-v5/include/c++/14.2.0/vector",
            "target/stow/llvm-v5/lib/clang/19/include/stdint.h",
        }
        runtime = {
            "target/stow/gcc-v5/lib64/libstdc++.a",
            "target/stow/gcc-v5/lib/gcc/s390x-mongodb-linux/14.2.0/crtbeginS.o",
            "target/stow/llvm-v5/lib/clang/19/lib/linux/libclang_rt.builtins-s390x.a",
            "target/v5/lib64/libgcc_s.so.1",
        }
        groups = filegroups(headers | runtime)
        self.assertTrue(headers <= groups["compiler_files"])
        self.assertTrue(runtime <= groups["linker_tool_files"])
        self.assertFalse(headers & groups["linker_tool_files"])
        self.assertFalse(runtime & groups["ar_files"])

    def test_strip_includes_the_llvm_strip_symlink_target(self) -> None:
        target = "stow/llvm-v5/bin/llvm-objcopy"
        groups = filegroups({"v5/bin/llvm-strip", "stow/llvm-v5/bin/llvm-strip", target})
        self.assertIn(target, groups["strip_files"])


if __name__ == "__main__":
    unittest.main()
