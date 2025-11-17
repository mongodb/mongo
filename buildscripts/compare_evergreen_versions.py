#!/usr/bin/env python3
"""
Script to compare task build times between two Evergreen versions.

Usage:
    python3 buildscripts/compare_evergreen_versions.py <version_id_1> <version_id_2> [options]

Examples:
    # Compare two versions
    python3 buildscripts/compare_evergreen_versions.py 507f1f77bcf86cd799439011 507f191e810c19729de860ea

    # Compare with specific output format
    python3 buildscripts/compare_evergreen_versions.py version1 version2 --format json

    # Filter by task name pattern
    python3 buildscripts/compare_evergreen_versions.py version1 version2 --filter "jscore"

    # Show only tasks with significant differences (>5% change)
    python3 buildscripts/compare_evergreen_versions.py version1 version2 --min-diff 5

    # Sort by largest time difference (absolute value)
    python3 buildscripts/compare_evergreen_versions.py version1 version2 --sort diff-abs

    # Sort by largest percentage difference (absolute value)
    python3 buildscripts/compare_evergreen_versions.py version1 version2 --sort diff-pct-abs

    # Sort by V2 time (slowest tasks first)
    python3 buildscripts/compare_evergreen_versions.py version1 version2 --sort v2-time
"""

import argparse
import csv
import json
import os
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional

# Add the repository root to sys.path to allow imports from buildscripts
_SCRIPT_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _SCRIPT_DIR.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from buildscripts.resmokelib.utils import evergreen_conn
from evergreen import RetryingEvergreenApi, Task, Version

# Constants
TASK_NAME_MAX_LENGTH = 50
VARIANT_NAME_MAX_LENGTH = 50
PROGRESS_UPDATE_INTERVAL = 10
TOP_N_TASKS = 10
NON_RUNNING_STATUSES = {"undispatched", "unscheduled", "inactive", "aborted"}

# Type alias for task dictionary key
TaskKey = tuple[str, str]  # (task_display_name, build_variant)


@dataclass
class TaskStats:
    """Statistics for a single task."""

    task_id: str
    display_name: str
    status: str
    time_taken_ms: Optional[int]
    time_taken_seconds: Optional[float]
    build_variant: str
    execution: int

    @property
    def time_taken_minutes(self) -> Optional[float]:
        """Return time taken in minutes."""
        if self.time_taken_seconds is not None:
            return self.time_taken_seconds / 60.0
        return None


@dataclass
class TaskComparison:
    """Comparison between two versions of the same task."""

    task_name: str
    build_variant: str
    version1_stats: Optional[TaskStats]
    version2_stats: Optional[TaskStats]

    @property
    def time_diff_seconds(self) -> Optional[float]:
        """Return time difference in seconds (version2 - version1)."""
        if (
            self.version1_stats
            and self.version2_stats
            and self.version1_stats.time_taken_seconds is not None
            and self.version2_stats.time_taken_seconds is not None
        ):
            return self.version2_stats.time_taken_seconds - self.version1_stats.time_taken_seconds
        return None

    @property
    def time_diff_percent(self) -> Optional[float]:
        """Return percentage change in time (positive = slower, negative = faster)."""
        if (
            self.version1_stats
            and self.version2_stats
            and self.version1_stats.time_taken_seconds is not None
            and self.version2_stats.time_taken_seconds is not None
            and self.version1_stats.time_taken_seconds > 0
        ):
            return (
                (self.version2_stats.time_taken_seconds - self.version1_stats.time_taken_seconds)
                / self.version1_stats.time_taken_seconds
            ) * 100
        return None

    @property
    def status_changed(self) -> bool:
        """Check if task status changed between versions."""
        if self.version1_stats and self.version2_stats:
            return self.version1_stats.status != self.version2_stats.status
        return False


