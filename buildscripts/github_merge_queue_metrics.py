import argparse
import json
import os
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path
from statistics import quantiles
from zoneinfo import ZoneInfo

import requests

EST = ZoneInfo("America/New_York")
CACHE_FILE = Path.home() / ".github_merge_queue_metrics.json"


def load_cache():
    """Load the cache from disk."""
    if CACHE_FILE.exists():
        try:
            with open(CACHE_FILE, "r") as f:
                return json.load(f)
        except (json.JSONDecodeError, IOError):
            return {}
    return {}


def save_cache(cache):
    """Save the cache to disk."""
    with open(CACHE_FILE, "w") as f:
        json.dump(cache, f, indent=2)


def get_cache_key(repo_owner, repo_name, pull_number):
    """Generate a cache key for a PR."""
    return f"{repo_owner}/{repo_name}/{pull_number}"


def parse_iso_timestamp(timestamp_str):
    """Parse an ISO 8601 timestamp string to a datetime object."""
    # GitHub API returns timestamps like "2024-01-15T10:30:00Z"
    # Replace 'Z' with '+00:00' for fromisoformat compatibility
    if timestamp_str.endswith("Z"):
        timestamp_str = timestamp_str[:-1] + "+00:00"
    return datetime.fromisoformat(timestamp_str)


def get_latest_pull_request_number(repo_owner, repo_name, headers):
    """Get the latest pull request number from the repository."""
    response = requests.get(
        f"https://api.github.com/repos/{repo_owner}/{repo_name}/pulls",
        headers=headers,
        params={"state": "all", "sort": "created", "direction": "desc", "per_page": 1},
        timeout=10,
    )
    response.raise_for_status()
    pulls = response.json()
    if not pulls:
        raise ValueError("No pull requests found in the repository")
    return pulls[0]["number"]


def fetch_pull_request_metrics(pull_number, repo_owner, repo_name, headers):
    """Fetch metrics for a single pull request. Returns tuple or None."""
    try:
        # Get the pull request details
        response = requests.get(
            f"https://api.github.com/repos/{repo_owner}/{repo_name}/pulls/{pull_number}",
            headers=headers,
            timeout=10,
        )
        response.raise_for_status()
        pull_request = response.json()

        target_branch = pull_request["base"]["ref"]
        if target_branch != "master":
            return ("skip", pull_number, "does not target master branch")

        # Get the list of events for the pull request
        response = requests.get(pull_request["issue_url"] + "/events", headers=headers, timeout=10)
        response.raise_for_status()
        events = response.json()

        added_to_merge_queue_events = [
            event for event in events if event["event"] == "added_to_merge_queue"
        ]
        merged_events = [event for event in events if event["event"] == "merged"]

        start_event = added_to_merge_queue_events[-1] if added_to_merge_queue_events else None
        end_event = merged_events[-1] if merged_events else None

        if end_event is not None and start_event is not None:
            # Parse the timestamps
            started_at = parse_iso_timestamp(start_event["created_at"])
            merged_at = parse_iso_timestamp(end_event["created_at"])
            # Calculate the time difference
            time_difference = merged_at - started_at
            return ("result", pull_number, started_at, merged_at, time_difference)

        return None
    except Exception as e:
        return ("error", pull_number, str(e))


