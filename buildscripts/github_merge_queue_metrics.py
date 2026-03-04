import argparse
import json
import os
import subprocess
from collections import defaultdict
from datetime import datetime
from statistics import quantiles
from zoneinfo import ZoneInfo

import requests

# Optional boto3 import for S3 cache storage
try:
    import boto3
    from botocore.exceptions import ClientError, NoCredentialsError

    BOTO3_AVAILABLE = True
except ImportError:
    BOTO3_AVAILABLE = False

# Optional OTEL imports for Honeycomb integration
try:
    from opentelemetry import trace
    from opentelemetry.exporter.otlp.proto.http.trace_exporter import OTLPSpanExporter
    from opentelemetry.sdk.resources import SERVICE_NAME, Resource
    from opentelemetry.sdk.trace import TracerProvider
    from opentelemetry.sdk.trace.export import BatchSpanProcessor

    OTEL_AVAILABLE = True
except ImportError:
    OTEL_AVAILABLE = False

EST = ZoneInfo("America/New_York")

# Honeycomb OTEL endpoint
HONEYCOMB_OTEL_ENDPOINT = "https://api.honeycomb.io/v1/traces"


def is_sso_profile_configured(profile):
    """Check if the AWS SSO profile is configured.

    Returns True if the profile has SSO configuration, False otherwise.
    """
    try:
        # Check if the profile has sso_start_url configured
        result = subprocess.run(
            ["aws", "configure", "get", "sso_start_url", "--profile", profile],
            capture_output=True,
            text=True,
            timeout=10,
        )
        return result.returncode == 0 and result.stdout.strip() != ""
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def configure_sso_profile(profile):
    """Run aws configure sso to set up the SSO profile interactively.

    Returns True if configuration was successful, False otherwise.
    """
    print(f"AWS SSO profile '{profile}' is not configured.")
    print("Running 'aws configure sso' to set up the profile...")
    print("You will need to provide:")
    print("  - SSO start URL (e.g., https://your-org.awsapps.com/start)")
    print("  - SSO region")
    print("  - Account and role to use")
    print()
    try:
        result = subprocess.run(
            ["aws", "configure", "sso", "--profile", profile],
            timeout=600,  # 10 minutes for interactive configuration
        )
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        print("AWS SSO configuration timed out.")
        return False
    except FileNotFoundError:
        print("AWS CLI not found. Please install the AWS CLI.")
        return False


def are_aws_credentials_valid(profile):
    """Check if AWS credentials are valid for the given profile.

    Returns True if credentials are valid, False otherwise.
    """
    try:
        result = subprocess.run(
            ["aws", "sts", "get-caller-identity", "--profile", profile],
            capture_output=True,
            text=True,
            timeout=30,
        )
        return result.returncode == 0
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def ensure_aws_auth(profile=None):
    """Ensure AWS CLI is authenticated, prompting for interactive login if needed.

    If the SSO profile is not configured, runs 'aws configure sso' first.
    If credentials are already valid, skips the login step.

    Args:
        profile: AWS profile name to use for SSO login.

    Returns True if authentication is successful, False otherwise.
    """
    if not profile:
        print("No AWS profile specified. Use --aws-profile to specify an SSO profile.")
        return False

    try:
        # First check if credentials are already valid
        if are_aws_credentials_valid(profile):
            print(f"AWS credentials for profile '{profile}' are valid.")
            return True

        # Check if the SSO profile is configured
        if not is_sso_profile_configured(profile):
            if not configure_sso_profile(profile):
                print("AWS SSO profile configuration failed.")
                return False

        # Now try to login with the configured profile
        print(f"AWS credentials expired or missing. Logging in with profile '{profile}'...")
        result = subprocess.run(
            ["aws", "sso", "login", "--profile", profile],
            timeout=300,  # 5 minutes for interactive login
        )
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        print("AWS authentication timed out.")
        return False
    except FileNotFoundError:
        print("AWS CLI not found. Please install the AWS CLI.")
        return False