def _fetch_build_tasks(
    evg_api: RetryingEvergreenApi,
    build_variant_name: str,
    build_id: str,
    non_running_statuses: set[str],
) -> List[TaskStats]:
    """
    Fetch tasks for a single build variant.

    Returns a list of TaskStats for tasks that actually ran.
    """
    try:
        build = evg_api.build_by_id(build_id)
        tasks: List[Task] = build.get_tasks()

        task_stats_list = []
        for task in tasks:
            # Skip tasks that didn't actually run
            if task.status in non_running_statuses:
                continue

            # Convert milliseconds to seconds
            time_seconds = None
            if task.time_taken_ms is not None:
                time_seconds = task.time_taken_ms / 1000.0

            task_stats = TaskStats(
                task_id=task.task_id,
                display_name=task.display_name,
                status=task.status,
                time_taken_ms=task.time_taken_ms,
                time_taken_seconds=time_seconds,
                build_variant=build_variant_name,
                execution=task.execution,
            )
            task_stats_list.append(task_stats)

        return task_stats_list
    except Exception as e:
        print(
            f"Warning: Could not fetch tasks for build variant {build_variant_name}: {e}",
            file=sys.stderr,
        )
        return []


def get_version_tasks(
    evg_api: RetryingEvergreenApi, version_id: str, max_workers: int = 10
) -> Dict[TaskKey, TaskStats]:
    """
    Fetch all tasks for a given version using parallel requests.

    Returns a dictionary mapping (task_display_name, build_variant) -> TaskStats
    Only includes tasks that were actually scheduled and ran for this version.
    """
    print(f"Fetching tasks for version {version_id}...", file=sys.stderr)

    try:
        version: Version = evg_api.version_by_id(version_id)
    except Exception as e:
        print(f"Error fetching version {version_id}: {e}", file=sys.stderr)
        sys.exit(1)

    print(
        f"Processing {len(version.build_variants_map)} build variants...",
        file=sys.stderr,
    )

    tasks_dict = {}

    # Try to use get_builds() for batch fetching (more efficient than individual calls)
    try:
        print("  Fetching all builds in batch...", file=sys.stderr)
        builds = list(version.get_builds())
        print(f"  Retrieved {len(builds)} builds", file=sys.stderr)

        # Create a mapping of build_id to build_variant_name
        build_id_to_variant = {
            build_id: variant_name for variant_name, build_id in version.build_variants_map.items()
        }

        # Process builds in parallel to get tasks
        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            future_to_build = {}
            for build in builds:
                variant_name = build_id_to_variant.get(build.id)
                if variant_name:
                    future = executor.submit(
                        _fetch_build_tasks,
                        evg_api,
                        variant_name,
                        build.id,
                        NON_RUNNING_STATUSES,
                    )
                    future_to_build[future] = variant_name

            # Collect results as they complete
            completed = 0
            for future in as_completed(future_to_build):
                build_variant_name = future_to_build[future]
                completed += 1

                try:
                    task_stats_list = future.result()

                    # Add tasks to dictionary
                    for task_stats in task_stats_list:
                        key = (task_stats.display_name, build_variant_name)
                        tasks_dict[key] = task_stats

                    if completed % PROGRESS_UPDATE_INTERVAL == 0 or completed == len(
                        future_to_build
                    ):
                        print(
                            f"  Progress: {completed}/{len(future_to_build)} variants processed",
                            file=sys.stderr,
                        )
                except Exception as e:
                    print(
                        f"  Error processing build variant {build_variant_name}: {e}",
                        file=sys.stderr,
                    )

    except Exception as e:
        # Fallback to individual build fetching if batch method fails
        print(f"  Batch fetch failed ({e}), falling back to individual fetches...", file=sys.stderr)

        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            # Submit all build fetch tasks
            future_to_variant = {
                executor.submit(
                    _fetch_build_tasks, evg_api, build_variant_name, build_id, NON_RUNNING_STATUSES
                ): build_variant_name
                for build_variant_name, build_id in version.build_variants_map.items()
            }

            # Collect results as they complete
            completed = 0
            for future in as_completed(future_to_variant):
                build_variant_name = future_to_variant[future]
                completed += 1

                try:
                    task_stats_list = future.result()

                    # Add tasks to dictionary
                    for task_stats in task_stats_list:
                        key = (task_stats.display_name, build_variant_name)
                        tasks_dict[key] = task_stats

                    if completed % PROGRESS_UPDATE_INTERVAL == 0 or completed == len(
                        future_to_variant
                    ):
                        print(
                            f"  Progress: {completed}/{len(future_to_variant)} variants processed",
                            file=sys.stderr,
                        )
                except Exception as e:
                    print(
                        f"  Error processing build variant {build_variant_name}: {e}",
                        file=sys.stderr,
                    )

    print(f"Found {len(tasks_dict)} tasks that ran for version {version_id}", file=sys.stderr)
    return tasks_dict


