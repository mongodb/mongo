"""Unit tests for buildscripts/resmokelib/multiversion/dsc_release.py.

Evergreen invokes it through the evergreen/resolve_dsc_release.py shim. This
file covers the resolution policy, the configuration parsing and the CLI; the
discovery-route implementations are tested in test_dsc_release_sources.py.

The test matrix runs with fake sources and no network: newest-wins,
skip-unpublished, cross-boundary, same-series rc passes the sanity gate,
sanity failure on a result not older than the version under test, empty ->
skip signal, and override precedence. The "older than the code under test"
bound itself is git topology, owned by the sources (their at/descending-HEAD
exclusion, tested in test_dsc_release_sources.py).
"""

import contextlib
import io
import os
import unittest
from tempfile import TemporaryDirectory
from unittest import mock

import buildscripts.resmokelib.multiversion.dsc_release as under_test
import buildscripts.resmokelib.multiversion.dsc_release_sources as under_test_sources


class FakeSource(under_test.VersionSource):
    """A VersionSource with canned candidates and publication answers.

    ``candidates`` must already be newest first -- ordering is the source's
    responsibility and :func:`resolve` consumes them in order.
    """

    def __init__(
        self,
        name="fake",
        candidates=None,
        published=(),
        list_error=None,
    ):
        self.name = name
        self._candidates = list(candidates or [])
        self._published = set(published)
        self._list_error = list_error
        self.probed = []
        self.listed = False

    def candidate_versions(self):
        if self._list_error is not None:
            raise self._list_error
        self.listed = True
        return list(self._candidates)

    def has_published_binary(self, version):
        self.probed.append(version)
        return version in self._published


class TestResolverConfig(unittest.TestCase):
    def test_reads_expansions_from_env(self):
        config = under_test.ResolverConfig.from_env(
            {
                "multiversion_platform": "amazon2023",
                "multiversion_architecture": "aarch64",
                "multiversion_edition": "enterprise",
            }
        )
        self.assertEqual(
            config.query,
            under_test.DscBinaryQuery(
                platform="amazon2023", architecture="aarch64", edition="enterprise"
            ),
        )

    def test_missing_required_env_is_a_config_error(self):
        for missing in (
            "multiversion_platform",
            "multiversion_architecture",
            "multiversion_edition",
        ):
            env = {
                "multiversion_platform": "amazon2023",
                "multiversion_architecture": "aarch64",
                "multiversion_edition": "enterprise",
            }
            del env[missing]
            with self.assertRaises(under_test.ConfigError):
                under_test.ResolverConfig.from_env(env)

    def test_version_pin_is_not_read_from_env(self):
        # The pin belongs to the shell: the resolver always discovers, so a set
        # pin must not short-circuit discovery.
        config = under_test.ResolverConfig.from_env(
            {
                "multiversion_platform": "p",
                "multiversion_architecture": "a",
                "multiversion_edition": "e",
                "multiversion_dsc_release": "9.0.0-rc1020",
            }
        )
        self.assertFalse(hasattr(config, "override"))


class TestReadVersionUnderTest(unittest.TestCase):
    def setUp(self):
        self._tmp = TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.path = os.path.join(self._tmp.name, ".bazelrc.target_mongo_version")

    def test_parses_the_mongo_version_line(self):
        with open(self.path, "w") as bazelrc:
            bazelrc.write("# comment\ncommon --define=MONGO_VERSION=9.1.0\n")
        self.assertEqual(under_test.read_version_under_test(self.path), "9.1.0")

    def test_missing_line_is_a_config_error(self):
        with open(self.path, "w") as bazelrc:
            bazelrc.write("build --no-version\n")
        with self.assertRaises(under_test.ConfigError):
            under_test.read_version_under_test(self.path)

    def test_missing_file_is_a_config_error(self):
        with self.assertRaises(under_test.ConfigError):
            under_test.read_version_under_test(os.path.join(self._tmp.name, "nonexistent"))