def get_s3_client(region="us-east-1", profile=None, aws_key=None, aws_secret=None):
    """Create an S3 client using either explicit credentials or profile.

    Args:
        region: AWS region.
        profile: AWS profile name to use (ignored if aws_key and aws_secret are provided).
        aws_key: AWS access key ID (optional, overrides profile).
        aws_secret: AWS secret access key (optional, overrides profile).

    Returns:
        boto3 S3 client or None if boto3 is not available.
    """
    if not BOTO3_AVAILABLE:
        return None

    if aws_key and aws_secret:
        # Use explicit credentials
        session = boto3.session.Session(
            aws_access_key_id=aws_key,
            aws_secret_access_key=aws_secret,
        )
    else:
        # Use profile-based authentication
        session = boto3.session.Session(profile_name=profile)

    return session.client("s3", region_name=region)


def download_cache_from_s3(
    s3_bucket, s3_key, region="us-east-1", profile=None, aws_key=None, aws_secret=None
):
    """Download cache file from S3.

    Args:
        s3_bucket: S3 bucket name.
        s3_key: S3 key for the cache file.
        region: AWS region.
        profile: AWS profile name to use (ignored if aws_key and aws_secret are provided).
        aws_key: AWS access key ID (optional, overrides profile).
        aws_secret: AWS secret access key (optional, overrides profile).

    Returns the cache dict.

    Raises:
        RuntimeError: If the cache cannot be downloaded.
    """
    if not BOTO3_AVAILABLE:
        raise RuntimeError("boto3 is not available. Install it to use S3 cache storage.")

    s3_client = get_s3_client(region, profile, aws_key, aws_secret)
    if s3_client is None:
        raise RuntimeError("Failed to create S3 client.")

    try:
        response = s3_client.get_object(Bucket=s3_bucket, Key=s3_key)
        content = response["Body"].read().decode("utf-8")
        cache = json.loads(content)
        print(f"Downloaded cache from s3://{s3_bucket}/{s3_key}")
        return cache
    except NoCredentialsError as e:
        raise RuntimeError("No AWS credentials available for S3 download.") from e
    except ClientError as e:
        error_code = e.response.get("Error", {}).get("Code", "")
        if error_code == "NoSuchKey":
            raise RuntimeError(f"Cache file not found in S3: s3://{s3_bucket}/{s3_key}") from e
        elif error_code == "AccessDenied":
            raise RuntimeError(f"Access denied to S3 cache: s3://{s3_bucket}/{s3_key}") from e
        else:
            raise RuntimeError(f"Error downloading cache from S3: {e}") from e


def upload_cache_to_s3(
    cache, s3_bucket, s3_key, region="us-east-1", profile=None, aws_key=None, aws_secret=None
):
    """Upload cache file to S3.

    Args:
        cache: Cache dict to upload.
        s3_bucket: S3 bucket name.
        s3_key: S3 key for the cache file.
        region: AWS region.
        profile: AWS profile name to use (ignored if aws_key and aws_secret are provided).
        aws_key: AWS access key ID (optional, overrides profile).
        aws_secret: AWS secret access key (optional, overrides profile).

    Returns True if successful, False otherwise.
    """
    if not BOTO3_AVAILABLE:
        print("boto3 is not available. Install it to use S3 cache storage.")
        return False

    try:
        s3_client = get_s3_client(region, profile, aws_key, aws_secret)
        if s3_client is None:
            return False
        cache_json = json.dumps(cache, indent=2)
        s3_client.put_object(
            Bucket=s3_bucket,
            Key=s3_key,
            Body=cache_json.encode("utf-8"),
            ContentType="application/json",
        )
        print(f"Uploaded cache to s3://{s3_bucket}/{s3_key}")
        return True
    except NoCredentialsError:
        print("No AWS credentials available for S3 upload.")
        return False
    except ClientError as e:
        print(f"Error uploading cache to S3: {e}")
        return False


