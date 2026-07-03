"""Unit tests for buildscripts/benchmark_profiler.py."""

import gzip
import json
import os
import tarfile
import tempfile
import unittest
from unittest import mock

from buildscripts import benchmark_profiler


class EnsureFlamegraphDirTest(unittest.TestCase):
    def test_returns_cache_dir_when_scripts_present(self):
        # When the cache already holds both scripts, it is returned without cloning (no network).
        with tempfile.TemporaryDirectory() as tmp:
            cache = os.path.join(tmp, "mongo-flamegraph")
            os.mkdir(cache)
            open(os.path.join(cache, "flamegraph.pl"), "w").close()
            open(os.path.join(cache, "stackcollapse-perf.pl"), "w").close()
            with mock.patch.object(benchmark_profiler.tempfile, "gettempdir", return_value=tmp):
                self.assertEqual(benchmark_profiler.ensure_flamegraph_dir(), cache)

    def test_raises_when_cache_dir_missing_stackcollapse(self):
        # A cached clone with flamegraph.pl but not stackcollapse-perf.pl must fail loudly rather than
        # returning an incomplete dir. flamegraph.pl is present so the clone branch is skipped (no
        # network access in this test).
        with tempfile.TemporaryDirectory() as tmp:
            cache = os.path.join(tmp, "mongo-flamegraph")
            os.mkdir(cache)
            open(os.path.join(cache, "flamegraph.pl"), "w").close()
            with mock.patch.object(benchmark_profiler.tempfile, "gettempdir", return_value=tmp):
                with self.assertRaises(FileNotFoundError):
                    benchmark_profiler.ensure_flamegraph_dir()


class BuildPerfRecordCmdTest(unittest.TestCase):
    def test_system_wide_flags(self):
        argv = benchmark_profiler.build_perf_record_cmd(perf_data="/tmp/perf.data")
        self.assertEqual(
            argv,
            [
                "sudo",
                "perf",
                "record",
                "-e",
                "instructions:u",
                "-a",
                "-g",
                "--buildid-all",
                "-F",
                "max",
                "-o",
                "/tmp/perf.data",
            ],
        )


class MergeFoldedFilesTest(unittest.TestCase):
    def test_sums_counts_for_identical_stacks(self):
        with tempfile.TemporaryDirectory() as tmp:
            part1 = os.path.join(tmp, "folded.cpu0")
            part2 = os.path.join(tmp, "folded.cpu1")
            with open(part1, "w") as f:
                f.write("crud_bm;foo 10\ncrud_bm;bar 5\n")
            with open(part2, "w") as f:
                f.write("crud_bm;foo 3\ncrud_bm;baz 7\n")
            merged = os.path.join(tmp, "folded")
            benchmark_profiler._merge_folded_files([part1, part2], merged)
            with open(merged) as f:
                result = {
                    line.rpartition(" ")[0]: int(line.rpartition(" ")[2])
                    for line in f
                    if line.strip()
                }
            self.assertEqual(result, {"crud_bm;foo": 13, "crud_bm;bar": 5, "crud_bm;baz": 7})

    def test_skips_missing_and_empty_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            part1 = os.path.join(tmp, "folded.cpu0")
            part2 = os.path.join(tmp, "folded.cpu1")
            with open(part1, "w") as f:
                f.write("crud_bm;foo 10\n")
            open(part2, "w").close()
            merged = os.path.join(tmp, "folded")
            benchmark_profiler._merge_folded_files(
                [part1, part2, os.path.join(tmp, "nonexistent")], merged
            )
            with open(merged) as f:
                self.assertEqual(f.read(), "crud_bm;foo 10\n")

    def test_filters_out_non_benchmark_processes(self):
        # System noise (mandb, systemd, etc.) captured by system-wide recording must be excluded.
        with tempfile.TemporaryDirectory() as tmp:
            part = os.path.join(tmp, "folded.cpu0")
            with open(part, "w") as f:
                f.write("crud_bm;main;foo 10\n")
                f.write("mandb;update;index 99\n")
                f.write("systemd-journal;write 50\n")
                f.write("service_entry_point_shard_role_bm;run 5\n")
            merged = os.path.join(tmp, "folded")
            benchmark_profiler._merge_folded_files([part], merged)
            with open(merged) as f:
                result = {
                    line.rpartition(" ")[0]: int(line.rpartition(" ")[2])
                    for line in f
                    if line.strip()
                }
            self.assertEqual(
                result, {"crud_bm;main;foo": 10, "service_entry_point_shard_role_bm;run": 5}
            )

    def test_keeps_benchmark_processes_with_digits(self):
        # Benchmark names may contain digits (e.g. storage2_bm); the filter must keep them.
        with tempfile.TemporaryDirectory() as tmp:
            part = os.path.join(tmp, "folded.cpu0")
            with open(part, "w") as f:
                f.write("storage2_bm;main;foo 10\n")
                f.write("mandb;write 99\n")
            merged = os.path.join(tmp, "folded")
            benchmark_profiler._merge_folded_files([part], merged)
            with open(merged) as f:
                result = {
                    line.rpartition(" ")[0]: int(line.rpartition(" ")[2])
                    for line in f
                    if line.strip()
                }
            self.assertEqual(result, {"storage2_bm;main;foo": 10})