class TestResolvePolicy(unittest.TestCase):
    """The policy matrix from S1, exercised through the public resolve() core."""

    def test_newest_published_candidate_wins(self):
        source = FakeSource(
            candidates=["9.0.0-rc1020", "9.0.0-rc1013"],
            published=["9.0.0-rc1013", "9.0.0-rc1020"],
        )
        self.assertEqual(under_test.resolve([source], "9.1.0"), "9.0.0-rc1020")
        # Newest first: the resolution stops at the first published candidate.
        self.assertEqual(source.probed, ["9.0.0-rc1020"])

    def test_unpublished_candidates_are_skipped(self):
        source = FakeSource(
            candidates=["9.0.0-rc1021", "9.0.0-rc1020"],
            published=["9.0.0-rc1020"],
        )
        self.assertEqual(under_test.resolve([source], "9.1.0"), "9.0.0-rc1020")
        self.assertEqual(source.probed, ["9.0.0-rc1021", "9.0.0-rc1020"])

    def test_resolution_steps_down_across_train_boundary(self):
        # The 9.1 train has no published binaries yet, so resolution keeps
        # stepping down into the 9.0 train.
        source = FakeSource(
            candidates=["9.1.0-rc1021", "9.0.0-rc1020"],
            published=["9.0.0-rc1020"],
        )
        self.assertEqual(under_test.resolve([source], "9.1.0"), "9.0.0-rc1020")
        self.assertEqual(source.probed, ["9.1.0-rc1021", "9.0.0-rc1020"])

    def test_published_same_series_rc_is_chosen_once_published(self):
        # Last-patch semantics: once the 9.1 DSC binaries are published, the
        # old side moves up to them -- that is what production runs -- even
        # though master still targets 9.1.0. That an rc sorts below the GA
        # version it precedes (``9.1.0-rc1021 < 9.1.0``) is a packaging
        # artifact; it passes the sanity gate.
        source = FakeSource(
            candidates=["9.1.0-rc1021", "9.0.0-rc1020"],
            published=["9.1.0-rc1021", "9.0.0-rc1020"],
        )
        self.assertEqual(under_test.resolve([source], "9.1.0"), "9.1.0-rc1021")

    def test_same_release_rc_is_chosen_for_an_alpha_version_under_test(self):
        # Master targets 9.2.0-alpha0 while the same release's DSC train tags
        # r9.2.0-rc1050. Topologically the train tag is older than HEAD (the
        # train forked before it), so it is the correct old side. That the rc
        # channel sorts above the alpha channel is a packaging artifact, not
        # release ordering: the sanity gate compares releases, not channels.
        source = FakeSource(
            candidates=["9.2.0-rc1050", "9.1.0-rc1021"],
            published=["9.2.0-rc1050", "9.1.0-rc1021"],
        )
        self.assertEqual(under_test.resolve([source], "9.2.0-alpha0"), "9.2.0-rc1050")

    def test_ga_tag_of_the_version_under_test_fails_the_sanity_gate(self):
        # A GA tag carrying the release the build targets is the build itself
        # leaking through the topology bound: refused even though the version
        # under test is a pre-release of that same release.
        source = FakeSource(candidates=["9.2.0"], published=["9.2.0"])
        with self.assertRaises(under_test.SanityError):
            under_test.resolve([source], "9.2.0-alpha0")

    def test_result_not_older_than_the_version_under_test_is_a_sanity_failure(self):
        # The MONGO_VERSION comparison is a sanity assertion on the result: a
        # published candidate is selected even when its version string is not
        # older than the version under test, and the violation surfaces as the
        # sanity failure.
        source = FakeSource(
            candidates=["9.2.0-rc0", "9.0.0-rc1020"],
            published=["9.2.0-rc0", "9.0.0-rc1020"],
        )
        with self.assertRaises(under_test.SanityError):
            under_test.resolve([source], "9.1.0")
        # Selected newest-first, then refused by the gate.
        self.assertEqual(source.probed, ["9.2.0-rc0"])

    def test_equal_version_is_also_a_sanity_failure(self):
        # "Older than the version under test" is strict: a tag carrying the
        # version under test itself (e.g. the build under test leaking through
        # the topology bound) must fail the gate.
        source = FakeSource(candidates=["9.1.0"], published=["9.1.0"])
        with self.assertRaises(under_test.SanityError):
            under_test.resolve([source], "9.1.0")

    def test_nothing_published_resolves_to_none(self):
        source = FakeSource(candidates=["9.0.0-rc1020"], published=[])
        self.assertIsNone(under_test.resolve([source], "9.1.0"))
        self.assertEqual(source.probed, ["9.0.0-rc1020"])

    def test_failing_source_falls_through_to_the_next_source(self):
        broken = FakeSource(name="feed", list_error=RuntimeError("no credentials"))
        working = FakeSource(name="tags", candidates=["9.0.0-rc1020"], published=["9.0.0-rc1020"])
        self.assertEqual(under_test.resolve([broken, working], "9.1.0"), "9.0.0-rc1020")
        self.assertFalse(broken.listed)
        self.assertTrue(working.listed)

    def test_first_source_with_a_qualifying_candidate_wins(self):
        feed = FakeSource(name="feed", candidates=["9.0.0-rc1020"], published=["9.0.0-rc1020"])
        tags = FakeSource(name="tags", candidates=["9.0.0-rc1013"], published=["9.0.0-rc1013"])
        self.assertEqual(under_test.resolve([feed, tags], "9.1.0"), "9.0.0-rc1020")
        self.assertFalse(tags.listed)

    def test_probe_failure_steps_down_to_the_older_candidate(self):
        class FlakyProbeSource(FakeSource):
            def has_published_binary(self, version):
                self.probed.append(version)
                if version == "9.0.0-rc1021":
                    raise RuntimeError("s3 is sad")
                return version == "9.0.0-rc1020"

        source = FlakyProbeSource(
            candidates=["9.0.0-rc1021", "9.0.0-rc1020"], published=["9.0.0-rc1020"]
        )
        self.assertEqual(under_test.resolve([source], "9.1.0"), "9.0.0-rc1020")

    def test_unparseable_candidates_are_ignored(self):
        # The step-down traverses unparseable candidates to reach the older
        # published one.
        source = FakeSource(
            candidates=["9.0.0-rc1020", "garbage", "9.0.0-rc1013"],
            published=["9.0.0-rc1013"],
        )
        self.assertEqual(under_test.resolve([source], "9.1.0"), "9.0.0-rc1013")
        self.assertEqual(source.probed, ["9.0.0-rc1020", "9.0.0-rc1013"])

    def test_config_error_propagates_out_of_resolve(self):
        # A ConfigError propagates out of resolve() as the usage error it is
        # (exit 2).
        source = FakeSource(candidates=["9.0.0-rc1020"], published=[])
        source.has_published_binary = mock.Mock(
            side_effect=under_test.ConfigError("unusable probe configuration")
        )
        with self.assertRaises(under_test.ConfigError):
            under_test.resolve([source], "9.1.0")