def compare_versions(
    evg_api: RetryingEvergreenApi,
    version_id_1: str,
    version_id_2: str,
    task_filter: Optional[str] = None,
    max_workers: int = 10,
) -> List[TaskComparison]:
    """
    Compare tasks between two versions.

    Args:
        evg_api: Evergreen API client
        version_id_1: First version ID (baseline)
        version_id_2: Second version ID (comparison)
        task_filter: Optional substring to filter task names
        max_workers: Maximum number of parallel API requests

    Returns:
        List of TaskComparison objects
    """
    version1_tasks = get_version_tasks(evg_api, version_id_1, max_workers=max_workers)
    version2_tasks = get_version_tasks(evg_api, version_id_2, max_workers=max_workers)

    # Get all unique task keys (task_name, build_variant)
    all_keys = set(version1_tasks.keys()) | set(version2_tasks.keys())

    comparisons = []
    for key in sorted(all_keys):
        task_name, build_variant = key

        # Apply filter if specified
        if task_filter and task_filter.lower() not in task_name.lower():
            continue

        comparison = TaskComparison(
            task_name=task_name,
            build_variant=build_variant,
            version1_stats=version1_tasks.get(key),
            version2_stats=version2_tasks.get(key),
        )
        comparisons.append(comparison)

    return comparisons


def format_time(seconds: Optional[float]) -> str:
    """Format time in a human-readable way."""
    if seconds is None:
        return "N/A"

    if seconds < 60:
        return f"{seconds:.1f}s"
    elif seconds < 3600:
        minutes = seconds / 60
        return f"{minutes:.1f}m"
    else:
        hours = seconds / 3600
        return f"{hours:.2f}h"


def format_diff(diff_seconds: Optional[float], diff_percent: Optional[float]) -> str:
    """Format time difference with percentage."""
    if diff_seconds is None or diff_percent is None:
        return "N/A"

    sign = "+" if diff_seconds >= 0 else "-"
    time_str = format_time(abs(diff_seconds))
    return f"{sign}{time_str} ({sign}{abs(diff_percent):.1f}%)"


def truncate_task_name(task_name: str) -> str:
    """Truncate task name if it exceeds maximum length."""
    if len(task_name) > TASK_NAME_MAX_LENGTH:
        return task_name[: TASK_NAME_MAX_LENGTH - 3] + "..."
    return task_name


def truncate_variant_name(variant_name: str) -> str:
    """Truncate variant name if it exceeds maximum length."""
    if len(variant_name) > VARIANT_NAME_MAX_LENGTH:
        return variant_name[: VARIANT_NAME_MAX_LENGTH - 3] + "..."
    return variant_name


