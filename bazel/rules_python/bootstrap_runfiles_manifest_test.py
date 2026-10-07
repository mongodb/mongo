import ast
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

_BOOTSTRAP_TEMPLATE = Path(sys.argv.pop(1))
_STAGE2_TEMPLATE = Path(sys.argv.pop(1))
_TEMPLATES = (_BOOTSTRAP_TEMPLATE, _STAGE2_TEMPLATE)
_PATCH_NG_RUNFILES_PATH = "rules_pycross~~_repo_rules~patch-ng/patch_ng.py"


def _load_manifest_helpers(template: str) -> dict[str, object]:
    module = ast.parse(template.replace("%shebang%", "#!/usr/bin/env python3"))
    helper_functions = [
        node
        for node in module.body
        if isinstance(node, ast.FunctionDef)
        and node.name in {"_runfiles_manifest_path", "_resolve_runfile"}
    ]
    namespace = {"IS_WINDOWS": False, "join": os.path.join, "os": os}
    exec(compile(ast.Module(helper_functions, type_ignores=[]), "bootstrap", "exec"), namespace)
    return namespace


class BootstrapRunfilesManifestTest(unittest.TestCase):
    def test_resolves_manifest_targets_in_both_bootstraps(self):
        for template_path in _TEMPLATES:
            for escaped in (False, True):
                with (
                    self.subTest(template=template_path.name, escaped=escaped),
                    tempfile.TemporaryDirectory() as temp_dir,
                ):
                    namespace = _load_manifest_helpers(template_path.read_text(encoding="utf-8"))
                    temp_path = Path(temp_dir)
                    runfiles_root = temp_path / "wheel_installer.exe.runfiles"
                    manifest_path = Path(str(runfiles_root) + "_manifest")
                    patch_ng = temp_path / "external with space" / "patch_ng.py"
                    patch_ng.parent.mkdir()
                    patch_ng.write_text("# test\n", encoding="utf-8")
                    target = str(patch_ng)
                    if escaped:
                        target = target.replace("\\", r"\b").replace(" ", r"\s")
                    manifest_path.write_text(
                        f"{' ' if escaped else ''}{_PATCH_NG_RUNFILES_PATH} {target}\n",
                        encoding="utf-8",
                    )
                    with mock.patch.dict(
                        os.environ, {"RUNFILES_MANIFEST_FILE": str(manifest_path)}, clear=False
                    ):
                        resolved = namespace["_resolve_runfile"](
                            str(runfiles_root), _PATCH_NG_RUNFILES_PATH
                        )
                    self.assertEqual(str(patch_ng), resolved)

    def test_existing_tree_files_take_precedence_over_manifest(self):
        for template_path in _TEMPLATES:
            with (
                self.subTest(template=template_path.name),
                tempfile.TemporaryDirectory() as temp_dir,
            ):
                namespace = _load_manifest_helpers(template_path.read_text(encoding="utf-8"))
                runfiles_root = Path(temp_dir) / "runfiles"
                source = runfiles_root / "repo" / "main.py"
                source.parent.mkdir(parents=True)
                source.touch()
                with mock.patch.dict(os.environ, {"RUNFILES_MANIFEST_FILE": ""}, clear=False):
                    self.assertEqual(
                        str(source),
                        namespace["_resolve_runfile"](str(runfiles_root), "repo/main.py"),
                    )

    def test_create_venv_resolves_explicit_symlinks_through_manifest(self):
        template = _BOOTSTRAP_TEMPLATE.read_text(encoding="utf-8")
        module = ast.parse(template.replace("%shebang%", "#!/usr/bin/env python3"))
        create_venv = next(
            node
            for node in module.body
            if isinstance(node, ast.FunctionDef) and node.name == "_create_venv"
        )

        uses_manifest_resolver = any(
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "_resolve_runfile"
            for node in ast.walk(create_venv)
        )
        self.assertTrue(uses_manifest_resolver)

    def test_windows_venv_traversal_uses_extended_paths(self):
        template = _BOOTSTRAP_TEMPLATE.read_text(encoding="utf-8")

        self.assertIn("if len(p) < 248", template)
        self.assertIn("os.listdir(_win_extended_path(python_home))", template)
        self.assertIn("os.listdir(_win_extended_path(runfiles_venv_bin))", template)


if __name__ == "__main__":
    unittest.main()