TASK_ENV = {
    under_test.PLATFORM_ENV_VAR: "amazon2023",
    under_test.ARCHITECTURE_ENV_VAR: "aarch64",
    under_test.EDITION_ENV_VAR: "enterprise",
}


class CLITestCase(unittest.TestCase):
    def run_main(self, argv=None, env=None, sources=None, patch_sources=True):
        """Run main() under a patched environment.

        ``env`` entries override TASK_ENV; a value of ``None`` removes the
        variable entirely (simulating a missing expansion).
        """
        merged_env = dict(TASK_ENV)
        for key, value in (env or {}).items():
            if value is None:
                merged_env.pop(key, None)
            else:
                merged_env[key] = value
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.dict(os.environ, merged_env, clear=True):
            patches = []
            if patch_sources:
                patches.append(
                    mock.patch.object(under_test, "_build_sources", return_value=sources or [])
                )
            with contextlib.ExitStack() as stack:
                for patcher in patches:
                    stack.enter_context(patcher)
                stack.enter_context(contextlib.redirect_stdout(stdout))
                stack.enter_context(contextlib.redirect_stderr(stderr))
                code = under_test.main(argv)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_resolved_version_is_the_only_stdout_line(self):
        source = FakeSource(candidates=["9.0.0-rc1020"], published=["9.0.0-rc1020"])
        code, out, err = self.run_main(
            ["--version-under-test", "9.1.0"],
            sources=[source],
        )
        self.assertEqual(code, under_test.EXIT_RESOLVED)
        self.assertEqual(out, "9.0.0-rc1020\n")
        self.assertTrue(err)

    def test_nothing_resolvable_is_exit_1_with_empty_stdout(self):
        # The skip signal: empty stdout + exit 1. The caller decides fail vs
        # skip (Inc 4 extends this with an explicit protocol).
        source = FakeSource(candidates=["9.0.0-rc1020"], published=[])
        code, out, err = self.run_main(
            ["--version-under-test", "9.1.0"],
            sources=[source],
        )
        self.assertEqual(code, under_test.EXIT_NOTHING_RESOLVABLE)
        self.assertEqual(out, "")
        self.assertTrue(err)

    def test_sanity_failure_is_exit_1_with_empty_stdout(self):
        # A resolved release that is not older than the version under test
        # refuses the result: same signal as nothing-resolvable (the caller
        # decides fail vs. skip), with the reason on stderr.
        source = FakeSource(candidates=["9.2.0-rc0"], published=["9.2.0-rc0"])
        code, out, err = self.run_main(
            ["--version-under-test", "9.1.0"],
            sources=[source],
        )
        self.assertEqual(code, under_test.EXIT_NOTHING_RESOLVABLE)
        self.assertEqual(out, "")
        self.assertIn("sanity check failed", err)

    def test_missing_env_is_a_usage_error(self):
        code, out, _ = self.run_main(
            ["--version-under-test", "9.1.0"],
            env={under_test.PLATFORM_ENV_VAR: None},
            sources=[],
        )
        self.assertEqual(code, under_test.EXIT_USAGE_ERROR)
        self.assertEqual(out, "")

    def test_bad_version_under_test_is_a_usage_error(self):
        code, out, _ = self.run_main(
            ["--version-under-test", "not-a-version"], sources=[FakeSource()]
        )
        self.assertEqual(code, under_test.EXIT_USAGE_ERROR)
        self.assertEqual(out, "")

    def test_override_env_does_not_bypass_discovery(self):
        # The pin expansion belongs to the shell; the resolver only honors
        # --override, so a set pin env var must not short-circuit discovery.
        source = FakeSource(candidates=["9.0.0-rc1020"], published=["9.0.0-rc1020"])
        code, out, _ = self.run_main(
            ["--version-under-test", "9.1.0"],
            env={under_test.OVERRIDE_ENV_VAR: "9.0.0-rc1020"},
            sources=[source],
        )
        self.assertEqual(code, under_test.EXIT_RESOLVED)
        self.assertEqual(out, "9.0.0-rc1020\n")
        # Discovery ran: the source was consulted and probed.
        self.assertTrue(source.listed)
        self.assertEqual(source.probed, ["9.0.0-rc1020"])

    def test_override_flag_bypasses_discovery(self):
        source = FakeSource(candidates=["9.0.0-rc1020"], published=["9.0.0-rc1020"])
        code, out, _ = self.run_main(
            ["--override", "9.0.0-rc1013", "--version-under-test", "9.1.0"],
            sources=[source],
        )
        self.assertEqual(code, under_test.EXIT_RESOLVED)
        self.assertEqual(out, "9.0.0-rc1013\n")
        # The override bypasses all of it: sources are never consulted.
        self.assertFalse(source.listed)
        self.assertEqual(source.probed, [])

    def test_override_works_without_discovery_inputs(self):
        # The emergency override must emit even when the discovery environment
        # (task expansions, version file) is broken or missing.
        stripped_env = {key: None for key in TASK_ENV}
        code, out, _ = self.run_main(["--override", "9.0.0-rc1020"], env=stripped_env, sources=[])
        self.assertEqual(code, under_test.EXIT_RESOLVED)
        self.assertEqual(out, "9.0.0-rc1020\n")

    def test_config_error_from_probe_is_a_usage_error(self):
        # A ConfigError from a probe exits 2 (usage error), distinct from the
        # "nothing resolvable" signal of exit 1.
        source = FakeSource(candidates=["9.0.0-rc1020"], published=[])
        source.has_published_binary = mock.Mock(
            side_effect=under_test.ConfigError("unusable probe configuration")
        )
        code, out, _ = self.run_main(
            ["--version-under-test", "9.1.0"],
            sources=[source],
        )
        self.assertEqual(code, under_test.EXIT_USAGE_ERROR)
        self.assertEqual(out, "")

    def test_debug_diagnostics_go_to_stderr(self):
        source = FakeSource(candidates=["9.0.0-rc1020"], published=["9.0.0-rc1020"])
        code, out, err = self.run_main(
            ["--version-under-test", "9.1.0", "--debug"],
            sources=[source],
        )
        self.assertEqual(code, under_test.EXIT_RESOLVED)
        # stdout purity holds even with the trace enabled.
        self.assertEqual(out, "9.0.0-rc1020\n")
        self.assertIn("Enumerated candidates", err)

    def test_print_candidates_outputs_each_source(self):
        broken = FakeSource(name="feed", list_error=RuntimeError("boom"))
        working = FakeSource(name="tags", candidates=["9.0.0-rc1020"], published=["9.0.0-rc1020"])
        code, out, err = self.run_main(
            ["--print-candidates", "--version-under-test", "9.1.0"],
            sources=[broken, working],
        )
        self.assertEqual(code, under_test.EXIT_RESOLVED)
        # Sources that cannot be examined are logged by the traversal, not
        # printed: stdout carries only the examinable sources' candidates.
        self.assertNotIn("[feed]", out)
        self.assertIn("[tags] 9.0.0-rc1020 eligible=True published=True", out)
        self.assertIn("Discovery source failed; falling through to the next source", err)

    def test_print_candidates_with_no_candidates_is_exit_1(self):
        code, out, _ = self.run_main(
            ["--print-candidates", "--version-under-test", "9.1.0"],
            sources=[FakeSource(name="tags", candidates=[])],
        )
        self.assertEqual(code, under_test.EXIT_NOTHING_RESOLVABLE)

    def test_unresolvable_real_invocation_is_exit_1(self):
        # A real invocation that resolves nothing is "nothing resolvable"
        # (exit 1, stdout empty), not a usage error. In the test sandbox the
        # feed probe cannot succeed, so this exercises the full real chain.
        code, out, _ = self.run_main(["--version-under-test", "9.1.0"], patch_sources=False)
        self.assertEqual(code, under_test.EXIT_NOTHING_RESOLVABLE)
        self.assertEqual(out, "")


class TestBuildSources(unittest.TestCase):
    def setUp(self):
        self.config = under_test.ResolverConfig(
            query=under_test.DscBinaryQuery(platform="p", architecture="a", edition="e"),
        )

    def test_builds_the_tag_topology_route_with_a_feed_probe(self):
        # The tag-topology path is authoritative: git-tag candidates, the feed
        # only as the publication probe.
        sources = under_test._build_sources(self.config)
        self.assertEqual([source.name for source in sources], ["tags"])
        self.assertIsInstance(sources[0].probe, under_test_sources.FeedSource)


if __name__ == "__main__":
    unittest.main()