def setup_otel_tracer(honeycomb_api_key, honeycomb_dataset):
    """Set up OpenTelemetry tracer with Honeycomb HTTP exporter."""
    if not OTEL_AVAILABLE:
        print(
            "OpenTelemetry is not available. "
            "Install opentelemetry packages to enable Honeycomb export."
        )
        return None

    resource = Resource(attributes={SERVICE_NAME: "github-merge-queue-metrics"})

    # Configure OTLP HTTP exporter for Honeycomb
    headers = {
        "x-honeycomb-team": honeycomb_api_key,
        "x-honeycomb-dataset": honeycomb_dataset,
    }

    exporter = OTLPSpanExporter(
        endpoint=HONEYCOMB_OTEL_ENDPOINT,
        headers=headers,
    )

    provider = TracerProvider(resource=resource)
    processor = BatchSpanProcessor(exporter)
    provider.add_span_processor(processor)
    trace.set_tracer_provider(provider)

    return trace.get_tracer("github-merge-queue-metrics")


def export_pr_metrics_to_honeycomb(tracer, results, repo_owner, repo_name):
    """Export PR merge queue metrics to Honeycomb as OTEL spans."""
    if tracer is None:
        return

    print(f"\nExporting {len(results)} PR metrics to Honeycomb...")

    for pull_number, started_at, merged_at, time_difference in results:
        # Create a span for each PR merge event
        # Use the actual timestamps from the PR for accurate timing
        span = tracer.start_span(
            "merge_queue_pr",
            start_time=int(started_at.timestamp() * 1e9),  # Convert to nanoseconds
        )
        duration_seconds = time_difference.total_seconds()

        # Set span attributes with PR details
        span.set_attribute("pr.number", pull_number)
        span.set_attribute("pr.repo_owner", repo_owner)
        span.set_attribute("pr.repo_name", repo_name)
        pr_url = f"https://github.com/{repo_owner}/{repo_name}/pull/{pull_number}"
        span.set_attribute("pr.url", pr_url)
        span.set_attribute("pr.merge_queue_started_at", started_at.isoformat())
        span.set_attribute("pr.merged_at", merged_at.isoformat())
        span.set_attribute("pr.merge_queue_duration_seconds", duration_seconds)
        span.set_attribute("pr.merge_queue_duration_minutes", duration_seconds / 60)

        # Add day of week and time of day for analysis
        merged_at_est = merged_at.astimezone(EST)
        span.set_attribute("pr.merged_day_of_week", merged_at_est.strftime("%A"))
        span.set_attribute("pr.merged_hour", merged_at_est.hour)
        span.set_attribute("pr.is_weekend", merged_at_est.weekday() >= 5)
        span.set_attribute("pr.metric_version", 2)

        # End the span at the actual merge time
        span.end(end_time=int(merged_at.timestamp() * 1e9))

    print("Export complete.")


def shutdown_otel():
    """Shutdown the OTEL tracer provider to flush pending spans."""
    if OTEL_AVAILABLE:
        provider = trace.get_tracer_provider()
        if hasattr(provider, "shutdown"):
            provider.shutdown()


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

        pr_title = pull_request.get("title", "")

        # Get the list of events for the pull request
        response = requests.get(pull_request["issue_url"] + "/events", headers=headers, timeout=10)
        response.raise_for_status()
        events = response.json()

        added_to_merge_queue_events = [
            event for event in events if event["event"] == "added_to_merge_queue"
        ]
        merged_events = [event for event in events if event["event"] == "merged"]
        removed_from_merge_queue_events = [
            event for event in events if event["event"] == "removed_from_merge_queue"
        ]

        start_event = added_to_merge_queue_events[-1] if added_to_merge_queue_events else None
        end_event = merged_events[-1] if merged_events else None

        if end_event is not None and start_event is not None:
            # Parse the timestamps
            started_at = parse_iso_timestamp(start_event["created_at"])
            merged_at = parse_iso_timestamp(end_event["created_at"])
            # Calculate the time difference
            time_difference = merged_at - started_at
            return ("result", pull_number, started_at, merged_at, time_difference, pr_title)

        # Check if PR was added to merge queue but removed (not merged)
        if start_event is not None and end_event is None and removed_from_merge_queue_events:
            removed_event = removed_from_merge_queue_events[-1]
            added_at = parse_iso_timestamp(start_event["created_at"])
            removed_at = parse_iso_timestamp(removed_event["created_at"])
            return ("removed", pull_number, added_at, removed_at, pr_title)

        return None
    except Exception as e:
        return ("error", pull_number, str(e))