def print_table_output(
    comparisons: List[TaskComparison],
    version_id_1: str,
    version_id_2: str,
    min_diff_percent: float = 0.0,
    show_only_changed: bool = False,
    sort_by: str = "task",
):
    """Print comparison results in a formatted table."""
    print(f"\n{'=' * 100}")
    print("Comparing Evergreen Versions")
    print(f"{'=' * 100}")
    print(f"Version 1 (Baseline): {version_id_1}")
    print(f"Version 2 (Compare):  {version_id_2}")
    print(f"{'=' * 100}\n")

    # Filter comparisons based on criteria
    filtered_comparisons = []
    for comp in comparisons:
        if show_only_changed and not comp.status_changed:
            continue

        if comp.time_diff_percent is not None:
            if abs(comp.time_diff_percent) < min_diff_percent:
                continue

        filtered_comparisons.append(comp)

    if not filtered_comparisons:
        print("No tasks match the specified criteria.")
        return

    # Sort comparisons based on the specified sort order
    def get_sort_key(comp: TaskComparison):
        if sort_by == "task":
            return (comp.build_variant, comp.task_name)
        elif sort_by == "v1-time":
            # Put None values at the end
            v1_time = comp.version1_stats.time_taken_seconds if comp.version1_stats else None
            return (comp.build_variant, v1_time if v1_time is not None else float("inf"))
        elif sort_by == "v2-time":
            v2_time = comp.version2_stats.time_taken_seconds if comp.version2_stats else None
            return (comp.build_variant, v2_time if v2_time is not None else float("inf"))
        elif sort_by == "diff":
            diff = comp.time_diff_seconds
            return (comp.build_variant, diff if diff is not None else float("inf"))
        elif sort_by == "diff-abs":
            diff = comp.time_diff_seconds
            abs_diff = abs(diff) if diff is not None else float("inf")
            return (comp.build_variant, abs_diff)
        elif sort_by == "diff-pct":
            diff_pct = comp.time_diff_percent
            return (comp.build_variant, diff_pct if diff_pct is not None else float("inf"))
        elif sort_by == "diff-pct-abs":
            diff_pct = comp.time_diff_percent
            abs_diff_pct = abs(diff_pct) if diff_pct is not None else float("inf")
            return (comp.build_variant, abs_diff_pct)
        return (comp.build_variant, comp.task_name)

    # Sort, with reverse=True for time-based sorts (slowest first)
    reverse_sort = sort_by in ["v1-time", "v2-time", "diff", "diff-abs", "diff-pct", "diff-pct-abs"]

    # Print header
    header = f"{'Task Name':<{TASK_NAME_MAX_LENGTH}} {'V1 Time':<12} {'V2 Time':<12} {'Difference':<20} {'Status':<15}"
    print(header)
    print("-" * len(header))

    # Group by build variant for better readability
    by_variant = defaultdict(list)
    for comp in filtered_comparisons:
        by_variant[comp.build_variant].append(comp)

    # Sort tasks within each variant
    for variant in by_variant:
        by_variant[variant].sort(key=lambda c: get_sort_key(c)[1], reverse=reverse_sort)

    # Print tasks grouped by variant
    for variant in sorted(by_variant.keys()):
        print(f"\n{variant}:")
        print("-" * len(header))

        for comp in by_variant[variant]:
            v1_time = (
                format_time(comp.version1_stats.time_taken_seconds)
                if comp.version1_stats
                else "N/A"
            )
            v2_time = (
                format_time(comp.version2_stats.time_taken_seconds)
                if comp.version2_stats
                else "N/A"
            )
            diff = format_diff(comp.time_diff_seconds, comp.time_diff_percent)

            # Status indicators
            status_parts = []
            if comp.version1_stats:
                status_parts.append(f"V1:{comp.version1_stats.status}")
            if comp.version2_stats:
                status_parts.append(f"V2:{comp.version2_stats.status}")
            status = " | ".join(status_parts) if status_parts else "N/A"

            # Truncate task name if too long
            task_name = truncate_task_name(comp.task_name)

            print(
                f"{task_name:<{TASK_NAME_MAX_LENGTH}} {v1_time:<12} {v2_time:<12} {diff:<20} {status:<15}"
            )

    # Print summary statistics
    print(f"\n{'=' * 100}")
    print("Summary Statistics")
    print(f"{'=' * 100}")

    total_tasks = len(filtered_comparisons)
    tasks_in_both = sum(1 for c in filtered_comparisons if c.version1_stats and c.version2_stats)
    tasks_only_v1 = sum(
        1 for c in filtered_comparisons if c.version1_stats and not c.version2_stats
    )
    tasks_only_v2 = sum(
        1 for c in filtered_comparisons if not c.version1_stats and c.version2_stats
    )

    print(f"Total tasks compared: {total_tasks}")
    print(f"Tasks in both versions: {tasks_in_both}")
    print(f"Tasks only in version 1: {tasks_only_v1}")
    print(f"Tasks only in version 2: {tasks_only_v2}")

    # Calculate aggregate statistics for tasks in both versions
    time_diffs = [
        c.time_diff_seconds for c in filtered_comparisons if c.time_diff_seconds is not None
    ]
    percent_diffs = [
        c.time_diff_percent for c in filtered_comparisons if c.time_diff_percent is not None
    ]

    if time_diffs:
        avg_time_diff = sum(time_diffs) / len(time_diffs)
        total_time_diff = sum(time_diffs)
        print(f"\nAverage time difference: {format_time(avg_time_diff)}")
        print(f"Total time difference: {format_time(total_time_diff)}")

    if percent_diffs:
        avg_percent_diff = sum(percent_diffs) / len(percent_diffs)
        print(f"Average percentage change: {avg_percent_diff:+.1f}%")

        faster_tasks = sum(1 for d in percent_diffs if d < 0)
        slower_tasks = sum(1 for d in percent_diffs if d > 0)
        print(f"\nTasks faster in V2: {faster_tasks}")
        print(f"Tasks slower in V2: {slower_tasks}")

    # Show top N improvements and regressions
    comparisons_with_diff = [c for c in filtered_comparisons if c.time_diff_percent is not None]

    if comparisons_with_diff:
        print(f"\n{'=' * 100}")
        print(f"Top {TOP_N_TASKS} Improvements (Faster in V2)")
        print(f"{'=' * 100}")
        improvements = sorted(comparisons_with_diff, key=lambda c: c.time_diff_percent)[
            :TOP_N_TASKS
        ]
        for comp in improvements:
            if comp.time_diff_percent < 0:
                task_name = truncate_task_name(comp.task_name)
                variant = truncate_variant_name(comp.build_variant)
                print(
                    f"  {task_name:<{TASK_NAME_MAX_LENGTH}} {variant:<{VARIANT_NAME_MAX_LENGTH}} {format_diff(comp.time_diff_seconds, comp.time_diff_percent)}"
                )

        print(f"\n{'=' * 100}")
        print(f"Top {TOP_N_TASKS} Regressions (Slower in V2)")
        print(f"{'=' * 100}")
        regressions = sorted(
            comparisons_with_diff, key=lambda c: c.time_diff_percent, reverse=True
        )[:TOP_N_TASKS]
        for comp in regressions:
            if comp.time_diff_percent > 0:
                task_name = truncate_task_name(comp.task_name)
                variant = truncate_variant_name(comp.build_variant)
                print(
                    f"  {task_name:<{TASK_NAME_MAX_LENGTH}} {variant:<{VARIANT_NAME_MAX_LENGTH}} {format_diff(comp.time_diff_seconds, comp.time_diff_percent)}"
                )


