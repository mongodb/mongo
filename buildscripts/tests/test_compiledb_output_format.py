import sys
import unittest

sys.path.append(".")

from bazel.wrapper_hook.compiledb import (
    SETUP_CLANG_TIDY_BUILD_TARGETS,
    _build_final_compile_command_entry,
    _resolve_extra_build_targets,
)


class CompiledbOutputFormatTest(unittest.TestCase):
    def test_setup_clang_tidy_targets_are_appended_once(self):
        extra_build_targets = [
            "//src/mongo/base:error_codes",
            SETUP_CLANG_TIDY_BUILD_TARGETS[0],
        ]

        resolved_targets = _resolve_extra_build_targets(
            extra_build_targets=extra_build_targets,
            setup_clang_tidy=True,
        )

        assert resolved_targets == [
            "//src/mongo/base:error_codes",
            *SETUP_CLANG_TIDY_BUILD_TARGETS,
        ]

    def test_setup_clang_tidy_targets_are_not_added_when_disabled(self):
        extra_build_targets = ["//src/mongo/base:error_codes"]

        resolved_targets = _resolve_extra_build_targets(
            extra_build_targets=extra_build_targets,
            setup_clang_tidy=False,
        )

        assert resolved_targets == extra_build_targets

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