# GraphQL query template for batch fetching PRs with timeline events
GRAPHQL_BATCH_QUERY = """
query($owner: String!, $repo: String!) {
  repository(owner: $owner, name: $repo) {
    %s
  }
}
"""

GRAPHQL_PR_FRAGMENT = """
    pr%d: pullRequest(number: %d) {
      number
      title
      baseRefName
      mergedAt
      timelineItems(first: 50, itemTypes: [ADDED_TO_MERGE_QUEUE_EVENT, MERGED_EVENT, REMOVED_FROM_MERGE_QUEUE_EVENT]) {
        nodes {
          __typename
          ... on AddedToMergeQueueEvent { createdAt }
          ... on MergedEvent { createdAt }
          ... on RemovedFromMergeQueueEvent { createdAt }
        }
      }
    }
"""

GRAPHQL_ENDPOINT = "https://api.github.com/graphql"


def fetch_prs_batch_graphql(pr_numbers, repo_owner, repo_name, headers, batch_size=50):
    """Fetch multiple PRs in batches using GraphQL.

    Args:
        pr_numbers: List of PR numbers to fetch
        repo_owner: Repository owner
        repo_name: Repository name
        headers: HTTP headers including authorization
        batch_size: Number of PRs per GraphQL request (max ~100 due to query complexity)

    Returns:
        List of result tuples, same format as fetch_pull_request_metrics
    """
    results = []
    total_batches = (len(pr_numbers) + batch_size - 1) // batch_size

    for batch_idx in range(total_batches):
        start_idx = batch_idx * batch_size
        end_idx = min(start_idx + batch_size, len(pr_numbers))
        batch = pr_numbers[start_idx:end_idx]

        # Build the GraphQL query with all PRs in this batch
        pr_fragments = "\n".join(GRAPHQL_PR_FRAGMENT % (pr_num, pr_num) for pr_num in batch)
        query = GRAPHQL_BATCH_QUERY % pr_fragments

        try:
            response = requests.post(
                GRAPHQL_ENDPOINT,
                headers=headers,
                json={"query": query, "variables": {"owner": repo_owner, "repo": repo_name}},
                timeout=60,
            )
            response.raise_for_status()
            data = response.json()

            if "errors" in data:
                print(f"GraphQL errors in batch {batch_idx + 1}: {data['errors']}")
                # Fall back to individual fetches for this batch
                for pr_num in batch:
                    results.append(("error", pr_num, f"GraphQL error: {data['errors']}"))
                continue

            repo_data = data.get("data", {}).get("repository", {})
            batch_results = _parse_graphql_batch_response(repo_data, batch)
            results.extend(batch_results)

            print(
                f"Fetched batch {batch_idx + 1}/{total_batches} " f"({len(batch)} PRs in 1 request)"
            )

        except Exception as e:
            print(f"Error fetching batch {batch_idx + 1}: {e}")
            for pr_num in batch:
                results.append(("error", pr_num, str(e)))

    return results


def _parse_graphql_batch_response(repo_data, pr_numbers):
    """Parse GraphQL response for a batch of PRs.

    Args:
        repo_data: The 'repository' object from the GraphQL response
        pr_numbers: List of PR numbers in this batch

    Returns:
        List of result tuples
    """
    results = []

    for pr_num in pr_numbers:
        pr_key = f"pr{pr_num}"
        pr_data = repo_data.get(pr_key)

        if pr_data is None:
            # PR doesn't exist or was deleted
            results.append(None)
            continue

        try:
            result = _parse_single_pr_graphql(pr_data)
            results.append(result)
        except Exception as e:
            results.append(("error", pr_num, str(e)))

    return results


