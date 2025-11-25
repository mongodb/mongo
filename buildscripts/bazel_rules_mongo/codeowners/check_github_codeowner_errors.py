#!/usr/bin/env python3
"""Check if a PR introduces new CODEOWNERS errors using GitHub GraphQL API."""

import argparse
import os
import sys
import time
import requests
import structlog
import yaml

from typing import Dict

LOGGER = structlog.get_logger(__name__)

STATUS_OK = 0
STATUS_ERROR = 1

MAX_RETRIES = 3
RETRY_DELAY_SECONDS = 2


def execute_graphql_query(query: str, github_token: str) -> dict:
    """
    Execute a GraphQL query against GitHub's API with retry logic.

    Args:
        query: The GraphQL query string
        github_token: GitHub authentication token

    Returns:
        The response data from the GraphQL API

    Raises:
        RuntimeError: If the query fails after all retries
    """
    headers = {"Authorization": f"Bearer {github_token}"}
    url = "https://api.github.com/graphql"

    for attempt in range(MAX_RETRIES):
        try:
            LOGGER.debug(f"GraphQL query attempt {attempt + 1}/{MAX_RETRIES}")

            req = requests.post(
                url=url,
                json={"query": query},
                headers=headers,
                timeout=60,
            )
            req.raise_for_status()
            resp = req.json()

            if "errors" in resp:
                LOGGER.error("GraphQL query failed", errors=resp["errors"], attempt=attempt + 1)
                raise RuntimeError(f"GraphQL query failed: {resp['errors']}")

            return resp

        except requests.exceptions.RequestException as e:
            LOGGER.warning(
                "Request failed",
                error=str(e),
                attempt=attempt + 1,
                max_retries=MAX_RETRIES,
            )

            if attempt < MAX_RETRIES - 1:
                delay = RETRY_DELAY_SECONDS * (2**attempt)  # Exponential backoff
                LOGGER.info(f"Retrying in {delay} seconds...")
                time.sleep(delay)
            else:
                LOGGER.error("All retry attempts exhausted", error=str(e))
                raise


def fetch_codeowners_errors(
    github_org: str,
    github_repo: str,
    ref: str,
    github_token: str,
) -> list[dict]:
    """
    Fetch CODEOWNERS errors for a specific ref using GitHub GraphQL API.

    Returns a list of error dictionaries containing information about each error.
    """
    query = f"""{{
        repository(owner: "{github_org}", name: "{github_repo}") {{
            codeowners(refName: "{ref}") {{
                errors {{
                    kind
                    line
                    column
                    message
                    path
                    source
                    suggestion
                }}
            }}
        }}
    }}"""

    LOGGER.info("Fetching CODEOWNERS errors", ref=ref[:7] if len(ref) == 40 else ref)
    resp = execute_graphql_query(query, github_token)

    repo_data = resp.get("data", {}).get("repository")
    if not repo_data:
        LOGGER.error("Repository not found")
        raise RuntimeError("Repository not found")

    codeowners_data = repo_data.get("codeowners", {})
    if not codeowners_data:
        LOGGER.info("No CODEOWNERS data found", ref=ref[:7] if len(ref) == 40 else ref)
        return []

    errors = codeowners_data.get("errors", [])

    LOGGER.info(
        "Found CODEOWNERS errors", count=len(errors), ref=ref[:7] if len(ref) == 40 else ref
    )

    return errors


def get_pr_info(
    github_org: str,
    github_repo: str,
    pr_number: int,
    github_token: str,
) -> dict:
    """
    Fetch PR information including head SHA and base SHA.
    """
    query = f"""{{
        repository(owner: "{github_org}", name: "{github_repo}") {{
            pullRequest(number: {pr_number}) {{
                headRefOid
                baseRefOid
                title
            }}
        }}
    }}"""

    LOGGER.info("Fetching PR information", pr_number=pr_number)

    resp = execute_graphql_query(query, github_token)

    pr_info = resp.get("data", {}).get("repository", {}).get("pullRequest")
    if not pr_info:
        LOGGER.error("PR not found", pr_number=pr_number)
        raise RuntimeError(f"PR {pr_number} not found")

    LOGGER.info(
        "PR information retrieved",
        pr_number=pr_number,
        title=pr_info.get("title"),
        head_sha=pr_info.get("headRefOid"),
        base_sha=pr_info.get("baseRefOid"),
    )

    return pr_info


