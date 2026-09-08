from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import pathlib
import sys
import tempfile
import types
import unittest
from unittest import mock


def _load_module() -> types.ModuleType:
    """Load the script with a stub for PyGithub, which is not needed by these tests."""
    github_module = types.ModuleType("github")
    github_module.GithubException = type("GithubException", (Exception,), {})
    github_module.GithubIntegration = object
    github_module.InputGitTreeElement = object
    github_module.Repository = types.SimpleNamespace(Repository=object)
    path = pathlib.Path(__file__).with_name("remote_execution_containers_pr.py")
    spec = importlib.util.spec_from_file_location("remote_execution_containers_pr", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {path}")
    module = importlib.util.module_from_spec(spec)
    with mock.patch.dict(sys.modules, {"github": github_module}):
        spec.loader.exec_module(module)
    return module


containers_pr = _load_module()


def containers_bzl(entries: dict[str, str]) -> str:
    body = "".join(
        f'    "{distro}": {{\n        "container-url": "{url}",\n    }},\n'
        for distro, url in entries.items()
    )
    return f"REMOTE_EXECUTION_CONTAINERS = {{\n{body}}}\n"


class SummarizeContainerChangesTest(unittest.TestCase):
    def test_reports_repinned_distros_by_digest(self) -> None:
        old = containers_bzl({"ubuntu24": "docker://repo@sha256:aaa"})
        new = containers_bzl({"ubuntu24": "docker://repo@sha256:bbb"})

        self.assertEqual(
            "- **ubuntu24**: `sha256:aaa` â†’ `sha256:bbb`",
            containers_pr.summarize_container_changes(old, new),
        )

    def test_reports_added_and_removed_distros(self) -> None:
        old = containers_bzl({"debian10": "docker://repo@sha256:aaa"})
        new = containers_bzl({"debian13": "docker://repo@sha256:bbb"})

        summary = containers_pr.summarize_container_changes(old, new)

        self.assertIn("- **debian13**: added, now `docker://repo@sha256:bbb`", summary)
        self.assertIn("- **debian10**: removed", summary)

    def test_lists_untouched_distros_separately(self) -> None:
        old = containers_bzl(
            {"rhel8": "docker://repo@sha256:aaa", "ubuntu24": "docker://repo@sha256:ccc"}
        )
        new = containers_bzl(
            {"rhel8": "docker://repo@sha256:bbb", "ubuntu24": "docker://repo@sha256:ccc"}
        )

        summary = containers_pr.summarize_container_changes(old, new)

        self.assertIn("- **rhel8**: `sha256:aaa` â†’ `sha256:bbb`", summary)
        self.assertIn("- _Unchanged: ubuntu24_", summary)

    def test_no_changes(self) -> None:
        content = containers_bzl({"rhel8": "docker://repo@sha256:aaa"})

        self.assertEqual(
            "- _Unchanged: rhel8_", containers_pr.summarize_container_changes(content, content)
        )

    def test_handles_a_container_file_that_does_not_exist_yet(self) -> None:
        new = containers_bzl({"rhel8": "docker://repo@sha256:aaa"})

        self.assertEqual(
            "- **rhel8**: added, now `docker://repo@sha256:aaa`",
            containers_pr.summarize_container_changes("", new),
        )


class PrintDryRunTest(unittest.TestCase):
    @contextlib.contextmanager
    def _workspace(self, container_file_content: str | None, lockfile_content: str | None = None):
        """Run in a temp directory holding the files a real run would commit."""
        cwd = os.getcwd()
        with tempfile.TemporaryDirectory() as temp_dir:
            os.chdir(temp_dir)
            try:
                for path, content in (
                    (containers_pr.CONTAINER_FILE, container_file_content),
                    (containers_pr.LOCKFILE, lockfile_content),
                ):
                    if content is not None:
                        pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
                        pathlib.Path(path).write_text(content, encoding="utf-8")
                yield
            finally:
                os.chdir(cwd)

    @staticmethod
    def _lockfile(digests: dict[str, str]) -> str:
        return json.dumps(
            {
                "moduleExtensions": {
                    name: {"general": {"bzlTransitiveDigest": digest}}
                    for name, digest in digests.items()
                }
            }
        )

    def test_dumps_the_generated_container_map(self) -> None:
        content = containers_bzl({"ubuntu24": "docker://repo@sha256:abc123"})
        output = io.StringIO()

        with self._workspace(content), contextlib.redirect_stdout(output):
            containers_pr.print_dry_run([containers_pr.CONTAINER_FILE])

        printed = output.getvalue()
        self.assertIn("Dry run, not opening a PR", printed)
        self.assertIn(content, printed, "the generated container map must be dumped in full")

    def test_dumps_the_lockfile_digests(self) -> None:
        """Rewriting the map invalidates these, so a dry run has to show the new values."""
        lockfile = self._lockfile({"//bazel:bzlmod.bzl%setup_mongo_toolchains": "abc="})
        output = io.StringIO()

        with (
            self._workspace(containers_bzl({"rhel8": "docker://repo@sha256:aaa"}), lockfile),
            contextlib.redirect_stdout(output),
        ):
            containers_pr.print_dry_run([])

        printed = output.getvalue()
        self.assertIn("MODULE.bazel.lock bzlTransitiveDigest:", printed)
        self.assertIn('"//bazel:bzlmod.bzl%setup_mongo_toolchains": "abc=",', printed)

    def test_skips_third_party_extension_digests(self) -> None:
        lockfile = self._lockfile(
            {
                "//bazel:bzlmod.bzl%setup_mongo_toolchains": "abc=",
                "@@rules_python~//python/uv:uv.bzl%uv": "def=",
            }
        )
        output = io.StringIO()

        with (
            self._workspace(containers_bzl({"rhel8": "docker://repo@sha256:aaa"}), lockfile),
            contextlib.redirect_stdout(output),
        ):
            containers_pr.print_dry_run([])

        printed = output.getvalue()
        self.assertIn("setup_mongo_toolchains", printed)
        self.assertNotIn("rules_python", printed)

    def test_does_not_list_the_files_it_would_commit(self) -> None:
        """The map and the digests are the output; the file list was noise."""
        output = io.StringIO()

        with (
            self._workspace(containers_bzl({"rhel8": "docker://repo@sha256:aaa"})),
            contextlib.redirect_stdout(output),
        ):
            containers_pr.print_dry_run(["bazel/remote_execution_container/ubuntu24/dockerfile"])

        self.assertNotIn("ubuntu24/dockerfile", output.getvalue())

    def test_reports_a_missing_container_map(self) -> None:
        output = io.StringIO()

        with self._workspace(None), contextlib.redirect_stdout(output):
            containers_pr.print_dry_run([])

        self.assertIn(f"{containers_pr.CONTAINER_FILE} does not exist", output.getvalue())


class IsTruthyTest(unittest.TestCase):
    def test_accepts_the_values_evergreen_expansions_produce(self) -> None:
        for value in ("true", "True", " TRUE ", "1", "yes"):
            with self.subTest(value=value):
                self.assertTrue(containers_pr.is_truthy(value))

    def test_everything_else_is_false(self) -> None:
        for value in ("false", "", "  ", "no", "0", "${is_patch}"):
            with self.subTest(value=value):
                self.assertFalse(containers_pr.is_truthy(value))


class DefaultFilesTest(unittest.TestCase):
    def test_includes_the_container_map_the_lockfile_and_every_dockerfile(self) -> None:
        # The repo layout is recreated here rather than globbed from the workspace, which is
        # not present when this runs in a test sandbox.
        cwd = os.getcwd()
        with tempfile.TemporaryDirectory() as temp_dir:
            os.chdir(temp_dir)
            try:
                for path in (
                    containers_pr.CONTAINER_FILE,
                    f"{containers_pr.DOCKERFILE_DIR}/ubuntu24/dockerfile",
                    f"{containers_pr.DOCKERFILE_DIR}/rhel89/Dockerfile",
                ):
                    pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
                    pathlib.Path(path).touch()

                files = containers_pr.default_files()
            finally:
                os.chdir(cwd)

        self.assertEqual(
            [
                containers_pr.CONTAINER_FILE,
                containers_pr.LOCKFILE,
                f"{containers_pr.DOCKERFILE_DIR}/rhel89/Dockerfile",
                f"{containers_pr.DOCKERFILE_DIR}/ubuntu24/dockerfile",
            ],
            files,
            "both Dockerfile spellings are picked up, and the lockfile goes with the map",
        )


if __name__ == "__main__":
    unittest.main()
