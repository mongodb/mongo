"""Protocol checks for the generated Windows host tool wrappers."""

import ast
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

TOOLCHAIN_SOURCE = Path(__file__).with_name("mongo_windows_cross_toolchain.bzl")


class WindowsCrossToolchainTest(unittest.TestCase):
    def test_linux_execution_platform_overrides_inherited_msvc_setting(self) -> None:
        path = TOOLCHAIN_SOURCE.parents[3] / "platforms" / "platform_util.bzl"
        source = ast.parse(path.read_text())
        setup = next(node for node in source.body if isinstance(node, ast.FunctionDef))
        native = SimpleNamespace(platform=mock.Mock())
        namespace = {
            "native": native,
            "REMOTE_EXECUTION_CONTAINERS": {"amazon_linux_2023": {"container-url": "image"}},
        }
        exec(compile(ast.Module(body=[setup], type_ignores=[]), str(path), "exec"), namespace)
        namespace[setup.name]("amd64", "amazon_linux_2023", "")
        base, cross = [call.kwargs for call in native.platform.call_args_list]
        self.assertNotIn("flags", base)
        self.assertEqual(cross["parents"], [":" + base["name"]])
        self.assertEqual(cross["name"], "amazon_linux_2023_amd64_windows_cross_exec")
        self.assertEqual(
            cross["flags"],
            [
                "--//bazel/config:compiler_type=clang",
                "--@rules_rust//rust/settings:toolchain_linker_preference=cc",
            ],
        )
        namespace[setup.name](
            "amd64",
            "amazon_linux_2023",
            "",
            name_suffix="_cross",
            extra_constraints=[":use_mongo_linux_cross_execution"],
        )
        platforms = [call.kwargs for call in native.platform.call_args_list]
        self.assertEqual(len(platforms), 3)
        self.assertEqual(len({platform["name"] for platform in platforms}), 3)
        self.assertIn(":use_mongo_linux_cross_execution", platforms[-1]["constraint_values"])

    def test_resource_directory_headers_are_declared_remote_inputs(self) -> None:
        source = ast.parse(TOOLCHAIN_SOURCE.read_text(encoding="utf-8"))
        stage = next(
            node
            for node in source.body
            if isinstance(node, ast.FunctionDef) and node.name == "_symlink_toolchain_files"
        )
        namespace = {
            "_LLVM_TOOLS": [],
            "_OPTIONAL_LLVM_TOOLS": [],
            "_clang_version": lambda *_: "19",
        }
        exec(
            compile(ast.Module(body=[stage], type_ignores=[]), str(TOOLCHAIN_SOURCE), "exec"),
            namespace,
        )
        ctx = SimpleNamespace(path=lambda _: SimpleNamespace(exists=True), symlink=mock.Mock())
        namespace[stage.name](ctx, "C:/LLVM")
        ctx.symlink.assert_any_call("C:/LLVM/lib/clang/19/include", "resource_dir/include")
        # RBE materializes symlinked directories as separate input trees. Two
        # copies of the builtin headers make include_next stop at the second
        # guarded Clang shim instead of reaching MSVC's vadefs/yvals_core.
        builtin_copies = [
            call.args[1]
            for call in ctx.symlink.call_args_list
            if call.args[0] == "C:/LLVM/lib/clang/19/include"
        ]
        self.assertEqual(builtin_copies, ["resource_dir/include"])
        template = TOOLCHAIN_SOURCE.with_name("BUILD.tmpl").read_text()
        compiler_inputs = template.split('name = "compiler_files",', 1)[1].split("filegroup(", 1)[0]
        self.assertIn('"resource_dir/**"', compiler_inputs)

    def _load_toolchain_functions(self, *names: str) -> dict:
        source = ast.parse(TOOLCHAIN_SOURCE.read_text(encoding="utf-8"))
        body = [
            node
            for node in source.body
            if (isinstance(node, ast.FunctionDef) and node.name in names)
            or (
                isinstance(node, ast.Assign)
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id.startswith("_")
                and not isinstance(node.value, ast.Call)
            )
        ]
        namespace: dict = {}
        exec(
            compile(ast.Module(body=body, type_ignores=[]), str(TOOLCHAIN_SOURCE), "exec"),
            namespace,
        )
        return namespace

    def _staged_tools(self, bin_files: set[str]) -> dict[str, str]:
        """Runs _symlink_toolchain_files against an LLVM tree holding only `bin_files`."""
        namespace = self._load_toolchain_functions("_tool", "_symlink_toolchain_files")
        namespace["_clang_version"] = lambda *_: "19"
        existing = {"/llvm/bin/" + name for name in bin_files} | {"/llvm/lib/clang/19/include"}
        links: dict[str, str] = {}
        ctx = SimpleNamespace(
            path=lambda path: SimpleNamespace(exists=path in existing),
            symlink=lambda source, dest: links.update({dest: source}),
        )
        namespace["_symlink_toolchain_files"](ctx, "/llvm")
        return {dest: source for dest, source in links.items() if dest.startswith("tools/")}

    def test_minimal_archive_layout_resolves_every_driver_name(self) -> None:
        # package_minimal_llvm.py stores the multicall tools under their real names only.
        tools = self._staged_tools(
            {"clang-19", "lld", "llvm-ar", "llvm-rc", "llvm-symbolizer", "llvm-pdbutil"}
        )
        self.assertEqual(
            tools,
            {
                "tools/clang-real": "/llvm/bin/clang-19",
                "tools/clang++": "/llvm/bin/clang-19",
                "tools/clang-cl": "/llvm/bin/clang-19",
                "tools/lld-link": "/llvm/bin/lld",
                "tools/llvm-lib": "/llvm/bin/llvm-ar",
                "tools/llvm-rc": "/llvm/bin/llvm-rc",
                "tools/llvm-symbolizer": "/llvm/bin/llvm-symbolizer",
                "tools/llvm-pdbutil": "/llvm/bin/llvm-pdbutil",
            },
        )

    def test_official_archive_layout_prefers_driver_names(self) -> None:
        tools = self._staged_tools(
            {
                "clang",
                "clang++",
                "clang-cl",
                "clang-19",
                "lld",
                "lld-link",
                "llvm-ar",
                "llvm-lib",
                "llvm-rc",
            }
        )
        self.assertEqual(tools["tools/clang-cl"], "/llvm/bin/clang-cl")
        self.assertEqual(tools["tools/lld-link"], "/llvm/bin/lld-link")
        self.assertEqual(tools["tools/llvm-lib"], "/llvm/bin/llvm-lib")
        # The diagnostic tools are optional: a local LLVM tree without them still stages.
        self.assertNotIn("tools/llvm-symbolizer", tools)
        self.assertNotIn("tools/llvm-pdbutil", tools)

    def test_runtime_library_copier_skips_pinned_minimal_archive(self) -> None:
        namespace = self._load_toolchain_functions("_archive_value", "_write_runtime_libs")
        for environ in [
            {},
            {
                "MONGO_WINDOWS_CROSS_LLVM_URL": namespace["_LLVM_X64_URL"],
                "MONGO_WINDOWS_CROSS_LLVM_SHA256": namespace["_LLVM_X64_SHA256"],
            },
        ]:
            with self.subTest(environ=environ):
                ctx = SimpleNamespace(
                    os=SimpleNamespace(name="windows", environ=environ),
                    file=mock.Mock(),
                    execute=mock.Mock(),
                )
                namespace["_write_runtime_libs"](
                    ctx, namespace["_LLVM_X64_URL"], namespace["_LLVM_X64_SHA256"]
                )
                ctx.file.assert_not_called()
                ctx.execute.assert_not_called()

    def test_runtime_library_copier_preserves_custom_and_full_archives(self) -> None:
        namespace = self._load_toolchain_functions("_archive_value", "_write_runtime_libs")
        for environ, default_url, default_sha in [
            ({"MONGO_WINDOWS_CROSS_LLVM_PATH": "C:/LLVM"}, "_LLVM_X64_URL", "_LLVM_X64_SHA256"),
            ({"MONGO_WINDOWS_CROSS_LLVM_URL": "custom"}, "_LLVM_X64_URL", "_LLVM_X64_SHA256"),
            ({"MONGO_WINDOWS_CROSS_LLVM_SHA256": "custom"}, "_LLVM_X64_URL", "_LLVM_X64_SHA256"),
        ]:
            with self.subTest(environ=environ, default_url=default_url):
                ctx = SimpleNamespace(
                    os=SimpleNamespace(name="windows", environ=environ),
                    file=mock.Mock(),
                    path=lambda path: path,
                    execute=mock.Mock(return_value=SimpleNamespace(return_code=0)),
                )
                namespace["_write_runtime_libs"](
                    ctx, namespace[default_url], namespace[default_sha]
                )
                self.assertEqual(ctx.file.call_args.args[0], "copy_runtime_libs.ps1")
                self.assertEqual(ctx.execute.call_args.args[0][0], "powershell.exe")

    def test_windows_registers_linux_execution_and_native_wrappers(self) -> None:
        namespace = self._load_toolchain_functions(
            "_execution_arch", "_windows_cross_toolchain_registration_impl"
        )
        for host, expected_count in [("windows", 2), ("linux", 1), ("mac os x", 0)]:
            with self.subTest(host=host):
                files = {}
                ctx = SimpleNamespace(
                    os=SimpleNamespace(name=host, environ={}),
                    file=lambda name, contents: files.update({name: contents}),
                )
                namespace["_windows_cross_toolchain_registration_impl"](ctx)
                build = files["BUILD.bazel"]
                self.assertEqual(build.count("\ntoolchain("), expected_count)
                if expected_count:
                    self.assertIn('"@platforms//os:linux"', build)
                    self.assertIn('//:cc-compiler-windows_x86_64"', build)
                self.assertEqual("-host-wrapper" in build, host == "windows")

    def test_archive_and_registration_use_explicit_execution_architecture(self):
        namespace = self._load_toolchain_functions(
            "_execution_arch",
            "_default_llvm_archive",
            "_windows_cross_toolchain_registration_impl",
        )
        for host in ("windows", "linux"):
            for arch, pool in (
                ("x86_64", "amd64"),
                ("amd64", "custom-x64-pool"),
            ):
                with self.subTest(host=host, arch=arch):
                    files = {}
                    ctx = SimpleNamespace(
                        os=SimpleNamespace(
                            name=host,
                            arch="aarch64",
                            environ={
                                "MONGO_WINDOWS_CROSS_EXEC_ARCH": arch,
                                "MONGO_WINDOWS_CROSS_RBE_POOL": pool,
                                "BAZEL_ENGFLOW_POOL": "conflicting-unrelated-value",
                            },
                        ),
                        file=lambda name, contents: files.update({name: contents}),
                    )
                    archive = namespace["_default_llvm_archive"](ctx)
                    self.assertEqual(archive[0], namespace["_LLVM_X64_URL"])
                    namespace["_windows_cross_toolchain_registration_impl"](ctx)
                    execution = files["BUILD.bazel"].split("target_compatible_with", 1)[0]
                    self.assertIn('"@platforms//cpu:x86_64"', execution)
        ctx.os.environ = {}
        self.assertEqual(namespace["_execution_arch"](ctx), "x86_64")

    def test_execution_architecture_rejects_arm_execution_and_pools(self):
        namespace = self._load_toolchain_functions("_execution_arch")
        namespace["fail"] = lambda message: (_ for _ in ()).throw(ValueError(message))
        for host in ("linux", "windows"):
            for environ in (
                {"MONGO_WINDOWS_CROSS_EXEC_ARCH": "arm64"},
                {"MONGO_WINDOWS_CROSS_EXEC_ARCH": "aarch64"},
                {"MONGO_WINDOWS_CROSS_EXEC_ARCH": "unsupported"},
                {"MONGO_WINDOWS_CROSS_RBE_POOL": "default"},
                {"MONGO_WINDOWS_CROSS_RBE_POOL": "arm64"},
                {"MONGO_WINDOWS_CROSS_RBE_POOL": "aarch64"},
            ):
                with self.subTest(host=host, environ=environ), self.assertRaises(ValueError):
                    namespace["_execution_arch"](
                        SimpleNamespace(os=SimpleNamespace(name=host, environ=environ))
                    )

    def test_windows_wrapper_uses_hermetic_python_without_where(self):
        source = TOOLCHAIN_SOURCE.read_text(encoding="utf-8")

        self.assertIn('if not defined python set "python=python.exe"', source)
        self.assertNotIn("where python", source)


if __name__ == "__main__":
    unittest.main()
