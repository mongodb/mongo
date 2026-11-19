from __future__ import annotations

import contextlib
import hashlib
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


def sysroot_pin(distro: str, architecture: str, label: str) -> dict[str, str]:
    digest = hashlib.sha256(label.encode()).hexdigest()
    return {
        "sha256": digest,
        "url": f"https://mdb-build-public.s3.amazonaws.com/toolchain-sysroots/toolchain-sysroot-{distro}-{architecture}-{digest}.tar.gz",
    }


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
    def test_toolchain_dry_run_reports_only_its_manifest(self) -> None:
        output = io.StringIO()
        with (
            self._workspace("CONTAINER-MARKER", '{"moduleExtensions": {}}'),
            contextlib.redirect_stdout(output),
        ):
            path = pathlib.Path(containers_pr.TOOLCHAIN_SYSROOT_FILE)
            path.parent.mkdir(parents=True)
            path.write_text('{"schema-version": 1, "sysroots": {}}')
            containers_pr.print_dry_run([str(path)], True)
        self.assertIn('"sysroots": {}', output.getvalue())
        self.assertNotIn("CONTAINER-MARKER", output.getvalue())
        self.assertNotIn("MODULE.bazel.lock", output.getvalue())

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
    def test_toolchain_mode_only_proposes_its_pin_manifest(self) -> None:
        self.assertEqual([containers_pr.TOOLCHAIN_SYSROOT_FILE], containers_pr.default_files(True))

    def test_sysroot_summary_includes_only_changed_targets(self) -> None:
        old = {
            "schema-version": 1,
            "sysroots": {
                "rhel8": {
                    "s390x": sysroot_pin("rhel8", "s390x", "old"),
                    "ppc64le": sysroot_pin("rhel8", "ppc64le", "same"),
                }
            },
        }
        new = {
            "schema-version": 1,
            "sysroots": {
                "rhel8": {
                    "s390x": sysroot_pin("rhel8", "s390x", "new"),
                    "ppc64le": sysroot_pin("rhel8", "ppc64le", "same"),
                }
            },
        }
        self.assertEqual(
            f"- **rhel8/s390x**: `{new['sysroots']['rhel8']['s390x']['sha256']}`",
            containers_pr.summarize_sysroot_changes(json.dumps(old), json.dumps(new)),
        )

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