def error_signature(error: dict) -> tuple:
    """
    Create a unique signature for an error to enable comparison.

    Uses kind and source to identify unique errors.
    """
    return (
        error.get("kind"),
        error.get("source"),
    )


def format_error(error: dict) -> str:
    """Format an error for display."""
    parts = [f"  - Kind: {error.get('kind')}"]

    if error.get("line") is not None:
        location = f"Line {error.get('line')}"
        if error.get("column") is not None:
            location += f", Column {error.get('column')}"
        parts.append(f"    Location: {location}")

    if error.get("source"):
        parts.append(f"    Source: {error.get('source').strip()}")

    if error.get("suggestion"):
        parts.append(f"    Suggestion: {error.get('suggestion').strip()}")

    if error.get("message"):
        parts.append(f"    Message: {error.get('message').strip()}")

    return "\n".join(parts)


def get_expansions(expansions_file: str) -> Dict[str, any]:
    if not expansions_file:
        return None

    if not os.path.exists(expansions_file):
        raise RuntimeError(f"Expansions file not found at {expansions_file}")

    with open(expansions_file, "r", encoding="utf8") as file:
        return yaml.safe_load(file)


def main():
    """
    Check if a PR introduces new CODEOWNERS errors.

    Compares CODEOWNERS errors on the PR head commit against the base commit.
    Fails if new errors are introduced.
    """
    # Change to bazel workspace directory
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY"))

    parser = argparse.ArgumentParser(description="Check if a PR introduces new CODEOWNERS errors")
    parser.add_argument(
        "--expansions-file",
        help="Path of the expansions file to pull values from",
        required=True,
    )

    args = parser.parse_args()

    expansions = get_expansions(args.expansions_file)
    github_org = expansions.get("github_org")
    github_repo = expansions.get("github_repo")
    pr_number = expansions.get("github_pr_number", None)
    github_token = expansions.get("github_token")

    if not pr_number:
        LOGGER.info(
            "No github_pr_number expansion detected, skipping CODEOWNERS github error check."
        )
        return STATUS_OK

    if not github_token:
        LOGGER.error("No GitHub token provided, cannot proceed with CODEOWNERS error check.")
        return STATUS_ERROR

    try:
        # Get PR information
        pr_info = get_pr_info(github_org, github_repo, pr_number, github_token)
        head_sha = pr_info["headRefOid"]
        base_sha = pr_info["baseRefOid"]

        # Fetch errors for both commits
        head_errors = fetch_codeowners_errors(github_org, github_repo, head_sha, github_token)
        base_errors = fetch_codeowners_errors(github_org, github_repo, base_sha, github_token)

        # Create sets of error signatures for comparison
        base_error_sigs = {error_signature(err) for err in base_errors}
        head_error_sigs = {error_signature(err) for err in head_errors}

        # Find new errors
        new_error_sigs = head_error_sigs - base_error_sigs

        if not new_error_sigs:
            LOGGER.info(
                "✓ No new CODEOWNERS errors introduced",
                base_errors=len(base_errors),
                head_errors=len(head_errors),
            )
            return STATUS_OK

        # Find the actual error objects for new errors
        new_errors = [err for err in head_errors if error_signature(err) in new_error_sigs]

        LOGGER.error(
            "✗ New CODEOWNERS errors detected",
            new_error_count=len(new_errors),
            base_error_count=len(base_errors),
            head_error_count=len(head_errors),
        )

        print("\nNew CODEOWNERS errors introduced by this PR:\n")
        for error in new_errors:
            print(format_error(error))
            print()

        print(f"\nSummary:")
        print(f"  Base commit ({base_sha[:7]}): {len(base_errors)} errors")
        print(f"  Head commit ({head_sha[:7]}): {len(head_errors)} errors")
        print(f"  New errors: {len(new_errors)}")
        print(f"\nPlease fix the new CODEOWNERS errors before merging this PR.")

        return STATUS_ERROR

    except Exception as e:
        LOGGER.error("Failed to check CODEOWNERS errors", error=str(e))
        return STATUS_ERROR


if __name__ == "__main__":
    sys.exit(main())