class ProfilingEnabledTest(unittest.TestCase):
    def test_enabled_when_flag_true(self):
        self.assertTrue(benchmark_profiler.profiling_enabled('{"enable_linux_perf": true}'))

    def test_disabled_when_flag_false(self):
        self.assertFalse(benchmark_profiler.profiling_enabled('{"enable_linux_perf": false}'))

    def test_disabled_when_flag_is_not_boolean_true(self):
        self.assertFalse(benchmark_profiler.profiling_enabled('{"enable_linux_perf": "true"}'))

    def test_disabled_when_flag_absent(self):
        self.assertFalse(benchmark_profiler.profiling_enabled('{"other": 1}'))

    def test_disabled_for_empty_or_unset_runtime_params(self):
        # Evergreen expands an undefined ${runtime_params_json} to an empty string; a non-profiling
        # run must read as disabled rather than raising on json.loads("").
        for value in ("", "   ", "{}"):
            self.assertFalse(benchmark_profiler.profiling_enabled(value))


class ProfilingLinksTest(unittest.TestCase):
    def test_write_round_trips_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "profiling_links.json")
            manifest = [{"name": "n", "link": "url", "visibility": "public"}]
            benchmark_profiler.write_profiling_links(path, manifest)
            with open(path) as f:
                self.assertEqual(json.load(f), manifest)

    def test_write_empty_manifest(self):
        # The unconditional "Profiling Data" s3.put reads associated_links_file on every run, so a
        # non-profiling run must still leave a valid (empty) links file behind.
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "profiling_links.json")
            benchmark_profiler.write_profiling_links(path, [])
            with open(path) as f:
                self.assertEqual(json.load(f), [])


class GzipFileTest(unittest.TestCase):
    def test_compresses_and_removes_original(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "perf.data")
            with open(src, "wb") as f:
                f.write(b"hello perf")
            out = benchmark_profiler.gzip_file(src)
            self.assertEqual(out, src + ".gz")
            self.assertFalse(os.path.exists(src))
            with open(out, "rb") as f:
                self.assertEqual(gzip.decompress(f.read()), b"hello perf")