class MergeSysrootPinsTest(unittest.TestCase):
    @staticmethod
    def _manifest(pins: dict[str, str]) -> str:
        return json.dumps(
            {
                "schema-version": 1,
                "sysroots": {
                    distro: {"s390x": sysroot_pin(distro, "s390x", pin)}
                    for distro, pin in pins.items()
                },
            }
        )

    def _merge(
        self,
        base: dict[str, str],
        pending: dict[str, str],
        ancestor: dict[str, str],
        generated: dict[str, str],
        published: set[str],
    ) -> dict:
        content = containers_pr.merge_sysroot_pins(
            self._manifest(base),
            self._manifest(pending),
            self._manifest(ancestor),
            self._manifest(generated),
            published,
        )
        return json.loads(content)["sysroots"]

    def _pins(self, pins: dict[str, str]) -> dict:
        return json.loads(self._manifest(pins))["sysroots"]

    def test_second_distro_publication_preserves_pending_first_distro(self) -> None:
        self.assertEqual(
            self._pins({"rhel8": "pending", "rhel9": "new"}),
            self._merge({}, {"rhel8": "pending"}, {}, {"rhel9": "new"}, {"rhel9"}),
        )

    def test_new_publication_replaces_pending_pins_for_that_distro(self) -> None:
        self.assertEqual(
            self._pins({"rhel8": "new", "rhel9": "pending"}),
            self._merge({}, {"rhel8": "old", "rhel9": "pending"}, {}, {"rhel8": "new"}, {"rhel8"}),
        )

    def test_untouched_pins_from_stale_checkout_or_pr_do_not_revert_master(self) -> None:
        self.assertEqual(
            self._pins({"rhel8": "master", "rhel9": "new", "rhel10": "pending"}),
            self._merge(
                {"rhel8": "master"},
                {"rhel8": "old", "rhel10": "pending"},
                {"rhel8": "old"},
                {"rhel8": "old", "rhel9": "new"},
                {"rhel9"},
            ),
        )

    def test_merged_pins_take_precedence_over_conflicting_pending_update(self) -> None:
        self.assertEqual(
            self._pins({"rhel8": "master", "rhel9": "new"}),
            self._merge(
                {"rhel8": "master"},
                {"rhel8": "pending"},
                {"rhel8": "old"},
                {"rhel9": "new"},
                {"rhel9"},
            ),
        )

    def test_malformed_nested_pins_are_rejected_by_all_manifest_consumers(self) -> None:
        valid = self._manifest({"rhel8": "good"})
        for sysroots, field in (
            ({"rhel8": []}, "sysroots.rhel8"),
            ({"rhel8": {"s390x": []}}, "sysroots.rhel8.s390x"),
            ({"rhel8": {"s390x": {}}}, "sysroots.rhel8.s390x.sha256"),
            ({"rhel8": {"s390x": {"sha256": ""}}}, "sysroots.rhel8.s390x.sha256"),
            ({"rhel8": {"s390x": {"sha256": 7}}}, "sysroots.rhel8.s390x.sha256"),
        ):
            malformed = json.dumps({"schema-version": 1, "sysroots": sysroots})
            with self.subTest(sysroots=sysroots):
                with self.assertRaisesRegex(ValueError, field):
                    containers_pr.read_sysroot_manifest(malformed)
                for index in range(2):
                    contents = [valid, valid]
                    contents[index] = malformed
                    with self.assertRaisesRegex(ValueError, field):
                        containers_pr.summarize_sysroot_changes(*contents)
                for index in range(4):
                    contents = [valid] * 4
                    contents[index] = malformed
                    with self.assertRaisesRegex(ValueError, field):
                        containers_pr.merge_sysroot_pins(*contents, {"rhel8"})

    def test_missing_published_distro_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing published distro rhel9"):
            self._merge({}, {}, {}, {"rhel8": "new"}, {"rhel9"})

    def test_unsupported_target_names_are_rejected(self) -> None:
        for distro, architecture in (
            ("rhel7", "s390x"),
            ("rhel8\n[link](https://example.com)", "s390x"),
            ("rhel8", "unknown"),
            ("rhel8", "s390x`\n[link](https://example.com)"),
        ):
            content = json.dumps(
                {
                    "schema-version": 1,
                    "sysroots": {distro: {architecture: sysroot_pin("rhel8", "s390x", "good")}},
                }
            )
            with self.subTest(distro=distro, architecture=architecture):
                with self.assertRaisesRegex(ValueError, "Unsupported"):
                    containers_pr.read_sysroot_manifest(content)

    def test_hashes_and_archive_urls_must_match_the_published_target(self) -> None:
        pin = sysroot_pin("rhel8", "s390x", "good")
        for field, value in (
            ("sha256", "g" * 64),
            ("sha256", "a" * 63),
            ("sha256", "A" * 64),
            ("sha256", "`\n[link](https://example.com)"),
            ("url", None),
            ("url", 7),
            ("url", pin["url"].replace("https:", "http:")),
            ("url", pin["url"].replace(pin["sha256"], "a" * 64)),
            ("url", pin["url"].replace("rhel8-s390x", "rhel9-s390x")),
            ("url", pin["url"].replace("https://", "https://user:password@")),
            ("url", pin["url"].replace("mdb-build-public.s3.amazonaws.com", "example.com")),
            ("url", pin["url"] + "?query=1"),
            ("url", pin["url"] + "#fragment"),
            ("url", pin["url"].replace("/toolchain-sysroots/", "/../")),
            ("url", pin["url"] + "\n"),
        ):
            invalid = {**pin, field: value}
            content = json.dumps({"schema-version": 1, "sysroots": {"rhel8": {"s390x": invalid}}})
            with self.subTest(field=field, value=value):
                with self.assertRaisesRegex(ValueError, f"sysroots.rhel8.s390x.{field}"):
                    containers_pr.read_sysroot_manifest(content)

    def test_supported_targets_and_custom_s3_prefixes_are_accepted(self) -> None:
        pins = {
            distro: {
                architecture: sysroot_pin(distro, architecture, architecture)
                for architecture in ("amd64", "arm64", "ppc64le", "s390x")
            }
            for distro in ("rhel8", "rhel9", "rhel10")
        }
        pins["rhel8"]["s390x"]["url"] = pins["rhel8"]["s390x"]["url"].replace(
            "mdb-build-public.s3.amazonaws.com/toolchain-sysroots/",
            "custom-bucket.s3.amazonaws.com/prefix.with-dots/nested/",
        )
        manifest = {"schema-version": 1, "sysroots": pins}
        self.assertEqual(manifest, containers_pr.read_sysroot_manifest(json.dumps(manifest)))