def print_json_output(
    comparisons: List[TaskComparison],
    version_id_1: str,
    version_id_2: str,
):
    """Print comparison results in JSON format."""
    output = {
        "version1": version_id_1,
        "version2": version_id_2,
        "comparisons": [],
    }

    for comp in comparisons:
        comparison_dict = {
            "task_name": comp.task_name,
            "build_variant": comp.build_variant,
            "version1": {
                "task_id": comp.version1_stats.task_id if comp.version1_stats else None,
                "status": comp.version1_stats.status if comp.version1_stats else None,
                "time_taken_seconds": comp.version1_stats.time_taken_seconds
                if comp.version1_stats
                else None,
            },
            "version2": {
                "task_id": comp.version2_stats.task_id if comp.version2_stats else None,
                "status": comp.version2_stats.status if comp.version2_stats else None,
                "time_taken_seconds": comp.version2_stats.time_taken_seconds
                if comp.version2_stats
                else None,
            },
            "difference": {
                "seconds": comp.time_diff_seconds,
                "percent": comp.time_diff_percent,
            },
            "status_changed": comp.status_changed,
        }
        output["comparisons"].append(comparison_dict)

    print(json.dumps(output, indent=2))


def print_csv_output(
    comparisons: List[TaskComparison],
    version_id_1: str,
    version_id_2: str,
):
    """Print comparison results in CSV format."""
    writer = csv.writer(sys.stdout)

    # Write header
    writer.writerow(
        [
            "task_name",
            "build_variant",
            "v1_time_seconds",
            "v2_time_seconds",
            "diff_seconds",
            "diff_percent",
            "v1_status",
            "v2_status",
        ]
    )

    # Write data rows
    for comp in comparisons:
        v1_time = comp.version1_stats.time_taken_seconds if comp.version1_stats else ""
        v2_time = comp.version2_stats.time_taken_seconds if comp.version2_stats else ""
        diff_seconds = comp.time_diff_seconds if comp.time_diff_seconds is not None else ""
        diff_percent = comp.time_diff_percent if comp.time_diff_percent is not None else ""
        v1_status = comp.version1_stats.status if comp.version1_stats else ""
        v2_status = comp.version2_stats.status if comp.version2_stats else ""

        writer.writerow(
            [
                comp.task_name,
                comp.build_variant,
                v1_time,
                v2_time,
                diff_seconds,
                diff_percent,
                v1_status,
                v2_status,
            ]
        )


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Compare task build times between two Evergreen versions",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    parser.add_argument(
        "version_id_1",
        help="First version ID (baseline)",
    )

    parser.add_argument(
        "version_id_2",
        help="Second version ID (comparison)",
    )

    parser.add_argument(
        "--format",
        choices=["table", "json", "csv"],
        default="table",
        help="Output format (default: table)",
    )

    parser.add_argument(
        "--filter",
        help="Filter tasks by name (case-insensitive substring match)",
    )

    parser.add_argument(
        "--min-diff",
        type=float,
        default=0.0,
        help="Minimum percentage difference to show (default: 0.0)",
    )

    parser.add_argument(
        "--only-changed",
        action="store_true",
        help="Show only tasks with status changes",
    )

    parser.add_argument(
        "--sort",
        choices=["task", "v1-time", "v2-time", "diff", "diff-abs", "diff-pct", "diff-pct-abs"],
        default="task",
        help="Sort order for table output: task (name), v1-time, v2-time, diff (signed time), diff-abs (absolute time), diff-pct (signed percent), diff-pct-abs (absolute percent) (default: task)",
    )

    parser.add_argument(
        "--evergreen-config",
        help="Path to .evergreen.yml config file (default: auto-detect)",
    )

    parser.add_argument(
        "--max-workers",
        type=int,
        default=10,
        help="Maximum number of parallel API requests (default: 10)",
    )

    args = parser.parse_args()

    # If no config specified, check common locations
    evergreen_config = args.evergreen_config
    if not evergreen_config:
        common_locations = [
            os.path.expanduser("~/.evergreen.yml"),
            os.path.join(os.getcwd(), ".evergreen.yml"),
        ]
        for location in common_locations:
            if os.path.isfile(location):
                evergreen_config = location
                break

    # Get Evergreen API client
    try:
        evg_api = evergreen_conn.get_evergreen_api(evergreen_config)
    except Exception as e:
        print(
            f"Error: Could not connect to Evergreen API: {e}\n\n"
            "Make sure you have a .evergreen.yml file configured with your API credentials.\n"
            "See: https://github.com/evergreen-ci/evergreen/wiki/Using-the-Command-Line-Tool#downloading-the-command-line-tool",
            file=sys.stderr,
        )
        sys.exit(1)

    # Compare versions
    comparisons = compare_versions(
        evg_api,
        args.version_id_1,
        args.version_id_2,
        task_filter=args.filter,
        max_workers=args.max_workers,
    )

    # Output results
    if args.format == "json":
        print_json_output(comparisons, args.version_id_1, args.version_id_2)
    elif args.format == "csv":
        print_csv_output(comparisons, args.version_id_1, args.version_id_2)
    else:  # table
        print_table_output(
            comparisons,
            args.version_id_1,
            args.version_id_2,
            min_diff_percent=args.min_diff,
            show_only_changed=args.only_changed,
            sort_by=args.sort,
        )


if __name__ == "__main__":
    main()