def main():
    # Parse command line arguments
    parser = argparse.ArgumentParser(description="Get pull request merge time")
    parser.add_argument("--owner", help="Repository owner", default="10gen")
    parser.add_argument("--repo", help="Repository name", default="mongo")
    parser.add_argument(
        "--token",
        default=os.environ.get("MERGE_QUEUE_ANALYTICS_GITHUB_TOKEN"),
        help="GitHub access token (default: MERGE_QUEUE_ANALYTICS_GITHUB_TOKEN env var)",
    )
    parser.add_argument(
        "--max-workers", type=int, default=10, help="Maximum number of parallel requests"
    )
    parser.add_argument(
        "--count", type=int, default=1000, help="Number of pull requests to analyze"
    )
    args = parser.parse_args()

    if not args.token:
        parser.error("--token is required or set MERGE_QUEUE_ANALYTICS_GITHUB_TOKEN env var")

    repo_owner = args.owner
    repo_name = args.repo
    token = args.token
    headers = {"Authorization": f"token {token}"}

    # Get the latest PR number from the repository
    latest_pr = get_latest_pull_request_number(repo_owner, repo_name, headers)
    print(f"Latest PR: {latest_pr}")

    # Load cache
    cache = load_cache()
    cache_hits = 0
    cache_misses = 0

    results = []
    pull_numbers = range(latest_pr - args.count, latest_pr + 1)

    # Check cache first and collect PRs that need fetching
    prs_to_fetch = []
    skipped_from_cache = 0
    for pull_number in pull_numbers:
        cache_key = get_cache_key(repo_owner, repo_name, pull_number)
        if cache_key in cache:
            cached = cache[cache_key]
            # Check if this PR was marked as skipped (e.g., non-master target branch)
            if cached.get("skipped"):
                skipped_from_cache += 1
                continue
            started_at = parse_iso_timestamp(cached["started_at"])
            merged_at = parse_iso_timestamp(cached["merged_at"])
            time_difference = merged_at - started_at
            results.append((pull_number, started_at, merged_at, time_difference))
            cache_hits += 1
        else:
            prs_to_fetch.append(pull_number)

    print(
        f"Cache hits: {cache_hits}, Skipped (cached): {skipped_from_cache}, "
        f"PRs to fetch: {len(prs_to_fetch)}"
    )

    with ThreadPoolExecutor(max_workers=args.max_workers) as executor:
        futures = {
            executor.submit(
                fetch_pull_request_metrics, pull_number, repo_owner, repo_name, headers
            ): pull_number
            for pull_number in prs_to_fetch
        }

        for future in as_completed(futures):
            result = future.result()
            if result is None:
                continue
            if result[0] == "skip":
                print(f"Pull request {result[1]} {result[2]}, skipping")
                # Cache skipped PRs to avoid re-querying
                cache_key = get_cache_key(repo_owner, repo_name, result[1])
                cache[cache_key] = {"skipped": True, "reason": result[2]}
            elif result[0] == "error":
                print(f"Error fetching PR {result[1]}: {result[2]}")
            elif result[0] == "result":
                _, pull_number, started_at, merged_at, time_difference = result
                results.append((pull_number, started_at, merged_at, time_difference))
                print(f"{pull_number}, {started_at}, {time_difference}")
                # Cache the result
                cache_key = get_cache_key(repo_owner, repo_name, pull_number)
                cache[cache_key] = {
                    "started_at": started_at.isoformat(),
                    "merged_at": merged_at.isoformat(),
                }
                cache_misses += 1

    # Save cache
    save_cache(cache)
    print(f"Cache updated: {cache_misses} new entries added")

    # Sort by merge date
    results.sort(key=lambda x: x[2])

    print("\n--- Sorted by merge date ---")
    for pull_number, started_at, merged_at, time_difference in results:
        print(f"{pull_number}, {started_at}, {time_difference}")

    def format_duration(seconds):
        """Format seconds as human-readable duration."""
        hours, remainder = divmod(int(seconds), 3600)
        minutes, secs = divmod(remainder, 60)
        if hours > 0:
            return f"{hours}h {minutes}m {secs}s"
        if minutes > 0:
            return f"{minutes}m {secs}s"
        return f"{secs}s"

    def calculate_percentiles(time_diffs_seconds):
        """Calculate P25, P50, P75, P90, P95, P99 for a list of time differences in seconds."""
        if len(time_diffs_seconds) < 2:
            val = time_diffs_seconds[0] if time_diffs_seconds else 0
            return val, val, val, val, val, val
        time_diffs_seconds = sorted(time_diffs_seconds)
        p25, p50, p75 = quantiles(time_diffs_seconds, n=4)
        p90 = quantiles(time_diffs_seconds, n=10)[8]
        p95 = quantiles(time_diffs_seconds, n=20)[18]
        p99 = quantiles(time_diffs_seconds, n=100)[98]
        return p25, p50, p75, p90, p95, p99

    def print_percentiles(label, time_diffs_seconds):
        """Print percentiles for a given set of time differences."""
        if not time_diffs_seconds:
            print(f"{label}: No data")
            return
        p25, p50, p75, p90, p95, p99 = calculate_percentiles(time_diffs_seconds)
        print(f"{label} (n={len(time_diffs_seconds)}):")
        print(f"  P25: {format_duration(p25)}")
        print(f"  P50 (median): {format_duration(p50)}")
        print(f"  P75: {format_duration(p75)}")
        print(f"  P90: {format_duration(p90)}")
        print(f"  P95: {format_duration(p95)}")
        print(f"  P99: {format_duration(p99)}")

    # Filter out weekends and group by day (in EST)
    if results:
        # Print the evaluation window
        earliest_start = min(r[1] for r in results)
        latest_end = max(r[2] for r in results)
        print("\n--- Evaluation Window (EST) ---")
        print(f"Earliest start time: {earliest_start.astimezone(EST)}")
        print(f"Latest end time: {latest_end.astimezone(EST)}")

        # Group results by day (using merge date in EST), excluding weekends
        results_by_day = defaultdict(list)
        weekday_results = []
        for pull_number, started_at, merged_at, time_difference in results:
            merged_at_est = merged_at.astimezone(EST)
            # Skip weekends (Saturday=5, Sunday=6)
            if merged_at_est.weekday() >= 5:
                continue
            day_key = merged_at_est.strftime("%Y-%m-%d (%A)")
            results_by_day[day_key].append(time_difference.total_seconds())
            weekday_results.append(time_difference.total_seconds())

        # Print overall percentiles (weekdays only)
        print("\n--- Overall Time Difference Percentiles (Weekdays Only) ---")
        print_percentiles("All weekdays", weekday_results)

        # Print percentiles by day
        print("\n--- Time Difference Percentiles by Day (EST, Weekdays Only) ---")
        for day in sorted(results_by_day.keys()):
            print_percentiles(day, results_by_day[day])


if __name__ == "__main__":
    main()