class FileContentTest(unittest.TestCase):
    def test_only_not_found_returns_empty_content(self) -> None:
        for status in (403, 404, 429, 500):
            with self.subTest(status=status):
                repo = mock.Mock()
                error = containers_pr.GithubException("read failed")
                error.status = status
                repo.get_contents.side_effect = error
                if status == 404:
                    self.assertEqual(
                        "", containers_pr.file_content_on_ref(repo, "pins.json", "ref")
                    )
                else:
                    with self.assertRaises(containers_pr.GithubException) as raised:
                        containers_pr.file_content_on_ref(repo, "pins.json", "ref")
                    self.assertIs(error, raised.exception)

    def test_successful_read_preserves_content(self) -> None:
        repo = mock.Mock()
        content = MergeSysrootPinsTest._manifest({"rhel8": "old"})
        repo.get_contents.return_value.decoded_content = content.encode()
        self.assertEqual(content, containers_pr.file_content_on_ref(repo, "pins.json", "ref"))


class ToolchainPublicationPRTest(unittest.TestCase):
    def test_failed_pin_reads_stop_before_github_mutations(self) -> None:
        for ref in ("refs/heads/master", "refs/heads/codex/toolchain-sysroots-master", "ancestor"):
            with self.subTest(ref=ref), tempfile.TemporaryDirectory() as temp_dir:
                root = pathlib.Path(temp_dir)
                manifest = root / "pins.json"
                manifest.write_text(MergeSysrootPinsTest._manifest({"rhel9": "new"}))
                report = root / "validation.json"
                report.write_text(json.dumps({"sysroots": {"rhel9": {}}, "failed_distros": []}))
                repo = mock.MagicMock()
                repo.get_pulls.return_value.totalCount = 1
                repo.compare.return_value.merge_base_commit.sha = "ancestor"
                error = containers_pr.GithubException("forbidden")
                error.status = 403

                def read(path: str, *, ref: str):
                    if ref == failing_ref:
                        raise error
                    return types.SimpleNamespace(
                        decoded_content=MergeSysrootPinsTest._manifest(
                            {"rhel8": "pending"}
                        ).encode()
                    )

                failing_ref = ref
                repo.get_contents.side_effect = read
                argv = [
                    "remote_execution_containers_pr.py",
                    "--github-owner=10gen",
                    "--github-repo=mongo",
                    "--base-branch=master",
                    "--new-branch=codex/toolchain-sysroots-master",
                    "--pr-title=SERVER-XXXX Update toolchain sysroots",
                    "--toolchain-sysroots",
                    "--app-id=1",
                    "--private-key=fixture",
                    f"--sysroot-report={report}",
                ]
                with (
                    mock.patch.object(containers_pr, "TOOLCHAIN_SYSROOT_FILE", str(manifest)),
                    mock.patch.object(containers_pr, "chdir_to_workspace"),
                    mock.patch.object(containers_pr, "get_repository", return_value=repo),
                    mock.patch.object(containers_pr, "create_branch") as create_branch,
                    mock.patch.object(containers_pr.time, "sleep"),
                    mock.patch.object(sys, "argv", argv),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    with self.assertRaises(containers_pr.GithubException) as raised:
                        containers_pr.main()
                    self.assertIs(error, raised.exception)
                    create_branch.assert_not_called()
                    repo.create_git_tree.assert_not_called()
                    repo.create_git_commit.assert_not_called()
                    repo.create_pull.assert_not_called()
                    repo.get_pulls.return_value.__getitem__.return_value.edit.assert_not_called()

    def test_repeated_publications_update_one_draft_pr_without_losing_pending_pins(self) -> None:
        empty = MergeSysrootPinsTest._manifest({})
        contents = {"refs/heads/master": empty, "ancestor": empty}
        branch = "refs/heads/codex/toolchain-sysroots-master"
        contents[branch] = empty
        repo = mock.MagicMock()
        pulls = mock.MagicMock(totalCount=0)
        existing_pr = mock.Mock(number=42)
        pulls.__getitem__.return_value = existing_pr
        repo.get_pulls.return_value = pulls
        repo.get_branch.return_value.commit.sha = "head"
        repo.compare.return_value.merge_base_commit.sha = "ancestor"
        repo.create_git_commit.return_value.sha = "updated"
        repo.get_contents.side_effect = lambda path, ref: types.SimpleNamespace(
            decoded_content=contents[ref].encode()
        )

        def create_tree(elements: list, old_tree: object) -> mock.Mock:
            contents[branch] = elements[0].content
            return mock.Mock()

        def create_pull(**kwargs) -> mock.Mock:
            self.assertTrue(kwargs["draft"])
            pulls.totalCount = 1
            return existing_pr

        repo.create_git_tree.side_effect = create_tree
        repo.create_pull.side_effect = create_pull
        argv = [
            "remote_execution_containers_pr.py",
            "--github-owner=10gen",
            "--github-repo=mongo",
            "--base-branch=master",
            "--new-branch=codex/toolchain-sysroots-master",
            "--pr-title=SERVER-XXXX Update toolchain sysroots",
            "--toolchain-sysroots",
            "--app-id=1",
            "--private-key=fixture",
        ]
        cwd = os.getcwd()
        with tempfile.TemporaryDirectory() as temp_dir:
            os.chdir(temp_dir)
            try:
                manifest = pathlib.Path(containers_pr.TOOLCHAIN_SYSROOT_FILE)
                manifest.parent.mkdir(parents=True)
                report = pathlib.Path(containers_pr.TOOLCHAIN_SYSROOT_REPORT)
                report.parent.mkdir(parents=True)
                with (
                    mock.patch.object(containers_pr, "get_repository", return_value=repo),
                    mock.patch.object(containers_pr, "create_branch"),
                    mock.patch.object(
                        containers_pr,
                        "InputGitTreeElement",
                        side_effect=lambda **kwargs: types.SimpleNamespace(**kwargs),
                    ),
                    mock.patch.object(containers_pr.time, "sleep"),
                    mock.patch.object(sys, "argv", argv),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    for distro in ("rhel8", "rhel9"):
                        manifest.write_text(MergeSysrootPinsTest._manifest({distro: distro}))
                        report.write_text(
                            json.dumps({"sysroots": {distro: {}}, "failed_distros": []})
                        )
                        self.assertEqual(0, containers_pr.main())
            finally:
                os.chdir(cwd)
        self.assertEqual({"rhel8", "rhel9"}, set(json.loads(contents[branch])["sysroots"]))
        repo.create_pull.assert_called_once()
        repo.compare.assert_called_once_with("master", "codex/toolchain-sysroots-master")
        self.assertIn("rhel8/s390x", existing_pr.edit.call_args.kwargs["body"])
        self.assertIn("rhel9/s390x", existing_pr.edit.call_args.kwargs["body"])


if __name__ == "__main__":
    unittest.main()
