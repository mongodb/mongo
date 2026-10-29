"""Unit tests for buildscripts/cs_suggest_variants.py."""

import contextlib
import io
import json
import unittest
import unittest.mock
from pathlib import Path
from tempfile import TemporaryDirectory

import yaml

from buildscripts import cs_suggest_variants as under_test

SHEET_17_HEADER = (
    "Test,Basis,Its own BF episodes,Build Baron verified episodes,"
    "Episodes behind the advice,Episodes covered,Run it in these suites,"
    "First BF,Last BF,Teams the BFs sat with\n"
)
SHEET_16_HEADER = (
    "Test,Kind,Failures in window,Failure episodes,Suites recommended,"
    "Run it in these suites,Variants it has failed on (context only),Note\n"
)


class TestParseSuiteGuidance(unittest.TestCase):
    def test_header_is_found_below_the_provenance_rows(self):
        csv_text = (
            '"36% at 3 suites and 42% at 5"\n'
            "\n" + SHEET_17_HEADER + "jstests/own.js,own history,4,3,4,75%,suite_a | suite_b,"
            "2025-01-01,2026-01-01,Cluster Scalability (2)\n"
            "jstests/area.js,area (jstests/sharding),1,1,4,50%,suite_c,"
            "2025-01-01,2026-01-01,Cluster Scalability (1)\n"
        )

        guidance = under_test.parse_suite_guidance(csv_text.splitlines())

        self.assertEqual("own history", guidance["jstests/own.js"].basis)
        self.assertEqual(["suite_a", "suite_b"], guidance["jstests/own.js"].suites)
        self.assertEqual("area (jstests/sharding)", guidance["jstests/area.js"].basis)
        self.assertEqual(["suite_c"], guidance["jstests/area.js"].suites)

    def test_sheet_without_a_header_yields_nothing(self):
        self.assertEqual({}, under_test.parse_suite_guidance(["note", "", "no header here"]))


class TestParseVariantGuidance(unittest.TestCase):
    def test_cpp_binaries_carry_their_kind_and_variant_context(self):
        csv_text = (
            '"sheet 16 provenance"\n'
            "\n" + SHEET_16_HEADER + "src/mongo/db/s/db_s_resharding_test,C++ unit test binary,"
            "17,3,1,unit_tests,linux-arm64-tsan-essential-required,3 episodes\n"
        )

        guidance = under_test.parse_variant_guidance(csv_text.splitlines())
        advice = guidance["src/mongo/db/s/db_s_resharding_test"]

        self.assertEqual(["unit_tests"], advice.suites)
        self.assertEqual("C++ unit test binary", advice.kind)
        self.assertEqual(["linux-arm64-tsan-essential-required"], advice.variant_context)


