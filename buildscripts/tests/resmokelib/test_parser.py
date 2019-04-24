"""Unit tests for buildscripts/resmokelib/parser.py."""

import unittest

from buildscripts.resmokelib import parser as _parser

# pylint: disable=missing-docstring


class TestLocalCommandLine(unittest.TestCase):
    """Unit tests for the to_local_args() function."""

    def test_keeps_any_positional_arguments(self):
        cmdline = _parser.to_local_args([
            "test_file1.js",
            "--suites=my_suite",
            "test_file2.js",
            "--storageEngine=my_storage_engine",
            "test_file3.js",
            "test_file4.js",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "test_file1.js",
            "test_file2.js",
            "test_file3.js",
            "test_file4.js",
        ])

    def test_keeps_continue_on_failure_option(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--continueOnFailure",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "--continueOnFailure",
        ])

    def test_keeps_exclude_with_any_tags_option(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--excludeWithAnyTags=tag1,tag2,tag4",
            "--excludeWithAnyTags=tag3,tag5",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "--excludeWithAnyTags=tag1,tag2,tag4",
            "--excludeWithAnyTags=tag3,tag5",
        ])

    def test_keeps_include_with_any_tags_option(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--includeWithAnyTags=tag1,tag2,tag4",
            "--includeWithAnyTags=tag3,tag5",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "--includeWithAnyTags=tag1,tag2,tag4",
            "--includeWithAnyTags=tag3,tag5",
        ])

    def test_keeps_no_journal_option(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--nojournal",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "--nojournal",
        ])

    def test_keeps_num_clients_per_fixture_option(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--numClientsPerFixture=10",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "--numClientsPerFixture=10",
        ])

    def test_keeps_repeat_options(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--repeatSuites=1000",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "--repeat=1000",
        ])

        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--repeatTests=1000",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "--repeatTests=1000",
        ])

        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--repeatTestsMax=1000",
            "--repeatTestsMin=20",
            "--repeatTestsSecs=300",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "--repeatTestsMax=1000",
            "--repeatTestsMin=20",
            "--repeatTestsSecs=300.0",
        ])

    def test_keeps_shuffle_option(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--shuffle",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "--shuffleMode=on",
        ])

    def test_keeps_storage_engine_cache_size_option(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--storageEngineCacheSizeGB=1",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, [
            "--suites=my_suite",
            "--storageEngine=my_storage_engine",
            "--storageEngineCacheSizeGB=1",
        ])

    def test_origin_suite_option_replaces_suite_option(self):
        cmdline = _parser.to_local_args([
            # We intentionally say --suite rather than --suites here to protect against this command
            # line option from becoming ambiguous if more similarly named command line options are
            # added in the future.
            "--suite=part_of_my_suite",
            "--originSuite=my_entire_suite",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, ["--suites=my_entire_suite", "--storageEngine=my_storage_engine"])

    def test_removes_archival_options(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--archiveFile=archive.json",
            "--archiveLimitMb=100",
            "--archiveLimitTests=10",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, ["--suites=my_suite", "--storageEngine=my_storage_engine"])

    def test_removes_evergreen_options(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--buildId=some_build_id",
            "--distroId=some_distro_id",
            "--executionNumber=1",
            "--gitRevision=c0de",
            "--patchBuild",
            "--projectName=some_project",
            "--revisionOrderId=20",
            "--taskName=some_task",
            "--taskId=some_task_id",
            "--variantName=some_variant",
            "--versionId=some_version_id",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, ["--suites=my_suite", "--storageEngine=my_storage_engine"])

    def test_removes_log_option(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--log=buildlogger",
            "--buildloggerUrl=some_url",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, ["--suites=my_suite", "--storageEngine=my_storage_engine"])

    def test_removes_report_file_options(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--reportFailureStatus=fail",
            "--reportFile=report.json",
            "--perfReportFile=perf.json",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, ["--suites=my_suite", "--storageEngine=my_storage_engine"])

    def test_removes_stagger_jobs_option(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--staggerJobs=on",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, ["--suites=my_suite", "--storageEngine=my_storage_engine"])

    def test_removes_tag_file_option(self):
        cmdline = _parser.to_local_args([
            "--suites=my_suite",
            "--tagFile=etc/test_retrial.yml",
            "--storageEngine=my_storage_engine",
        ])

        self.assertEqual(cmdline, ["--suites=my_suite", "--storageEngine=my_storage_engine"])
