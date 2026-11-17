"""Exercise the repository helper against repository_ctx.template's calling contract."""

import ast
import tempfile
import unittest
from pathlib import Path


class WasiRepositoryTest(unittest.TestCase):
    def test_linux_wrappers_are_rendered_as_executable_files(self) -> None:
        source = Path(__file__).with_name("wasi_repo.bzl")
        tree = ast.parse(source.read_text())
        functions = [
            node
            for node in tree.body
            if isinstance(node, ast.FunctionDef)
            and node.name in ("linux_tool_wrapper_substitutions", "_write_linux_tool_wrappers")
        ]
        namespace = {"LINUX_TOOL_WRAPPER_TEMPLATE": source.with_name("linux_tool_wrapper.sh")}
        exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), "exec"), namespace)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            template_calls: dict[str, bool] = {}

            class Context:
                def template(self, output, template, /, *, substitutions, executable):
                    template_calls[output] = executable
                    content = template.read_text()
                    for old, new in substitutions.items():
                        content = content.replace(old, new)
                    destination = root / output
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_text(content)

            namespace["_write_linux_tool_wrappers"](Context())
            wrappers = list((root / "bin").iterdir())
            self.assertEqual(3, len(wrappers))
            for wrapper in wrappers:
                self.assertNotIn("@MONGO_WASI_", wrapper.read_text())
                self.assertTrue(template_calls[wrapper.relative_to(root).as_posix()])


if __name__ == "__main__":
    unittest.main()
