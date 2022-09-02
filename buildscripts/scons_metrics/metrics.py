"""SCons metrics."""
import re
from typing import Optional, NamedTuple, List, Pattern, AnyStr

from buildscripts.util.cedar_report import CedarMetric, CedarTestReport

SCONS_METRICS_REGEX = re.compile(r"scons: done building targets\.((\n.*)*)", re.MULTILINE)

MEMORY_BEFORE_READING_SCONSCRIPT_FILES_REGEX = re.compile(
    r"Memory before reading SConscript files:(.+)")
MEMORY_AFTER_READING_SCONSCRIPT_FILES_REGEX = re.compile(
    r"Memory after reading SConscript files:(.+)")
MEMORY_BEFORE_BUILDING_TARGETS_REGEX = re.compile(r"Memory before building targets:(.+)")
MEMORY_AFTER_BUILDING_TARGETS_REGEX = re.compile(r"Memory after building targets:(.+)")
OBJECT_COUNTS_REGEX = re.compile(r"Object counts:(\n.*)+Class\n(^[^:]+$)", re.MULTILINE)
TOTAL_BUILD_TIME_REGEX = re.compile(r"Total build time:(.+)seconds")
TOTAL_SCONSCRIPT_FILE_EXECUTION_TIME_REGEX = re.compile(
    r"Total SConscript file execution time:(.+)seconds")
TOTAL_SCONS_EXECUTION_TIME_REGEX = re.compile(r"Total SCons execution time:(.+)seconds")
TOTAL_COMMAND_EXECUTION_TIME_REGEX = re.compile(r"Total command execution time:(.+)seconds")

CACHE_HIT_RATIO_REGEX = re.compile(r"(?s)\.*hit rate: (\d+\.\d+)%(?!.*hit rate: (\d+\.\d+)%)")

DEFAULT_CEDAR_METRIC_TYPE = "THROUGHPUT"


class ObjectCountsMetric(NamedTuple):
    """Class representing Object counts metric."""

    class_: Optional[str]
    pre_read: Optional[int]
    post_read: Optional[int]
    pre_build: Optional[int]
    post_build: Optional[int]

    def as_cedar_report(self) -> CedarTestReport:
        """Return cedar report representation."""
        metrics = [
            CedarMetric(
                name="pre-read object count",
                type=DEFAULT_CEDAR_METRIC_TYPE,
                value=self.pre_read,
            ),
            CedarMetric(
                name="post-read object count",
                type=DEFAULT_CEDAR_METRIC_TYPE,
                value=self.post_read,
            ),
            CedarMetric(
                name="pre-build object count",
                type=DEFAULT_CEDAR_METRIC_TYPE,
                value=self.pre_build,
            ),
            CedarMetric(
                name="post-build object count",
                type=DEFAULT_CEDAR_METRIC_TYPE,
                value=self.post_build,
            ),
        ]

        return CedarTestReport(
            test_name=f"{self.class_} class",
            thread_level=1,
            metrics=metrics,
        )


