import ast
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

_INSTALLER_SOURCE = Path(sys.argv.pop(1))
_PATCH_NG_RUNFILES_PATH = "rules_pycross++_repo_rules+patch-ng/patch_ng.py"
_INSTALLER_RUNFILES_PATH = (
    "rules_pycross++pycross+rules_pycross_internal/" "_installer_extracted/installer/__init__.py"
)
_PYCROSS_RUNFILES_PATH = "rules_pycross+/pycross/private/tools/namespace_pkgs.py"


def _load_runfiles_resolver() -> dict[str, object]:
    module = ast.parse(_INSTALLER_SOURCE.read_text(encoding="utf-8"))
    helper_nodes = [
        node
        for node in module.body
        if (isinstance(node, ast.FunctionDef) and node.name == "_resolve_module_runfile")
        or (
            isinstance(node, ast.Assign)
            and any(
                isinstance(target, ast.Name)
                and target.id in {"_PATCH_NG_RUNFILES_PATH", "_INSTALLER_RUNFILES_PATH"}
                for target in node.targets
            )
        )
    ]
    namespace: dict[str, object] = {"os": os}
    exec(
        compile(ast.Module(body=helper_nodes, type_ignores=[]), "wheel_installer", "exec"),
        namespace,
    )
    return namespace


