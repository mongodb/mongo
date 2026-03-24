"""Downloads historic test runtimes for a resmoke suite from S3.

Used as a genrule tool inside resmoke_suite_test to produce a per-suite
historic runtimes JSON file at build time.
"""

import argparse
import json
import urllib.request

S3_BASE = "https://mongo-test-stats.s3.amazonaws.com"

# If run outside of CI, use the following defaults as they should provide reasonably close runtimes.
DEFAULT_PROJECT = "mongodb-mongo-master"
DEFAULT_BUILD_VARIANT = "linux-64-debug-required"


def parse_volatile_status(path):
    result = {}
    try:
        with open(path) as f:
            for line in f:
                parts = line.strip().split(" ", 1)
                if len(parts) == 2:
                    result[parts[0]] = parts[1]
    except OSError:
        pass
    return result


def fetch_stats(url):
    """Fetches and returns parsed stats JSON from url, or None on any error."""
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            stats = json.loads(response.read())
        for s in stats:
            for field in ["num_pass", "num_fail", "max_duration_pass"]:
                s.pop(field, None)
        return stats
    except Exception:
        return None


def short_name(suite):
    """Returns the name portion of a Bazel label, e.g. 'libunwind' from '//pkg:libunwind'."""
    if ":" in suite:
        return suite.split(":")[-1]
    return suite


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", required=True)
    parser.add_argument("--volatile-status", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    volatile = parse_volatile_status(args.volatile_status)
    project = volatile.get("project", "")
    build_variant = volatile.get("build_variant", "")

    # URL fallback order:
    #   If project/build_variant are set:
    #     1. {project}/{build_variant}/{suite}
    #     2. {project}/{build_variant}/{short_name(suite)}  Temporarily useful while migrating tasks from non-bazel to bazel.
    #     3. {DEFAULT_PROJECT}/{build_variant}/{suite}  (omitted if identical to #1), useful for new release branches that have no test runs yet.
    #   Otherwise, which is useful for local workstation execution.
    #     1. {DEFAULT_PROJECT}/{DEFAULT_BUILD_VARIANT}/{suite}
    if project and build_variant:
        seen = set()
        urls = []
        for url in [
            f"{S3_BASE}/{project}/{build_variant}/{args.suite}",
            f"{S3_BASE}/{project}/{build_variant}/{short_name(args.suite)}",
            f"{S3_BASE}/{DEFAULT_PROJECT}/{build_variant}/{args.suite}",
        ]:
            if url not in seen:
                seen.add(url)
                urls.append(url)
    else:
        urls = [f"{S3_BASE}/{DEFAULT_PROJECT}/{DEFAULT_BUILD_VARIANT}/{args.suite}"]

    stats = None
    for url in urls:
        stats = fetch_stats(url)
        if stats is not None:
            break
    if stats is None:
        stats = []

    with open(args.output, "w") as f:
        json.dump(stats, f)


if __name__ == "__main__":
    main()
