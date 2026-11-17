"""Check link metadata survives renaming debug-bearing libraries."""

import ast
import unittest
from pathlib import Path
from types import SimpleNamespace


class Depset:
    def __init__(self, direct=(), transitive=()):
        self.values = list(direct)
        for child in transitive:
            self.values.extend(child.to_list())

    def to_list(self):
        return self.values


class SeparateDebugTest(unittest.TestCase):
    def test_library_renaming_preserves_files_referenced_by_link_flags(self):
        source = Path(__file__).with_name("separate_debug.bzl")
        function = next(
            node
            for node in ast.parse(source.read_text()).body
            if isinstance(node, ast.FunctionDef) and node.name == "create_new_ccinfo_library"
        )
        provider = SimpleNamespace
        own = SimpleNamespace(
            user_link_flags=["-Wl,--symbol-ordering-file=own.orderfile", "-Tdep.ld"],
            additional_inputs=["own.orderfile"],
        )
        dependency = SimpleNamespace(user_link_flags=["-Tdep.ld"], additional_inputs=["dep.ld"])
        dep_info = provider(linking_context=SimpleNamespace(linker_inputs=Depset([dependency])))
        original = provider(
            compilation_context="headers",
            linking_context=SimpleNamespace(linker_inputs=Depset([own, dependency])),
        )
        namespace = {
            "CcInfo": provider,
            "depset": Depset,
            "cc_common": SimpleNamespace(
                configure_features=lambda **kwargs: None,
                create_library_to_link=lambda **kwargs: SimpleNamespace(**kwargs),
                create_linker_input=lambda **kwargs: SimpleNamespace(**kwargs),
                create_linking_context=lambda **kwargs: SimpleNamespace(**kwargs),
            ),
            "flat_solib_name": lambda path: "lib" + path,
        }
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), "exec"), namespace)
        for shared in (False, True):
            with self.subTest(shared=shared):
                library = SimpleNamespace(path="bazel-out/lib.so", short_path="lib.so")
                ctx = SimpleNamespace(
                    label="//fixture:renamed",
                    features=[],
                    disabled_features=[],
                    actions=None,
                    attr=SimpleNamespace(
                        type="library",
                        deps=[{provider: dep_info}],
                        binary_with_debug={provider: original},
                    ),
                )
                result = namespace[function.name](
                    ctx, None, library if shared else None, None if shared else library
                )
                linker = result.linking_context.linker_inputs.to_list()[0]
                self.assertEqual(
                    set(linker.additional_inputs.to_list()), {"own.orderfile", "dep.ld"}
                )
                self.assertEqual(
                    linker.user_link_flags, ["-Wl,--symbol-ordering-file=own.orderfile"]
                )
                self.assertEqual(result.compilation_context, "headers")
                self.assertEqual(result.linking_context.linker_inputs.to_list()[1], dependency)


if __name__ == "__main__":
    unittest.main()