class CreateProfilingBundleTest(unittest.TestCase):
    def test_bundles_only_existing_members(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("benchmarks_sep.svg", "perf.data.gz"):
                with open(os.path.join(tmp, name), "w") as f:
                    f.write("x")
            members = ["benchmarks_sep.svg", "benchmarks_sep.folded", "perf.data.gz"]
            out = benchmark_profiler.create_profiling_bundle(
                tmp, "benchmarks_sep_profiling.tar.gz", members
            )
            self.assertEqual(out, os.path.join(tmp, "benchmarks_sep_profiling.tar.gz"))
            with tarfile.open(out, "r:gz") as tar:
                names = tar.getnames()
            self.assertIn("benchmarks_sep.svg", names)
            self.assertIn("perf.data.gz", names)
            # A member that was never produced must be skipped, not cause a failure.
            self.assertNotIn("benchmarks_sep.folded", names)


class StartTaskTest(unittest.TestCase):
    def test_skips_install_and_recording_when_disabled(self):
        with tempfile.TemporaryDirectory() as tmp:
            with (
                mock.patch.object(benchmark_profiler, "ensure_perf_installed") as install,
                mock.patch.object(benchmark_profiler, "start_system_wide_recording") as start,
            ):
                rc = benchmark_profiler.start_task(runtime_params_json="{}", output_dir=tmp)
            self.assertEqual(rc, 0)
            install.assert_not_called()
            start.assert_not_called()

    def test_installs_and_records_when_enabled(self):
        with tempfile.TemporaryDirectory() as tmp:
            with (
                mock.patch.object(benchmark_profiler, "ensure_perf_installed") as install,
                mock.patch.object(
                    benchmark_profiler, "start_system_wide_recording", return_value=123
                ) as start,
            ):
                rc = benchmark_profiler.start_task(
                    runtime_params_json='{"enable_linux_perf": true}', output_dir=tmp
                )
            self.assertEqual(rc, 0)
            install.assert_called_once()
            start.assert_called_once()


class ProcessTaskTest(unittest.TestCase):
    def test_skips_processing_but_writes_empty_links_when_disabled(self):
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(benchmark_profiler, "stop_system_wide_recording") as stop:
                rc = benchmark_profiler.process_task(
                    runtime_params_json="{}",
                    output_dir=tmp,
                    svg_link="svg-url",
                )
            self.assertEqual(rc, 0)
            stop.assert_not_called()
            with open(os.path.join(tmp, "profiling_links.json")) as f:
                self.assertEqual(json.load(f), [])

    def test_fails_when_perf_data_empty(self):
        # Profiling was explicitly requested, so an empty perf.data after stop is a hard failure.
        with tempfile.TemporaryDirectory() as tmp:
            open(os.path.join(tmp, "perf.data"), "wb").close()
            with (
                mock.patch.object(benchmark_profiler, "stop_system_wide_recording"),
                mock.patch.object(benchmark_profiler, "_chown_to_current_user"),
                mock.patch.object(benchmark_profiler, "render_flamegraph") as render,
            ):
                rc = benchmark_profiler.process_task(
                    runtime_params_json='{"enable_linux_perf": true}',
                    output_dir=tmp,
                    svg_link="svg-url",
                )
            self.assertNotEqual(rc, 0)
            render.assert_not_called()

    def test_processes_and_writes_links_when_enabled(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "perf.data"), "wb") as f:
                f.write(b"perf samples")

            with (
                mock.patch.object(benchmark_profiler, "stop_system_wide_recording") as stop,
                mock.patch.object(benchmark_profiler, "_chown_to_current_user"),
                mock.patch.object(benchmark_profiler, "ensure_flamegraph_dir", return_value=tmp),
                mock.patch.object(benchmark_profiler, "render_flamegraph") as render,
            ):
                rc = benchmark_profiler.process_task(
                    runtime_params_json='{"enable_linux_perf": true}',
                    output_dir=tmp,
                    svg_link="svg-url",
                )
            self.assertEqual(rc, 0)
            stop.assert_called_once()
            render.assert_called_once()
            # perf.data is gzipped for upload.
            self.assertTrue(os.path.exists(os.path.join(tmp, "perf.data.gz")))
            with open(os.path.join(tmp, "profiling_links.json")) as f:
                manifest = json.load(f)
            self.assertEqual([entry["link"] for entry in manifest], ["svg-url"])


class MainTest(unittest.TestCase):
    def test_start_task_flag(self):
        with mock.patch.object(benchmark_profiler, "start_task", return_value=0) as start:
            rc = benchmark_profiler.main(
                ["--start-task", '--runtime-params-json={"enable_linux_perf": true}']
            )
        self.assertEqual(rc, 0)
        start.assert_called_once_with(runtime_params_json='{"enable_linux_perf": true}')

    def test_process_task_flag(self):
        with mock.patch.object(benchmark_profiler, "process_task", return_value=0) as process:
            rc = benchmark_profiler.main(
                [
                    "--process-task",
                    "--runtime-params-json={}",
                    "--svg-link=https://example.com/svg",
                ]
            )
        self.assertEqual(rc, 0)
        process.assert_called_once_with(
            runtime_params_json="{}", svg_link="https://example.com/svg"
        )

    def test_process_task_requires_svg_link(self):
        with self.assertRaises(SystemExit):
            benchmark_profiler.main(["--process-task", "--runtime-params-json={}"])

    def test_requires_one_flag(self):
        with self.assertRaises(SystemExit):
            benchmark_profiler.main(["--runtime-params-json={}"])

    def test_mutually_exclusive_flags(self):
        with self.assertRaises(SystemExit):
            benchmark_profiler.main(
                [
                    "--start-task",
                    "--process-task",
                    "--runtime-params-json={}",
                    "--svg-link=url",
                ]
            )


if __name__ == "__main__":
    unittest.main()