def _parse_single_pr_graphql(pr_data):
    """Parse a single PR from GraphQL response.

    Args:
        pr_data: The PR object from GraphQL response

    Returns:
        Result tuple matching fetch_pull_request_metrics format
    """
    pr_number = pr_data["number"]
    pr_title = pr_data.get("title", "")
    base_ref = pr_data.get("baseRefName", "")

    # Check if PR targets master
    if base_ref != "master":
        return ("skip", pr_number, "does not target master branch")

    timeline_items = pr_data.get("timelineItems", {}).get("nodes", [])

    # Extract events by type
    added_events = [
        item for item in timeline_items if item.get("__typename") == "AddedToMergeQueueEvent"
    ]
    merged_events = [item for item in timeline_items if item.get("__typename") == "MergedEvent"]
    removed_events = [
        item for item in timeline_items if item.get("__typename") == "RemovedFromMergeQueueEvent"
    ]

    start_event = added_events[-1] if added_events else None
    end_event = merged_events[-1] if merged_events else None

    if end_event is not None and start_event is not None:
        started_at = parse_iso_timestamp(start_event["createdAt"])
        merged_at = parse_iso_timestamp(end_event["createdAt"])
        time_difference = merged_at - started_at
        return ("result", pr_number, started_at, merged_at, time_difference, pr_title)

    # Check if PR was added to merge queue but removed (not merged)
    if start_event is not None and end_event is None and removed_events:
        removed_event = removed_events[-1]
        added_at = parse_iso_timestamp(start_event["createdAt"])
        removed_at = parse_iso_timestamp(removed_event["createdAt"])
        return ("removed", pr_number, added_at, removed_at, pr_title)

    return None


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
    parser.add_argument(
        "--show-removed",
        action="store_true",
        help="Print a list of PRs that were removed from the merge queue (not merged)",
    )
    parser.add_argument(
        "--honeycomb-api-key",
        default=os.environ.get("HONEYCOMB_API_KEY"),
        help="Honeycomb API key for exporting metrics (default: HONEYCOMB_API_KEY env var)",
    )
    parser.add_argument(
        "--honeycomb-dataset",
        default=os.environ.get("HONEYCOMB_DATASET", "merge-queue-metrics"),
        help="Honeycomb dataset name (default: HONEYCOMB_DATASET env var or 'merge-queue-metrics')",
    )
    parser.add_argument(
        "--s3-cache-bucket",
        default="mdb-build-private",
        help="S3 bucket for shared cache storage (default: S3_CACHE_BUCKET env var)",
    )
    parser.add_argument(
        "--s3-cache-key",
        default=os.environ.get("S3_CACHE_KEY", "data_store/merge_queue/cache.json"),
        help="S3 key for cache file (default: data_store/merge_queue/cache.json)",
    )
    parser.add_argument(
        "--aws-region",
        default=os.environ.get("AWS_REGION", "us-east-1"),
        help="AWS region for S3 (default: us-east-1)",
    )
    parser.add_argument(
        "--aws-profile",
        default=os.environ.get("AWS_PROFILE", "mongodb-dev"),
        help="AWS profile name for SSO authentication (default: mongodb-dev)",
    )
    parser.add_argument(
        "--graphql-batch-size",
        type=int,
        default=50,
        help="Number of PRs to fetch per GraphQL request (default: 50, max ~100)",
    )
    args = parser.parse_args()

    if not args.token:
        parser.error("--token is required or set MERGE_QUEUE_ANALYTICS_GITHUB_TOKEN env var")

    # Set up OTEL tracer for Honeycomb export if API key is provided
    tracer = None
    if args.honeycomb_api_key:
        tracer = setup_otel_tracer(args.honeycomb_api_key, args.honeycomb_dataset)
        if tracer:
            print(f"Honeycomb export enabled (dataset: {args.honeycomb_dataset})")
    else:
        print("Honeycomb export disabled (no API key provided)")

    repo_owner = args.owner
    repo_name = args.repo
    token = args.token
    headers = {"Authorization": f"token {token}"}

    # Get the latest PR number from the repository
    latest_pr = get_latest_pull_request_number(repo_owner, repo_name, headers)
    print(f"Latest PR: {latest_pr}")

    # Load cache from S3
    # Check for explicit AWS credentials from environment variables
    aws_key_build = os.environ.get("aws_key_build")
    aws_secret_build = os.environ.get("aws_secret_build")
    use_explicit_aws_creds = bool(aws_key_build and aws_secret_build)

    if use_explicit_aws_creds:
        # Use explicit AWS credentials from environment variables
        print("Using AWS credentials from aws_key_build/aws_secret_build environment variables")
        cache = download_cache_from_s3(
            args.s3_cache_bucket,
            args.s3_cache_key,
            args.aws_region,
            aws_key=aws_key_build,
            aws_secret=aws_secret_build,
        )
    elif ensure_aws_auth(profile=args.aws_profile):
        # Fall back to profile-based authentication
        cache = download_cache_from_s3(
            args.s3_cache_bucket, args.s3_cache_key, args.aws_region, profile=args.aws_profile
        )
    else:
        raise RuntimeError(
            "AWS authentication failed. Either set aws_key_build and aws_secret_build "
            "environment variables, or ensure AWS CLI is configured with --aws-profile."
        )
    cache_hits = 0
    cache_misses = 0

    results = []  # All results (cached + new) for statistics
    new_results = []  # Only newly fetched results for Honeycomb export
    removed_results = []
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

    # Use GraphQL batch fetching for efficiency (~99% fewer API requests)
    if prs_to_fetch:
        print(f"Using GraphQL batch fetching (batch size: {args.graphql_batch_size})")
        batch_results = fetch_prs_batch_graphql(
            prs_to_fetch, repo_owner, repo_name, headers, batch_size=args.graphql_batch_size
        )

        for result in batch_results:
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
                _, pull_number, started_at, merged_at, time_difference, pr_title = result
                pr_result = (pull_number, started_at, merged_at, time_difference)
                results.append(pr_result)
                new_results.append(pr_result)  # Track for Honeycomb export (dedup)
                print(f"{pull_number}, {started_at}, {time_difference}")
                # Cache the result
                cache_key = get_cache_key(repo_owner, repo_name, pull_number)
                cache[cache_key] = {
                    "started_at": started_at.isoformat(),
                    "merged_at": merged_at.isoformat(),
                }
                cache_misses += 1
            elif result[0] == "removed":
                _, pull_number, added_at, removed_at, pr_title = result
                removed_results.append((pull_number, added_at, removed_at, pr_title))

    # Save cache to S3
    if use_explicit_aws_creds:
        upload_cache_to_s3(
            cache,
            args.s3_cache_bucket,
            args.s3_cache_key,
            args.aws_region,
            aws_key=aws_key_build,
            aws_secret=aws_secret_build,
        )
    else:
        upload_cache_to_s3(
            cache,
            args.s3_cache_bucket,
            args.s3_cache_key,
            args.aws_region,
            profile=args.aws_profile,
        )
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

    # Print removed PRs if requested
    if args.show_removed:
        # Sort removed results by removed_at date
        removed_results.sort(key=lambda x: x[2])
        print(f"\n--- PRs Removed from Merge Queue (n={len(removed_results)}) ---")
        if removed_results:
            for pull_number, added_at, removed_at, pr_title in removed_results:
                removed_at_est = removed_at.astimezone(EST)
                pr_url = f"https://github.com/{repo_owner}/{repo_name}/pull/{pull_number}"
                print(f"#{pull_number} | {removed_at_est.strftime('%Y-%m-%d %H:%M')} | {pr_url}")
                print(f"  Title: {pr_title}")
        else:
            print("No PRs were removed from the merge queue in this range.")

    # Export only NEW results to Honeycomb (skip cached PRs to avoid duplicates)
    if tracer and new_results:
        export_pr_metrics_to_honeycomb(tracer, new_results, repo_owner, repo_name)
        shutdown_otel()
    elif tracer and not new_results:
        print("\nNo new PRs to export to Honeycomb (all were cached).")


if __name__ == "__main__":
    main()
