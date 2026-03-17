import sys
import unittest

sys.path.append(".")

from bazel.wrapper_hook.compiledb import _build_final_compile_command_entry


class CompiledbOutputFormatTest(unittest.TestCase):
    def test_final_entry_omits_non_standard_target_key(self):
        def rewrite_exec_path(path, out_root_str, external_root_str):
            if path.startswith("bazel-out/"):
                return out_root_str + "/" + path[len("bazel-out/") :]
            return path

        entry = {
            "file": "bazel-out/k8/bin/src/mongo/base/error_codes.cpp",
            "arguments": ["clang++", "-c", "src/mongo/base/error_codes.cpp"],
            "output": "bazel-out/k8/bin/src/mongo/base/error_codes.cpp.o",
            "target": "//src/mongo/base:error_codes",
        }

        formatted_entry = _build_final_compile_command_entry(
            entry=entry,
            arguments=entry["arguments"],
            repo_root_resolved="/repo",
            rewrite_exec_path=rewrite_exec_path,
            out_root_str="/real/bazel-out",
            external_root_str="/real/external",
        )

        assert formatted_entry == {
            "file": "/real/bazel-out/k8/bin/src/mongo/base/error_codes.cpp",
            "arguments": ["clang++", "-c", "src/mongo/base/error_codes.cpp"],
            "directory": "/repo",
            "output": "/real/bazel-out/k8/bin/src/mongo/base/error_codes.cpp.o",
        }
        assert "target" not in formatted_entry


if __name__ == "__main__":
    unittest.main()