class TestLoadGuidance(unittest.TestCase):
    def test_a_jstest_present_only_in_sheet_16_still_gets_advice(self):
        """Sheet 16 holds both kinds of target; its jstests must reach the
        jstest lookup rather than the C++ one, or they resolve to nothing."""
        with TemporaryDirectory() as directory:
            sheet_17 = Path(directory, "sheet17.csv")
            sheet_17.write_text(
                "note\n\n" + SHEET_17_HEADER + "jstests/in17.js,own history,2,2,2,50%,suite_a,,,\n"
            )
            sheet_16 = Path(directory, "sheet16.csv")
            sheet_16.write_text(
                "note\n\n"
                + SHEET_16_HEADER
                + "jstests/only16.js,jstest,9,2,,suite_b,variant-b,\n"
                + "src/mongo/db/s/db_s_a_test,C++ unit test binary,4,2,,unit_tests,variant-c,\n"
            )
            guidance = under_test.load_guidance(sheet_path=sheet_17, variant_path=sheet_16)

        advice = guidance.advice_for(under_test.Target("jstests/only16.js", under_test.JSTEST))
        self.assertIsNotNone(advice)
        self.assertEqual(["suite_b"], advice.suites)
        self.assertEqual(["variant-b"], advice.variant_context)
        # The C++ row must not leak into the jstest lookup.
        self.assertNotIn("src/mongo/db/s/db_s_a_test", guidance.jstest_tests)
        self.assertIn("src/mongo/db/s/db_s_a_test", guidance.cpp_tests)

    def test_a_truncated_artifact_degrades_instead_of_raising(self):
        with TemporaryDirectory() as directory:
            sheet = Path(directory, "sheet.csv")
            sheet.write_text(
                "note\n\n" + SHEET_17_HEADER + "jstests/a.js,own history,2,2,2,50%,suite_a,,,\n"
            )
            holdout = Path(directory, "holdout.json")
            holdout.write_text('{"at_3_suites_pct": 4')  # truncated mid-write

            guidance = under_test.load_guidance(sheet_path=sheet, holdout_path=holdout)

        self.assertIn("jstests/a.js", guidance.jstest_tests)
        self.assertEqual({}, guidance.accuracy)

    def _without_evergreen(self):
        under_test._evergreen_binary.cache_clear()
        under_test._parsed_project_config.cache_clear()
        self.addCleanup(under_test._evergreen_binary.cache_clear)
        self.addCleanup(under_test._parsed_project_config.cache_clear)
        return unittest.mock.patch.object(
            under_test, "find_evergreen_binary", side_effect=OSError("not installed")
        )

    def test_missing_cli_reads_current_required_variants_from_components(self):
        with TemporaryDirectory() as directory, self._without_evergreen():
            repo = Path(directory)
            component = repo / "etc" / "variants.yml"
            component.parent.mkdir()
            component.write_text(
                "buildvariants:\n"
                "  - name: required-now\n"
                '    display_name: "! required"\n'
                "  - name: suggested-now\n"
                '    display_name: "* suggested"\n'
            )
            (repo / "etc" / "evergreen.yml").write_text(
                "include:\n  - filename: etc/variants.yml\n"
            )
            with contextlib.redirect_stdout(io.StringIO()) as output:
                required = under_test.required_variants(repo, under_test.PROJECT)

        self.assertEqual({"required-now"}, required)
        self.assertIn("evergreen CLI not found", output.getvalue())

    def test_a_config_the_cli_rejects_is_raised_not_approximated(self):
        """An invalid config must surface, not yield a normal-looking report
        built from the component-file approximation."""
        under_test._parsed_project_config.cache_clear()
        self.addCleanup(under_test._parsed_project_config.cache_clear)
        with TemporaryDirectory() as directory:
            with (
                unittest.mock.patch.object(under_test, "_evergreen_binary", return_value="evg"),
                unittest.mock.patch.object(
                    under_test, "parse_evergreen_file", side_effect=RuntimeError("bad config")
                ),
            ):
                for lookup in (
                    under_test.required_variants,
                    under_test.variant_tasks,
                    under_test._suite_to_task,
                ):
                    with self.assertRaises(RuntimeError):
                        lookup(Path(directory), under_test.PROJECT)

    def test_the_detected_evergreen_path_is_used_to_evaluate_the_config(self):
        under_test._parsed_project_config.cache_clear()
        self.addCleanup(under_test._parsed_project_config.cache_clear)
        parsed = object()
        with TemporaryDirectory() as directory:
            config = Path(directory, "etc", "evergreen.yml")
            config.parent.mkdir()
            config.write_text("{}")
            with (
                unittest.mock.patch.object(
                    under_test, "_evergreen_binary", return_value="/home/user/evergreen"
                ),
                unittest.mock.patch.object(
                    under_test, "parse_evergreen_file", return_value=parsed
                ) as parse,
            ):
                result = under_test._parsed_project_config(directory, under_test.PROJECT)

        self.assertIs(parsed, result)
        parse.assert_called_once_with(str(config), evergreen_binary="/home/user/evergreen")

    def test_wrong_typed_json_sections_degrade_instead_of_raising(self):
        with TemporaryDirectory() as directory:
            path = Path(directory, "guidance.json")
            path.write_text(
                json.dumps(
                    {
                        "required_variants": None,
                        "tests": None,
                        "jstest_tests": {"jstests/bad.js": "not-a-mapping"},
                        "cpp_tests": {"db_s_test": {"suites": ["unit_tests", 3]}},
                    }
                )
            )
            guidance = under_test.load_guidance(guidance_file=path)

        self.assertEqual(set(), guidance.required_variants)
        self.assertEqual({}, guidance.jstest_tests)
        self.assertEqual(["unit_tests"], guidance.cpp_tests["db_s_test"].suites)

    def test_malformed_cloned_sheet_uses_valid_local_fallback(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            clone = root / "clone"
            cloned_sheets = clone / under_test.GUIDANCE_SUBDIR / "sheets_master"
            cloned_sheets.mkdir(parents=True)
            (cloned_sheets / under_test.SUITE_SHEET).write_text("truncated before header\n")
            (cloned_sheets / under_test.VARIANT_SHEET).write_text(SHEET_16_HEADER)

            local = root / "local"
            local_sheets = local / "sheets_master"
            local_sheets.mkdir(parents=True)
            (local_sheets / under_test.SUITE_SHEET).write_text(
                SHEET_17_HEADER + "jstests/local.js,own history,2,2,2,50%,suite_a,,,\n"
            )
            (local_sheets / under_test.VARIANT_SHEET).write_text(SHEET_16_HEADER)

            with unittest.mock.patch.object(
                under_test,
                "_shallow_clone",
                return_value=contextlib.nullcontext(clone),
            ):
                guidance = under_test._cloned_guidance(
                    "unused", "unused", under_test._load_cloned_sheet_guidance, local
                )

        self.assertIn("jstests/local.js", guidance.jstest_tests)

    def test_guidance_json_routes_cpp_binaries_by_kind(self):
        with TemporaryDirectory() as directory:
            override = Path(directory, "guidance.json")
            override.write_text(
                json.dumps(
                    {
                        "tests": {
                            "jstests/test.js": {"kind": None, "suites": ["js_suite"]},
                            "src/mongo/db/s/db_s_test": {
                                "kind": "C++ unit test binary",
                                "suites": "unit_tests",
                            },
                        },
                        "required_variants": ["required-a"],
                    }
                )
            )

            guidance = under_test.load_guidance(guidance_file=override)

        self.assertEqual(["jstests/test.js"], list(guidance.jstest_tests))
        self.assertEqual(["src/mongo/db/s/db_s_test"], list(guidance.cpp_tests))
        self.assertEqual({"required-a"}, guidance.required_variants)


class TestLoadRateContext(unittest.TestCase):
    def test_missing_summary_file_is_empty_and_unavailable(self):
        with TemporaryDirectory() as directory:
            context = under_test.load_rate_context(Path(directory, "missing.json"))

        self.assertEqual({}, context.worst)
        self.assertEqual({}, context.cpp_cells)
        self.assertFalse(context.available)

    def test_a_valid_summary_with_no_failures_is_available(self):
        with TemporaryDirectory() as directory:
            summary = Path(directory, "summary.json")
            summary.write_text(json.dumps({"per_test": [], "per_cell_failing": []}))
            context = under_test.load_rate_context(summary)

        self.assertTrue(context.available)

    def test_an_incomplete_summary_is_unavailable_not_measured_zero(self):
        with TemporaryDirectory() as directory:
            summary = Path(directory, "summary.json")
            summary.write_text(json.dumps({"per_test": [], "per_cell_failing": None}))
            context = under_test.load_rate_context(summary)

        self.assertFalse(context.available)
        self.assertEqual({}, context.worst)
        self.assertEqual({}, context.cpp_cells)

    def test_invalid_rate_cells_are_ignored(self):
        with TemporaryDirectory() as directory:
            summary = Path(directory, "rates.json")
            summary.write_text(
                json.dumps(
                    {
                        "per_test": [
                            {
                                "test": "db_s_test",
                                "worst_cell": {
                                    "variant": "v",
                                    "task": "unit_tests",
                                    "runs": 0,
                                    "failures": 1,
                                },
                            }
                        ],
                        "per_cell_failing": [
                            {
                                "project": under_test.PROJECT,
                                "test": "db_s_test",
                                "variant": "v",
                                "task": "unit_tests",
                                "runs": 0,
                                "failures": 1,
                            },
                            {
                                "project": under_test.PROJECT,
                                "test": "db_s_test",
                                "variant": "missing",
                                "task": "unit_tests",
                                "failures": 1,
                            },
                            {
                                "project": under_test.PROJECT,
                                "test": "db_s_test",
                                "variant": "impossible",
                                "task": "unit_tests",
                                "runs": 10,
                                "failures": 11,
                            },
                        ],
                    }
                )
            )

            context = under_test.load_rate_context(summary)

        self.assertEqual({}, context.worst)
        self.assertEqual({}, context.cpp_cells)

    def test_cells_from_other_projects_are_ignored(self):
        with TemporaryDirectory() as directory:
            summary = Path(directory, "rates.json")
            summary.write_text(
                json.dumps(
                    {
                        "per_test": [
                            {
                                "test": "jstests/own.js",
                                "worst_cell": {
                                    "variant": "v",
                                    "task": "t",
                                    "runs": 100,
                                    "failures": 4,
                                },
                            }
                        ],
                        "per_cell_failing": [
                            {
                                "project": under_test.PROJECT,
                                "test": "db_s_test",
                                "variant": "v",
                                "task": "unit_tests",
                                "runs": 100,
                                "failures": 4,
                            },
                            {
                                "project": "mongodb-mongo-v8.0",
                                "test": "db_s_test",
                                "variant": "other",
                                "task": "unit_tests",
                                "runs": 100,
                                "failures": 9,
                            },
                        ],
                    }
                )
            )

            context = under_test.load_rate_context(summary)

        self.assertEqual(25, context.worst["jstests/own.js"].one_in_n)
        self.assertEqual(["v"], [cell.variant for cell in context.cpp_cells["db_s_test"]])

    def test_wrong_typed_rate_identifiers_are_skipped(self):
        """Hand-edited JSON can carry objects where dictionary keys are expected;
        those cells are invalid evidence, not a reason for the loader to crash."""
        with TemporaryDirectory() as directory:
            summary = Path(directory, "summary.json")
            summary.write_text(
                json.dumps(
                    {
                        "per_test": [
                            {
                                "test": ["not", "hashable"],
                                "worst_cell": {
                                    "variant": "v",
                                    "task": "t",
                                    "runs": 10,
                                    "failures": 2,
                                },
                            }
                        ],
                        "per_cell_failing": [
                            {
                                "project": [],
                                "test": "db_s_test",
                                "variant": "v",
                                "task": "t",
                                "runs": 10,
                                "failures": 2,
                            },
                            {
                                "project": under_test.PROJECT,
                                "test": ["not", "hashable"],
                                "variant": "v",
                                "task": "t",
                                "runs": 10,
                                "failures": 2,
                            },
                        ],
                    }
                )
            )

            context = under_test.load_rate_context(summary)

        self.assertEqual({}, context.worst)
        self.assertEqual({}, context.cpp_cells)


class TestSelectCppCells(unittest.TestCase):
    def test_required_single_observation_and_negligible_cells_are_excluded(self):
        cells = [
            under_test.RateEvidence("high", "unit_tests", 272, 25),
            under_test.RateEvidence("required", "unit_tests", 316, 18),
            under_test.RateEvidence("single", "unit_tests", 1, 1),
            under_test.RateEvidence("negligible", "unit_tests", 1925, 5),
        ]

        selected = under_test.select_cpp_cells(cells, {"required"})

        self.assertEqual(["high"], [cell.variant for cell in selected])
        self.assertIn(
            "db_s_resharding_test fails ~1 in 11 runs", selected[0].why("db_s_resharding_test")
        )

    def test_stale_variants_are_not_selected(self):
        cells = [
            under_test.RateEvidence("removed", "unit_tests", 100, 20),
            under_test.RateEvidence("current", "unit_tests", 100, 10),
        ]

        runnable = under_test._runnable_cells(
            under_test.select_cpp_cells(cells, set()),
            {under_test.PROJECT: {"current": {"unit_tests"}}},
        )

        self.assertEqual(["current"], [cell.variant for cell in runnable])


class TestBuildUnitTestSourceMap(unittest.TestCase):
    def test_local_and_absolute_labels_both_resolve(self):
        build = """
mongo_cc_unit_test(
    name = "db_s_resharding_test",
    srcs = [
        "resharding_future_util_test.cpp",
        "//src/mongo/db/s/resharding:resharding_recipient_service_test.cpp",
    ],
)
"""

        self.assertEqual(
            {
                "src/mongo/db/s/resharding/resharding_future_util_test.cpp": {
                    "src/mongo/db/s/resharding/db_s_resharding_test"
                },
                "src/mongo/db/s/resharding/resharding_recipient_service_test.cpp": {
                    "src/mongo/db/s/resharding/db_s_resharding_test"
                },
            },
            under_test.build_unit_test_source_map(
                Path("src/mongo/db/s/resharding/BUILD.bazel"), build
            ),
        )

    def test_non_source_deps_and_targets_without_srcs_are_skipped(self):
        build = """
mongo_cc_unit_test(
    name = "no_srcs_test",
    deps = ["//src/mongo/db:service_context"],
)

mongo_cc_unit_test(
    name = "db_s_test",
    srcs = [
        "a_test.cpp",
        "a_test.h",
    ],
)
"""

        self.assertEqual(
            {
                "src/mongo/db/s/a_test.cpp": {"src/mongo/db/s/db_s_test"},
                "src/mongo/db/s/a_test.h": {"src/mongo/db/s/db_s_test"},
            },
            under_test.build_unit_test_source_map(Path("src/mongo/db/s/BUILD.bazel"), build),
        )

    def test_configuration_gated_sources_are_mapped(self):
        """A source reachable only under one build config still belongs to its
        binary; missing it classifies a changed test as production-only."""
        build = """
mongo_cc_unit_test(
    name = "crypto_test",
    srcs = [
        "sha1_block_test.cpp",
    ],
    srcs_select = [{
        "//bazel/config:mongo_crypto_openssl": ["jwt_test.cpp"],
        "//conditions:default": [],
    }],
    tags = ["mongo_unittest_eighth_group"],
)
"""

        self.assertEqual(
            {
                "src/mongo/crypto/sha1_block_test.cpp": {"src/mongo/crypto/crypto_test"},
                "src/mongo/crypto/jwt_test.cpp": {"src/mongo/crypto/crypto_test"},
            },
            under_test.build_unit_test_source_map(Path("src/mongo/crypto/BUILD.bazel"), build),
        )

    def test_private_headers_are_mapped(self):
        """A shared test fixture header reaches its binaries only through
        private_hdrs; missing it makes a changed fixture production-only."""
        build = """
mongo_cc_unit_test(
    name = "stepping_up_test",
    srcs = ["stepping_up_test.cpp"],
    private_hdrs = ["state_test_base.h"],
)

mongo_cc_unit_test(
    name = "initial_test",
    srcs = ["initial_test.cpp"],
    private_hdrs = [":state_test_base.h"],
)
"""

        source_map = under_test.build_unit_test_source_map(Path("src/mongo/sm/BUILD.bazel"), build)

        # The ":" label names a file in this same package, so both spellings
        # have to land on one path rather than two.
        self.assertEqual(
            {"src/mongo/sm/stepping_up_test", "src/mongo/sm/initial_test"},
            source_map["src/mongo/sm/state_test_base.h"],
        )


class TestCollectTargets(unittest.TestCase):
    def test_changed_files_are_split_by_kind(self):
        source_map = {"src/mongo/db/s/a_test.cpp": {"src/mongo/db/s/db_s_test"}}

        targets, ignored_cpp = under_test.collect_targets(
            [
                "jstests/sharding/a.js",
                "src/mongo/db/s/jstests/b.js",
                "src/mongo/db/s/a_test.cpp",
                "src/mongo/db/s/production.cpp",
                "src/mongo/db/s/production.h",
                "etc/evergreen.yml",
            ],
            source_map,
        )

        self.assertEqual(
            [
                ("jstests/sharding/a.js", under_test.JSTEST),
                ("src/mongo/db/s/jstests/b.js", under_test.JSTEST),
                ("src/mongo/db/s/db_s_test", under_test.CPP_UNIT_TEST),
            ],
            [(target.name, target.kind) for target in targets],
        )
        self.assertEqual(
            ["src/mongo/db/s/production.cpp", "src/mongo/db/s/production.h"], ignored_cpp
        )

    def test_a_source_shared_by_two_binaries_yields_both(self):
        """A header compiled into several test binaries must recommend all of
        them; keeping only one would silently drop coverage."""
        source_map = {"src/mongo/db/s/shared.h": {"src/mongo/db/s/b_test", "src/mongo/db/s/a_test"}}

        targets, _ = under_test.collect_targets(["src/mongo/db/s/shared.h"], source_map)

        self.assertEqual(
            ["src/mongo/db/s/a_test", "src/mongo/db/s/b_test"],
            [target.name for target in targets],
        )

    def test_two_changed_sources_in_one_binary_yield_one_target(self):
        source_map = {
            "src/mongo/db/s/a.cpp": {"src/mongo/db/s/shared_test"},
            "src/mongo/db/s/b.cpp": {"src/mongo/db/s/shared_test"},
        }

        targets, ignored = under_test.collect_targets(
            ["src/mongo/db/s/a.cpp", "src/mongo/db/s/b.cpp"], source_map
        )

        self.assertEqual(["src/mongo/db/s/shared_test"], [target.name for target in targets])
        self.assertEqual([], ignored)


class TestChooseSuiteVariants(unittest.TestCase):
    VARIANT_TASKS = {
        "required-a": {"suite_a", "other"},
        "suggested-a": {"suite_a"},
        "unrelated": {"other"},
    }

    def test_failure_context_variant_is_preferred(self):
        choices = under_test.choose_suite_variants(
            ["suite_a"],
            self.VARIANT_TASKS,
            context_variants={"required-a"},
            variant_costs={"required-a": 2, "suggested-a": 1, "unrelated": 3},
        )

        self.assertEqual(["required-a"], [choice.variant for choice in choices])
        self.assertIn("historical failure context", choices[0].why)

    def test_cheapest_variant_is_used_without_failure_context(self):
        """With nothing required running the suite, the fallback still has to
        name some variant for it to run on."""
        choices = under_test.choose_suite_variants(
            ["suite_a"],
            self.VARIANT_TASKS,
            variant_costs={"required-a": 2, "suggested-a": 1, "unrelated": 3},
        )

        self.assertEqual(["suggested-a"], [choice.variant for choice in choices])
        self.assertIn("lowest-task-count", choices[0].why)

    def test_no_context_prefers_a_required_variant_over_an_optional_one(self):
        """The alias runs the required variant anyway, so adding an arbitrary
        optional one buys no coverage and only lengthens the patch."""
        choices = under_test.choose_suite_variants(
            ["suite_a"],
            self.VARIANT_TASKS,
            # The optional variant is the cheaper of the two.
            variant_costs={"required-a": 2, "suggested-a": 1, "unrelated": 3},
            required_variants={"required-a"},
        )

        self.assertEqual(["required-a"], [choice.variant for choice in choices])
        self.assertIn("already in the required set", choices[0].why)

    def test_generated_suite_matches_base_task_variant(self):
        choices = under_test.choose_suite_variants(
            ["suite_a_gen"],
            {"variant-a": {"suite_a"}},
        )

        self.assertEqual(["variant-a"], [choice.variant for choice in choices])

    def test_a_cheaper_required_variant_does_not_displace_actionable_evidence(self):
        """The patch runs required variants anyway, so choosing one by cost
        would drop the only variant that adds coverage."""
        choices = under_test.choose_suite_variants(
            ["suite_a"],
            self.VARIANT_TASKS,
            context_variants={"required-a", "suggested-a"},
            variant_costs={"required-a": 1, "suggested-a": 10},
            required_variants={"required-a"},
        )

        self.assertEqual(["suggested-a"], [choice.variant for choice in choices])
        self.assertTrue(choices[0].from_context)

    def test_the_variant_with_more_recorded_failures_wins_over_the_cheaper_one(self):
        choices = under_test.choose_suite_variants(
            ["suite_a"],
            self.VARIANT_TASKS,
            context_variants={"required-a", "suggested-a"},
            variant_costs={"required-a": 1, "suggested-a": 10},
            evidence_by_variant={"suggested-a": 9, "required-a": 1},
        )

        self.assertEqual(["suggested-a"], [choice.variant for choice in choices])


class TestRecommend(unittest.TestCase):
    def test_required_variants_are_left_out_of_the_patch(self):
        guidance = under_test.Guidance(
            jstest_tests={"jstests/a.js": under_test.TestGuidance("own history", ["suite_a"])},
            required_variants={"required-a"},
        )

        recommendation = under_test.recommend(
            [under_test.Target("jstests/a.js", "jstest")],
            [],
            guidance,
            {"required-a": {"suite_a"}},
            {"required-a"},
        )

        self.assertEqual(["suite_a"], recommendation.suites)
        self.assertEqual([], recommendation.variants)
        self.assertIn("-t", recommendation.command)
        self.assertNotIn("-v", recommendation.command)

    def test_cpp_evidence_adds_its_variant_and_task(self):
        guidance = under_test.Guidance(
            cpp_tests={"db_s_test": under_test.TestGuidance("own history", ["unit_test_group7"])},
            rates=under_test.RateContext(
                cpp_cells={
                    "db_s_test": [under_test.RateEvidence("tsan", "unit_tests", 272, 25)],
                }
            ),
        )

        recommendation = under_test.recommend(
            [under_test.Target("db_s_test", "cpp_unit_test")],
            [],
            guidance,
            {
                "required-a": {"unit_tests"},
                "tsan": {"unit_tests", "unit_test_group7"},
            },
            {"required-a"},
        )

        self.assertEqual(["tsan"], recommendation.variants)
        self.assertEqual(["tsan"], [cell.variant for cell in recommendation.cpp_cells["db_s_test"]])
        self.assertIn("unit_test_group7,unit_tests", " ".join(recommendation.command))
        self.assertIn("unit_tests", " ".join(recommendation.command))

    def test_jstest_context_is_scoped_to_its_suite_and_test_set(self):
        guidance = under_test.Guidance(
            jstest_tests={
                "jstests/a.js": under_test.TestGuidance(
                    "own history", ["suite_s"], variant_context=[]
                ),
                "jstests/b.js": under_test.TestGuidance(
                    "own history", ["suite_s"], variant_context=["suggested-a"]
                ),
            }
        )

        recommendation = under_test.recommend(
            [
                under_test.Target("jstests/a.js", "jstest"),
                under_test.Target("jstests/b.js", "jstest"),
            ],
            [],
            guidance,
            {
                "required-a": {"suite_s"},
                "suggested-a": {"suite_s"},
            },
            {"required-a"},
        )

        self.assertEqual(["suggested-a"], recommendation.variants)
        self.assertIn("historical failure context", recommendation.js_choices[0].why)

    def test_an_unanalysed_target_recommends_nothing(self):
        recommendation = under_test.recommend(
            [under_test.Target("jstests/unknown.js", "jstest")],
            [],
            under_test.Guidance(),
            {"required-a": {"suite_a"}},
            set(),
        )

        self.assertEqual([], recommendation.suites)
        self.assertEqual([], recommendation.variants)
        self.assertEqual([], recommendation.nightly_command)

    def _nightly_guidance(self) -> under_test.Guidance:
        """Guidance whose only qualifying evidence sits on a nightly-only variant."""
        return under_test.Guidance(
            cpp_tests={"db_s_test": under_test.TestGuidance("own history", ["unit_tests"])},
            rates=under_test.RateContext(
                cpp_cells={
                    "db_s_test": [
                        under_test.RateEvidence(
                            "amazon2023",
                            "unit_tests",
                            75,
                            5,
                            project=under_test.NIGHTLY_PROJECT,
                        )
                    ]
                }
            ),
        )

    def test_nightly_only_evidence_becomes_a_second_patch(self):
        recommendation = under_test.recommend(
            [under_test.Target("db_s_test", "cpp_unit_test")],
            [],
            self._nightly_guidance(),
            {"required-a": {"unit_tests"}},
            {"required-a"},
            nightly_variant_tasks={"amazon2023": {"unit_tests"}},
        )

        self.assertEqual([], recommendation.variants)
        self.assertEqual(["amazon2023"], recommendation.nightly_variants)
        self.assertIn(under_test.NIGHTLY_PROJECT, recommendation.nightly_command)
        self.assertIn("amazon2023", recommendation.nightly_command)
        self.assertIn("unit_tests", recommendation.nightly_command)
        self.assertNotIn("required", recommendation.nightly_command)

    def _jstest_guidance(self, context: list[str]) -> under_test.Guidance:
        """Guidance for one jstest whose failure context is the given variants."""
        return under_test.Guidance(
            jstest_tests={
                "jstests/a.js": under_test.TestGuidance(
                    "own history", ["suite_a"], variant_context=context
                )
            }
        )

    def test_a_nightly_only_context_variant_becomes_a_second_patch(self):
        recommendation = under_test.recommend(
            [under_test.Target("jstests/a.js", "jstest")],
            [],
            self._jstest_guidance(["nightly-a"]),
            {"master-a": {"suite_a"}},
            set(),
            nightly_variant_tasks={"nightly-a": {"suite_a"}, "nightly-b": {"suite_a"}},
        )

        self.assertEqual(["master-a"], recommendation.variants)
        self.assertEqual(["nightly-a"], recommendation.nightly_variants)
        self.assertIn("suite_a", recommendation.nightly_command)
        self.assertIn(under_test.NIGHTLY_PROJECT, recommendation.nightly_command)

    def test_nightly_adds_no_variant_without_failure_history_there(self):
        recommendation = under_test.recommend(
            [under_test.Target("jstests/a.js", "jstest")],
            [],
            self._jstest_guidance([]),
            {"master-a": {"suite_a"}},
            set(),
            nightly_variant_tasks={"nightly-a": {"suite_a"}},
        )

        self.assertEqual(["master-a"], recommendation.variants)
        self.assertEqual([], recommendation.nightly_variants)
        self.assertEqual([], recommendation.nightly_command)

    def test_a_nightly_variant_the_project_does_not_have_is_dropped(self):
        """No nightly_variant_tasks means the variant is not in that project's
        matrix at all, so there is nothing for a patch to request."""
        recommendation = under_test.recommend(
            [under_test.Target("db_s_test", "cpp_unit_test")],
            [],
            self._nightly_guidance(),
            {"required-a": {"unit_tests"}},
            {"required-a"},
        )

        self.assertEqual([], recommendation.nightly_variants)
        self.assertEqual([], recommendation.nightly_command)


class TestUnschedulableSuites(unittest.TestCase):
    def test_a_suite_no_variant_runs_is_reported_not_dropped(self):
        """It cannot reach either command, so the printed plan must say so
        rather than letting the reader assume it will be exercised."""
        guidance = under_test.Guidance(
            jstest_tests={
                "jstests/a.js": under_test.TestGuidance("own history", ["suite_a", "orphan_suite"])
            }
        )

        recommendation = under_test.recommend(
            [under_test.Target("jstests/a.js", under_test.JSTEST)],
            [],
            guidance,
            {"variant-a": {"suite_a"}},
            set(),
        )

        self.assertIn("orphan_suite", recommendation.suites)
        self.assertEqual(["orphan_suite"], recommendation.unschedulable_suites)
        self.assertNotIn("orphan_suite", recommendation.command)

    def test_a_suite_on_a_variant_the_patch_does_not_request_is_reported(self):
        """The suite runs somewhere in the project, but not on any variant the
        command names, so Evergreen matches no task for it."""
        guidance = under_test.Guidance(
            cpp_tests={"db_s_test": under_test.TestGuidance("own history", ["orphan_suite"])},
            rates=under_test.RateContext(
                cpp_cells={"db_s_test": [under_test.RateEvidence("A", "other_task", 100, 20)]}
            ),
        )

        recommendation = under_test.recommend(
            [under_test.Target("db_s_test", under_test.CPP_UNIT_TEST)],
            [],
            guidance,
            # orphan_suite runs only on B, which nothing selects.
            {"A": {"other_task"}, "B": {"orphan_suite"}},
            set(),
        )

        self.assertEqual(["A"], recommendation.variants)
        self.assertEqual(["orphan_suite"], recommendation.unschedulable_suites)
        self.assertNotIn("orphan_suite", recommendation.command)


class TestProjectSeparation(unittest.TestCase):
    """Master and nightly evidence must not leak into each other's decisions."""

    def test_one_master_context_does_not_hide_another_tests_nightly_context(self):
        guidance = under_test.Guidance(
            jstest_tests={
                "jstests/a.js": under_test.TestGuidance(
                    "own history", ["suite_a"], variant_context=["master-a"]
                ),
                "jstests/b.js": under_test.TestGuidance(
                    "own history",
                    ["suite_a"],
                    variant_context=["master-b", "nightly-b"],
                ),
            }
        )

        recommendation = under_test.recommend(
            [
                under_test.Target("jstests/a.js", under_test.JSTEST),
                under_test.Target("jstests/b.js", under_test.JSTEST),
            ],
            [],
            guidance,
            {"master-a": {"suite_a"}, "master-b": {"suite_a"}},
            set(),
            nightly_variant_tasks={"nightly-b": {"suite_a"}},
        )

        self.assertEqual(["master-a"], recommendation.variants)
        self.assertEqual(["nightly-b"], recommendation.nightly_variants)

    def test_a_nightly_required_variant_is_still_requested(self):
        """The nightly patch carries no alias, so nothing else schedules it."""
        guidance = under_test.Guidance(
            jstest_tests={
                "jstests/a.js": under_test.TestGuidance(
                    "own history", ["suite_a"], variant_context=["nightly-req"]
                )
            }
        )

        recommendation = under_test.recommend(
            [under_test.Target("jstests/a.js", under_test.JSTEST)],
            [],
            guidance,
            {},
            set(),
            nightly_variant_tasks={"nightly-req": {"suite_a"}},
        )

        self.assertIn("nightly-req", recommendation.nightly_variants)
        self.assertIn("-v", recommendation.nightly_command)
        self.assertNotIn("-a", recommendation.nightly_command)

    def test_a_cell_whose_variant_no_longer_runs_its_task_is_dropped(self):
        """The variant still exists but has stopped running the task, so the
        evidence cannot be reproduced by asking for that pair."""
        guidance = under_test.Guidance(
            cpp_tests={"db_s_test": under_test.TestGuidance("own history", ["unit_tests"])},
            rates=under_test.RateContext(
                cpp_cells={"db_s_test": [under_test.RateEvidence("tsan", "unit_tests", 100, 20)]}
            ),
        )

        recommendation = under_test.recommend(
            [under_test.Target("db_s_test", under_test.CPP_UNIT_TEST)],
            [],
            guidance,
            {"tsan": {"something_else"}},
            set(),
        )

        self.assertEqual([], recommendation.variants)
        self.assertNotIn("unit_tests", " ".join(recommendation.command))
        # The report reads from cpp_cells, so an unrunnable cell left in there
        # would be printed as rate-selected while the command omits it.
        self.assertEqual([], recommendation.cpp_cells["db_s_test"])

    def test_both_projects_evidence_is_selected_and_routed_to_its_own_patch(self):
        """One binary can carry evidence in both projects at once. Each cell
        has to reach the patch for the project that measured it, and the
        printed evidence has to stay in the artifact's order, not rate order."""
        guidance = under_test.Guidance(
            cpp_tests={"db_s_test": under_test.TestGuidance("own history", ["unit_tests"])},
            rates=under_test.RateContext(
                cpp_cells={
                    "db_s_test": [
                        under_test.RateEvidence("master-a", "unit_tests", 100, 20),
                        under_test.RateEvidence(
                            "nightly-a",
                            "unit_tests",
                            100,
                            30,
                            project=under_test.NIGHTLY_PROJECT,
                        ),
                    ]
                }
            ),
        )

        recommendation = under_test.recommend(
            [under_test.Target("db_s_test", under_test.CPP_UNIT_TEST)],
            [],
            guidance,
            {"master-a": {"unit_tests"}},
            set(),
            nightly_variant_tasks={"nightly-a": {"unit_tests"}},
        )

        self.assertEqual(["master-a"], recommendation.variants)
        self.assertEqual(["nightly-a"], recommendation.nightly_variants)
        self.assertEqual(
            ["master-a", "nightly-a"],
            [cell.variant for cell in recommendation.cpp_cells["db_s_test"]],
        )

    def test_nightly_measured_failures_do_not_rank_master_variants(self):
        guidance = under_test.Guidance(
            jstest_tests={
                "jstests/a.js": under_test.TestGuidance(
                    "own history", ["suite_a"], variant_context=["cheap", "pricey"]
                )
            },
            rates=under_test.RateContext(
                worst={
                    "jstests/a.js": under_test.RateEvidence(
                        "pricey", "suite_a", 100, 50, project=under_test.NIGHTLY_PROJECT
                    )
                }
            ),
        )

        recommendation = under_test.recommend(
            [under_test.Target("jstests/a.js", under_test.JSTEST)],
            [],
            guidance,
            {"cheap": {"suite_a"}, "pricey": {"suite_a", "extra"}},
            set(),
        )

        # The nightly-measured strength must not lift "pricey" over "cheap".
        self.assertEqual(["cheap"], recommendation.variants)


class TestVariantTasksFromComponents(unittest.TestCase):
    """The CLI-free fallback must follow each project's own include list.

    Crawling the component directories mixes the projects up in both
    directions, and a variant attributed to the wrong project produces a patch
    command Evergreen rejects.
    """

    def _repo(self, directory: str) -> Path:
        repo = Path(directory)
        components = repo / "etc" / "evergreen_yml_components" / "variants"
        (components / "amazon").mkdir(parents=True)
        (repo / "etc" / "custom").mkdir(parents=True)
        (components / "amazon" / "test_dev.yml").write_text(
            "buildvariants:\n  - name: master-only\n    tasks:\n      - name: suite_a\n"
        )
        (components / "amazon" / "test_release.yml").write_text(
            "buildvariants:\n  - name: nightly-only\n    tasks:\n      - name: suite_a\n"
        )
        # Included by nightly but living outside the variants tree entirely.
        (repo / "etc" / "custom" / "variants.yml").write_text(
            "buildvariants:\n  - name: custom-build\n    tasks:\n      - name: suite_a\n"
        )
        (repo / "etc" / "evergreen.yml").write_text(
            "include:\n" "  - filename: etc/evergreen_yml_components/variants/amazon/test_dev.yml\n"
        )
        (repo / "etc" / "evergreen_nightly.yml").write_text(
            "include:\n"
            "  - filename: etc/evergreen_yml_components/variants/amazon/test_release.yml\n"
            "  - filename: etc/custom/variants.yml\n"
        )
        return repo

    def test_each_project_sees_only_the_variants_it_includes(self):
        with TemporaryDirectory() as directory:
            repo = self._repo(directory)
            master = under_test._variant_tasks_from_components(repo, under_test.PROJECT)
            nightly = under_test._variant_tasks_from_components(repo, under_test.NIGHTLY_PROJECT)

        self.assertEqual(["master-only"], sorted(master))
        # custom-build lives outside the variants tree; a directory crawl misses it.
        self.assertEqual(["custom-build", "nightly-only"], sorted(nightly))

    def test_the_suite_to_task_map_has_a_component_fallback_too(self):
        """variant_tasks falls back to the components without the evergreen
        CLI, so the task mapping must as well: otherwise every suite is
        requested under its own name and a generated one selects nothing."""
        with TemporaryDirectory() as directory:
            repo = self._repo(directory)
            (repo / "etc" / "evergreen_yml_components" / "variants" / "amazon").joinpath(
                "test_dev.yml"
            ).write_text(
                "buildvariants:\n  - name: master-only\n    tasks:\n      - name: sharding_gen\n"
            )

            mapping = under_test._suite_to_task_from_components(repo, under_test.PROJECT)

        self.assertEqual(
            {"sharding": "sharding_gen", "sharding_gen": "sharding_gen"}, mapping["master-only"]
        )

    def test_suite_names_come_from_the_task_definition(self):
        """jsCore runs suite core; its spelling alone cannot say so."""
        with TemporaryDirectory() as directory:
            repo = self._repo(directory)
            tasks = repo / "etc" / "tasks.yml"
            tasks.write_text(
                "tasks:\n"
                "  - name: jsCore\n"
                "    commands:\n"
                "      - func: run tests\n"
                "        vars:\n"
                "          suite: core\n"
            )
            (repo / "etc" / "evergreen_yml_components" / "variants" / "amazon").joinpath(
                "test_dev.yml"
            ).write_text(
                "buildvariants:\n  - name: master-only\n    tasks:\n      - name: jsCore\n"
            )
            (repo / "etc" / "evergreen.yml").write_text(
                "include:\n"
                "  - filename: etc/tasks.yml\n"
                "  - filename: etc/evergreen_yml_components/variants/amazon/test_dev.yml\n"
            )

            variant_tasks = under_test._variant_tasks_from_components(repo, under_test.PROJECT)
            mapping = under_test._suite_to_task_from_components(repo, under_test.PROJECT)
            costs = under_test._variant_task_counts_from_components(repo, under_test.PROJECT)

        self.assertIn("core", variant_tasks["master-only"])
        self.assertEqual("jsCore", mapping["master-only"]["core"])
        self.assertGreater(len(variant_tasks["master-only"]), costs["master-only"])
        self.assertEqual(1, costs["master-only"])

    def test_tag_selectors_are_expanded_from_task_tags(self):
        with TemporaryDirectory() as directory:
            repo = self._repo(directory)
            (repo / "etc" / "tasks.yml").write_text(
                "tasks:\n"
                "  - name: small\n"
                "    tags: [critical]\n"
                "  - name: large\n"
                "    tags: [critical, requires_large_host]\n"
                "  - name: other\n"
                "    tags: [release]\n"
            )
            (repo / "etc" / "evergreen_yml_components" / "variants" / "amazon").joinpath(
                "test_dev.yml"
            ).write_text(
                "buildvariants:\n"
                "  - name: master-only\n"
                "    tasks:\n"
                "      - name: .critical !.requires_large_host\n"
                "      - name: compile_only\n"
            )
            (repo / "etc" / "evergreen.yml").write_text(
                "include:\n"
                "  - filename: etc/tasks.yml\n"
                "  - filename: etc/evergreen_yml_components/variants/amazon/test_dev.yml\n"
            )

            tasks = under_test._variant_tasks_from_components(repo, under_test.PROJECT)

        self.assertEqual({"small", "compile_only"}, tasks["master-only"])

    def test_task_group_selectors_expand_the_group_before_filtering_tags(self):
        with TemporaryDirectory() as directory:
            repo = self._repo(directory)
            (repo / "etc" / "tasks.yml").write_text(
                "tasks:\n"
                "  - name: small\n"
                "  - name: multiversion_task\n"
                "    tags: [multiversion]\n"
                "task_groups:\n"
                "  - name: run_tests_TG\n"
                "    tasks: [small, multiversion_task]\n"
            )
            (repo / "etc" / "evergreen_yml_components" / "variants" / "amazon").joinpath(
                "test_dev.yml"
            ).write_text(
                "buildvariants:\n"
                "  - name: master-only\n"
                "    tasks:\n"
                "      - name: run_tests_TG !.multiversion\n"
            )
            (repo / "etc" / "evergreen.yml").write_text(
                "include:\n"
                "  - filename: etc/tasks.yml\n"
                "  - filename: etc/evergreen_yml_components/variants/amazon/test_dev.yml\n"
            )

            tasks = under_test._declared_variant_tasks(repo, under_test.PROJECT)

        self.assertEqual({"small"}, tasks["master-only"])

    def test_invalid_includes_are_not_treated_as_an_empty_matrix(self):
        with TemporaryDirectory() as directory:
            repo = Path(directory)
            (repo / "etc").mkdir()
            (repo / "etc" / "evergreen.yml").write_text("include: [\n")

            with self.assertRaises(yaml.YAMLError):
                under_test._variant_tasks_from_components(repo, under_test.PROJECT)

        with TemporaryDirectory() as directory:
            repo = Path(directory)
            (repo / "etc").mkdir()
            (repo / "etc" / "evergreen.yml").write_text("include:\n  - filename: etc/missing.yml\n")

            with self.assertRaises(FileNotFoundError):
                under_test._variant_tasks_from_components(repo, under_test.PROJECT)


class TestSuiteToTaskNames(unittest.TestCase):
    """`evergreen patch -t` matches task names, not suite names."""

    GUIDANCE = under_test.Guidance(
        jstest_tests={"jstests/a.js": under_test.TestGuidance("own history", ["sharding"])}
    )

    def _command(self, suite_to_task, variant_tasks=None):
        return under_test.recommend(
            [under_test.Target("jstests/a.js", under_test.JSTEST)],
            [],
            self.GUIDANCE,
            variant_tasks or {"variant-a": {"sharding", "sharding_gen"}},
            set(),
            suite_to_task=suite_to_task,
        )

    def test_a_suite_with_no_mapping_is_requested_unchanged(self):
        self.assertIn("sharding", self._command({}).command)

    def test_a_rate_cell_task_is_resolved_against_its_own_variant(self):
        """Cell tasks come from the analysis artifact, which records the suite
        spelling, so they need the same translation a jstest suite gets."""
        guidance = under_test.Guidance(
            cpp_tests={"db_s_test": under_test.TestGuidance("own history", [])},
            rates=under_test.RateContext(
                cpp_cells={"db_s_test": [under_test.RateEvidence("tsan", "sharding", 100, 20)]}
            ),
        )

        recommendation = under_test.recommend(
            [under_test.Target("db_s_test", under_test.CPP_UNIT_TEST)],
            [],
            guidance,
            {"tsan": {"sharding", "sharding_gen"}},
            set(),
            suite_to_task={"tsan": {"sharding": "sharding_gen"}},
        )

        self.assertEqual(["tsan"], recommendation.variants)
        self.assertIn("sharding_gen", recommendation.command)
        self.assertNotIn("-t sharding ", " ".join(recommendation.command))

    def test_the_task_is_resolved_against_the_variant_that_was_chosen(self):
        """Several tasks can run one suite while a variant carries only one of
        them, so a globally chosen task can miss the selected variant and leave
        -v and -t with an empty intersection."""
        recommendation = under_test.recommend(
            [under_test.Target("jstests/a.js", under_test.JSTEST)],
            [],
            under_test.Guidance(
                jstest_tests={"jstests/a.js": under_test.TestGuidance("own history", ["core"])}
            ),
            # "chosen" is the cheaper variant, so it wins the suite.
            {"chosen": {"core"}, "elsewhere": {"core", "extra"}},
            set(),
            suite_to_task={
                "chosen": {"core": "jsCore"},
                "elsewhere": {"core": "config_fuzzer_jsCore"},
            },
        )

        self.assertEqual(["chosen"], recommendation.variants)
        self.assertIn("jsCore", recommendation.command)
        self.assertNotIn("config_fuzzer_jsCore", recommendation.command)


class TestChangedFiles(unittest.TestCase):
    def test_a_failing_git_command_is_not_treated_as_an_empty_diff(self):
        """A missing base ref would otherwise reduce a whole branch of
        committed tests to "no changed files found"."""
        with TemporaryDirectory() as directory:
            repo = Path(directory)
            under_test.subprocess.run(["git", "init", "-q"], cwd=repo, check=True)

            with self.assertRaises(RuntimeError) as caught:
                under_test.changed_files(repo, base_ref="no/such/ref")

        self.assertIn("no/such/ref", str(caught.exception))

    def test_untracked_and_deleted_files_are_not_reported_as_changed(self):
        """`evergreen patch -u` submits the diff against HEAD, which carries no
        untracked files, and deleted tests no longer need coverage."""
        with TemporaryDirectory() as directory:
            repo = Path(directory)
            under_test.subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            under_test.subprocess.run(
                ["git", "config", "user.email", "t@example.com"], cwd=repo, check=True
            )
            under_test.subprocess.run(["git", "config", "user.name", "t"], cwd=repo, check=True)
            Path(repo, "jstests").mkdir()
            Path(repo, "jstests", "committed.js").write_text("//\n")
            deleted = Path(repo, "jstests", "deleted.js")
            deleted.write_text("//\n")
            under_test.subprocess.run(["git", "add", "-A"], cwd=repo, check=True)
            under_test.subprocess.run(["git", "commit", "-qm", "base"], cwd=repo, check=True)
            Path(repo, "jstests", "untracked.js").write_text("//\n")
            Path(repo, "jstests", "committed.js").write_text("// edited\n")
            deleted.unlink()

            changed = under_test.changed_files(repo, base_ref="HEAD")

        self.assertIn("jstests/committed.js", changed)
        self.assertNotIn("jstests/untracked.js", changed)
        self.assertNotIn("jstests/deleted.js", changed)


class TestBuildEvergreenCommand(unittest.TestCase):
    def test_uses_the_detected_evergreen_binary_path(self):
        command = under_test.build_evergreen_command(
            [], [], evergreen_binary="/home/user/evergreen"
        )

        self.assertEqual("/home/user/evergreen", command[0])

    def test_variants_and_tasks_are_sorted_and_deduplicated(self):
        command = under_test.build_evergreen_command(
            ["suggested-a", "required-a", "required-a"], ["suite_b", "suite_a"]
        )

        self.assertEqual(
            [
                "evergreen",
                "patch",
                "-p",
                "mongodb-mongo-master",
                "-a",
                "required",
                "-v",
                "required-a,suggested-a",
                "-t",
                "suite_a,suite_b",
                "-f",
                "-y",
                "-u",
            ],
            command,
        )


if __name__ == "__main__":
    unittest.main()