class SconsMetrics:
    """Class representing SCons metrics."""

    memory_before_reading_sconscript_files: Optional[int] = None
    memory_after_reading_sconscript_files: Optional[int] = None
    memory_before_building_targets: Optional[int] = None
    memory_after_building_targets: Optional[int] = None
    object_counts: List[ObjectCountsMetric] = None
    total_build_time: Optional[float] = None
    total_sconscript_file_execution_time: Optional[float] = None
    total_scons_execution_time: Optional[float] = None
    total_command_execution_time: Optional[float] = None
    final_cache_hit_ratio: Optional[float] = None

    def __init__(self, stdout_log_file, cache_debug_log_file):
        """Init."""
        with open(stdout_log_file, "r") as fh:
            res = SCONS_METRICS_REGEX.search(fh.read())
            self.raw_report = res.group(1).strip() if res else ""

        if self.raw_report:
            self.memory_before_reading_sconscript_files = self._parse_int(
                MEMORY_BEFORE_READING_SCONSCRIPT_FILES_REGEX, self.raw_report)
            self.memory_after_reading_sconscript_files = self._parse_int(
                MEMORY_AFTER_READING_SCONSCRIPT_FILES_REGEX, self.raw_report)
            self.memory_before_building_targets = self._parse_int(
                MEMORY_BEFORE_BUILDING_TARGETS_REGEX, self.raw_report)
            self.memory_after_building_targets = self._parse_int(
                MEMORY_AFTER_BUILDING_TARGETS_REGEX, self.raw_report)

            self.object_counts = self._parse_object_counts(OBJECT_COUNTS_REGEX, self.raw_report)

            self.total_build_time = self._parse_float(TOTAL_BUILD_TIME_REGEX, self.raw_report)
            self.total_sconscript_file_execution_time = self._parse_float(
                TOTAL_SCONSCRIPT_FILE_EXECUTION_TIME_REGEX, self.raw_report)
            self.total_scons_execution_time = self._parse_float(TOTAL_SCONS_EXECUTION_TIME_REGEX,
                                                                self.raw_report)
            self.total_command_execution_time = self._parse_float(
                TOTAL_COMMAND_EXECUTION_TIME_REGEX, self.raw_report)

        if cache_debug_log_file:
            with open(cache_debug_log_file, "r") as fh:
                self.final_cache_hit_ratio = self._parse_float(CACHE_HIT_RATIO_REGEX, fh.read())
        else:
            self.final_cache_hit_ratio = 0.0

    def make_cedar_report(self) -> List[dict]:
        """Format the data to look like a cedar report json."""
        cedar_report = []
        if not self.raw_report:
            return cedar_report

        if self.memory_before_reading_sconscript_files:
            cedar_report.append(
                CedarTestReport(
                    test_name="Memory before reading SConscript files",
                    thread_level=1,
                    metrics=[
                        CedarMetric(
                            name="bytes",
                            type=DEFAULT_CEDAR_METRIC_TYPE,
                            value=self.memory_before_reading_sconscript_files,
                        )
                    ],
                ).as_dict())

        if self.memory_after_reading_sconscript_files:
            cedar_report.append(
                CedarTestReport(
                    test_name="Memory after reading SConscript files",
                    thread_level=1,
                    metrics=[
                        CedarMetric(
                            name="bytes",
                            type=DEFAULT_CEDAR_METRIC_TYPE,
                            value=self.memory_after_reading_sconscript_files,
                        )
                    ],
                ).as_dict())

        if self.memory_before_building_targets:
            cedar_report.append(
                CedarTestReport(
                    test_name="Memory before building targets",
                    thread_level=1,
                    metrics=[
                        CedarMetric(
                            name="bytes",
                            type=DEFAULT_CEDAR_METRIC_TYPE,
                            value=self.memory_before_building_targets,
                        )
                    ],
                ).as_dict())

        if self.memory_after_building_targets:
            cedar_report.append(
                CedarTestReport(
                    test_name="Memory after building targets",
                    thread_level=1,
                    metrics=[
                        CedarMetric(
                            name="bytes",
                            type=DEFAULT_CEDAR_METRIC_TYPE,
                            value=self.memory_after_building_targets,
                        )
                    ],
                ).as_dict())

        if self.total_build_time:
            cedar_report.append(
                CedarTestReport(
                    test_name="Total build time",
                    thread_level=1,
                    metrics=[
                        CedarMetric(
                            name="seconds",
                            type=DEFAULT_CEDAR_METRIC_TYPE,
                            value=self.total_build_time,
                        )
                    ],
                ).as_dict())

        if self.total_sconscript_file_execution_time:
            cedar_report.append(
                CedarTestReport(
                    test_name="Total SConscript file execution time",
                    thread_level=1,
                    metrics=[
                        CedarMetric(
                            name="seconds",
                            type=DEFAULT_CEDAR_METRIC_TYPE,
                            value=self.total_sconscript_file_execution_time,
                        )
                    ],
                ).as_dict())

        if self.total_scons_execution_time:
            cedar_report.append(
                CedarTestReport(
                    test_name="Total SCons execution time",
                    thread_level=1,
                    metrics=[
                        CedarMetric(
                            name="seconds",
                            type=DEFAULT_CEDAR_METRIC_TYPE,
                            value=self.total_scons_execution_time,
                        )
                    ],
                ).as_dict())

        if self.total_command_execution_time:
            cedar_report.append(
                CedarTestReport(
                    test_name="Total command execution time",
                    thread_level=1,
                    metrics=[
                        CedarMetric(
                            name="seconds",
                            type=DEFAULT_CEDAR_METRIC_TYPE,
                            value=self.total_command_execution_time,
                        )
                    ],
                ).as_dict())

        if self.object_counts:
            for obj_counts in self.object_counts:
                cedar_report.append(obj_counts.as_cedar_report().as_dict())

        if self.final_cache_hit_ratio:
            cedar_report.append(
                CedarTestReport(
                    test_name="Final cache hit ratio",
                    thread_level=1,
                    metrics=[
                        CedarMetric(
                            name="percent",
                            type=DEFAULT_CEDAR_METRIC_TYPE,
                            value=self.final_cache_hit_ratio,
                        ),
                    ],
                ).as_dict())

        return cedar_report

    @classmethod
    def _parse_int(cls, regex: Pattern[AnyStr], raw_str: str) -> Optional[int]:
        """Parse int value."""
        res = regex.search(raw_str)
        if res:
            return int(res.group(1).strip())
        return None

    @classmethod
    def _parse_float(cls, regex: Pattern[AnyStr], raw_str: str) -> Optional[float]:
        """Parse float value."""
        res = regex.search(raw_str)
        if res:
            return float(res.group(1).strip())
        return None

    @classmethod
    def _parse_object_counts(cls, regex: Pattern[AnyStr], raw_str: str) -> List[ObjectCountsMetric]:
        """Parse object counts metrics."""
        object_counts = []
        res = regex.search(raw_str)
        if res:
            object_counts_raw = res.group(2)
            for line in object_counts_raw.splitlines():
                line_split = line.split()
                if len(line_split) == 5:
                    object_counts.append(
                        ObjectCountsMetric(
                            class_=line_split[4],
                            pre_read=int(line_split[0]),
                            post_read=int(line_split[1]),
                            pre_build=int(line_split[2]),
                            post_build=int(line_split[3]),
                        ))
        return object_counts
