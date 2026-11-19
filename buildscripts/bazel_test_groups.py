"""Partition the tests selected by a successful build without changing task filters."""

import argparse
import json
from pathlib import Path


def partition_tests(build_events: Path) -> tuple[list[str], list[str]]:
    """Return regular and timing-sensitive tests that Bazel successfully built."""
    configured_tests: dict[str, bool] = {}
    built_targets: set[str] = set()
    with build_events.open(encoding="utf-8") as events:
        for line in events:
            event = json.loads(line)
            event_id = event.get("id", {})
            configured = event.get("configured", {})
            if "testSize" in configured:
                label = event_id["targetConfigured"]["label"]
                configured_tests[label] = "windows_timing_sensitive" in configured.get("tag", [])
            if event.get("completed", {}).get("success"):
                built_targets.add(event_id["targetCompleted"]["label"])

    regular: list[str] = []
    timing_sensitive: list[str] = []
    for label in sorted(configured_tests.keys() & built_targets):
        group = timing_sensitive if configured_tests[label] else regular
        group.append(label)
    return regular, timing_sensitive


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_events", type=Path)
    parser.add_argument("regular_targets", type=Path)
    parser.add_argument("timing_sensitive_targets", type=Path)
    args = parser.parse_args()

    regular, timing_sensitive = partition_tests(args.build_events)
    for path, labels in (
        (args.regular_targets, regular),
        (args.timing_sensitive_targets, timing_sensitive),
    ):
        path.write_text("".join(label + "\n" for label in labels), encoding="utf-8")
    print(f"Selected {len(regular)} regular and {len(timing_sensitive)} timing-sensitive tests.")


if __name__ == "__main__":
    main()