class WheelInstallerRunfilesTest(unittest.TestCase):
    def test_resolves_patch_ng_with_versioned_repository_name(self):
        namespace = _load_runfiles_resolver()
        with tempfile.TemporaryDirectory() as temp_dir:
            target = Path(temp_dir) / "patch_ng.py"
            target.write_text("# test\n", encoding="utf-8")
            manifest = Path(temp_dir) / "MANIFEST"
            manifest.write_text(
                f"rules_pycross+0.8.3+_repo_rules+patch-ng/patch_ng.py {target}\n",
                encoding="utf-8",
            )
            original_sys_path = sys.path[:]
            try:
                with mock.patch.dict(
                    os.environ,
                    {"RUNFILES_DIR": "", "RUNFILES_MANIFEST_FILE": str(manifest)},
                ):
                    namespace["_resolve_module_runfile"](
                        namespace["_PATCH_NG_RUNFILES_PATH"], "+patch-ng/patch_ng.py", 0
                    )
                self.assertEqual(str(target.parent), sys.path[0])
            finally:
                sys.path[:] = original_sys_path

    def test_resolves_patch_ng_from_runfiles_directory(self):
        namespace = _load_runfiles_resolver()
        with tempfile.TemporaryDirectory() as temp_dir:
            target = Path(temp_dir) / _PATCH_NG_RUNFILES_PATH
            target.parent.mkdir(parents=True)
            target.write_text("# test\n", encoding="utf-8")
            original_sys_path = sys.path[:]
            try:
                with mock.patch.dict(
                    os.environ,
                    {"RUNFILES_DIR": temp_dir, "RUNFILES_MANIFEST_FILE": ""},
                ):
                    namespace["_resolve_module_runfile"](
                        namespace["_PATCH_NG_RUNFILES_PATH"], "+patch-ng/patch_ng.py", 0
                    )
                self.assertEqual(str(target.parent), sys.path[0])
            finally:
                sys.path[:] = original_sys_path

    def test_resolves_escaped_manifest_entries(self):
        namespace = _load_runfiles_resolver()

        def escape(path: str) -> str:
            return path.replace("\\", r"\b").replace(" ", r"\s").replace("\n", r"\n")

        # Windows paths use backslashes, and either side of a manifest entry may
        # need escaping. Literal escape-like sequences must survive decoding.
        runfile = "repo with space\\pycross\\private\\tools\\namespace_pkgs.py"
        target = "C:\\checkout with space\\newline\\backslash\\space\nnewline\\namespace_pkgs.py"
        manifest = f" {escape(runfile)} {escape(target)}\n"
        original_sys_path = sys.path[:]
        try:
            with (
                mock.patch.dict(
                    os.environ,
                    {"RUNFILES_DIR": "", "RUNFILES_MANIFEST_FILE": "/test/MANIFEST"},
                ),
                mock.patch("builtins.open", mock.mock_open(read_data=manifest)),
                mock.patch("os.path.isfile", return_value=True) as isfile,
            ):
                namespace["_resolve_module_runfile"](
                    _PYCROSS_RUNFILES_PATH, "/pycross/private/tools/namespace_pkgs.py", 3
                )
            isfile.assert_any_call(target)
            self.assertNotEqual(original_sys_path, sys.path)
        finally:
            sys.path[:] = original_sys_path

    def test_resolves_escaped_target_with_spaces(self):
        namespace = _load_runfiles_resolver()

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            manifest_path = temp_path / "MANIFEST"
            target = temp_path / "checkout with space" / "patch_ng.py"
            target.parent.mkdir()
            target.write_text("# test\n", encoding="utf-8")
            escaped_target = (
                str(target).replace("\\", r"\b").replace(" ", r"\s").replace("\n", r"\n")
            )
            manifest_path.write_text(
                f" {_PATCH_NG_RUNFILES_PATH} {escaped_target}\n", encoding="utf-8"
            )
            original_sys_path = sys.path[:]
            try:
                with mock.patch.dict(
                    os.environ,
                    {"RUNFILES_DIR": "", "RUNFILES_MANIFEST_FILE": str(manifest_path)},
                ):
                    namespace["_resolve_module_runfile"](
                        namespace["_PATCH_NG_RUNFILES_PATH"], "+patch-ng/patch_ng.py", 0
                    )
                self.assertEqual(str(target.parent), sys.path[0])
            finally:
                sys.path[:] = original_sys_path

    def test_resolves_patch_ng_from_manifest(self):
        namespace = _load_runfiles_resolver()

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            runfiles_root = temp_path / "wheel_installer.exe.runfiles"
            manifest_path = Path(str(runfiles_root) + "_manifest")
            patch_ng_path = temp_path / "external" / "patch_ng.py"
            patch_ng_path.parent.mkdir()
            patch_ng_path.write_text("# test\n", encoding="utf-8")
            manifest_path.write_text(
                f"{_PATCH_NG_RUNFILES_PATH} {patch_ng_path}\n", encoding="utf-8"
            )

            original_sys_path = sys.path[:]
            try:
                with mock.patch.dict(
                    os.environ,
                    {
                        "RUNFILES_DIR": str(runfiles_root),
                        "RUNFILES_MANIFEST_FILE": str(manifest_path),
                    },
                    clear=False,
                ):
                    namespace["_resolve_module_runfile"](
                        namespace["_PATCH_NG_RUNFILES_PATH"],
                        "+patch-ng/patch_ng.py",
                        0,
                    )
                self.assertEqual(str(patch_ng_path.parent), sys.path[0])
            finally:
                sys.path[:] = original_sys_path

    def test_resolves_installer_package_from_manifest(self):
        namespace = _load_runfiles_resolver()

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            runfiles_root = temp_path / "wheel_installer.exe.runfiles"
            manifest_path = Path(str(runfiles_root) + "_manifest")
            installer_path = (
                temp_path / "external" / "_installer_extracted" / "installer" / "__init__.py"
            )
            installer_path.parent.mkdir(parents=True)
            installer_path.write_text("# test\n", encoding="utf-8")
            manifest_path.write_text(
                f"{_INSTALLER_RUNFILES_PATH} {installer_path}\n", encoding="utf-8"
            )

            original_sys_path = sys.path[:]
            try:
                with mock.patch.dict(
                    os.environ,
                    {
                        "RUNFILES_DIR": str(runfiles_root),
                        "RUNFILES_MANIFEST_FILE": str(manifest_path),
                    },
                    clear=False,
                ):
                    namespace["_resolve_module_runfile"](
                        namespace["_INSTALLER_RUNFILES_PATH"],
                        "/_installer_extracted/installer/__init__.py",
                        1,
                    )
                self.assertEqual(str(installer_path.parent.parent), sys.path[0])
            finally:
                sys.path[:] = original_sys_path

    def test_resolves_pycross_package_from_manifest(self):
        namespace = _load_runfiles_resolver()

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            runfiles_root = temp_path / "wheel_installer.exe.runfiles"
            manifest_path = Path(str(runfiles_root) + "_manifest")
            namespace_pkgs_path = (
                temp_path
                / "external"
                / "rules_pycross~"
                / "pycross"
                / "private"
                / "tools"
                / "namespace_pkgs.py"
            )
            namespace_pkgs_path.parent.mkdir(parents=True)
            namespace_pkgs_path.write_text("# test\n", encoding="utf-8")
            manifest_path.write_text(
                f"{_PYCROSS_RUNFILES_PATH} {namespace_pkgs_path}\n", encoding="utf-8"
            )

            original_sys_path = sys.path[:]
            try:
                with mock.patch.dict(
                    os.environ,
                    {
                        "RUNFILES_DIR": str(runfiles_root),
                        "RUNFILES_MANIFEST_FILE": str(manifest_path),
                    },
                    clear=False,
                ):
                    namespace["_resolve_module_runfile"](
                        _PYCROSS_RUNFILES_PATH,
                        "/pycross/private/tools/namespace_pkgs.py",
                        3,
                    )
                self.assertEqual(str(namespace_pkgs_path.parents[3]), sys.path[0])
            finally:
                sys.path[:] = original_sys_path


if __name__ == "__main__":
    unittest.main()
