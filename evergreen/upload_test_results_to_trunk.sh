#!/usr/bin/env bash
# Uploads test results to Trunk Flaky Tests. Never fails the calling task.
#
# Input (at least one; both may be given):
#   TRUNK_REPORT_FILE    Evergreen report.json, converted to JUnit XML with
#                        buildscripts/evergreen_report_to_junit.py first
#   TRUNK_JUNIT_PATHS    Comma-separated glob patterns of JUnit XML files
#
# Environment:
#   DISABLE_TRUNK_TEST_ANALYSIS  Set to "true" to skip the upload (opt-out)
#   TRUNK_API_TOKEN      Trunk organization API token (skips if empty)
#   TRUNK_ORG_URL_SLUG   Trunk organization URL slug
#   TRUNK_TEST_COLLECTION_ID  Trunk test collection id
#   TRUNK_DISPLAY_TASK_NAME, TRUNK_TASK_NAME
#                        Suite name for TRUNK_REPORT_FILE (part of the Trunk test id): the display
#                        task name if set, else the task name
#   TRUNK_CONVERTER      Path to evergreen_report_to_junit.py
#   JOB_NAME, JOB_URL    Evergreen job name and Spruce task URL shown in Trunk
#   TRUNK_REPO_URL, TRUNK_REPO_HEAD_AUTHOR_NAME, TRUNK_PR_NUMBER
#                        Git metadata for tasks without a git checkout (the CLI reads these itself;
#                        see `trunk-analytics-cli upload --help`). An empty PR number is ignored.

set -u

# Resolved before main changes directory.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR

readonly TRUNK_CLI_VERSION="0.16.0"
readonly TRUNK_RELEASES_URL="https://trunk.io/releases/analytics-cli/prod"
readonly TRUNK_VARIANT_MAX_LENGTH=64

cli_sha256_for_platform() {
    case "$1" in
    x86_64-unknown-linux) echo "2a578d6441ee56c70f7d3319af43c7475724f163e27f8c5057b7e6357a879027" ;;
    aarch64-unknown-linux) echo "d919ad8edaef995976fbf08e2054f6b7c433e51630fda0be7892024499e22adb" ;;
    x86_64-apple-darwin) echo "f96043d4715e72a7edee9bf0b36765c4d3d804b92c6b9d9e1fb2f9e254eb1bae" ;;
    aarch64-apple-darwin) echo "694f78dc75c6f64cd1e69d3b9ed7de369f2f52ec58bd3360b718248be2571cb7" ;;
    *) return 1 ;;
    esac
}

detect_platform() {
    local -r arch="$(uname -m)"
    case "$(uname -s)" in
    Linux) echo "${arch}-unknown-linux" ;;
    Darwin) echo "${arch}-apple-darwin" ;;
    *) return 1 ;;
    esac
}

download_cli() {
    local -r platform="$1"
    local -r destination="$2"
    local expected_sha256 actual_sha256
    expected_sha256="$(cli_sha256_for_platform "$platform")" || return 1
    local -r tarball="${destination}/trunk-analytics-cli.tar.gz"

    curl --silent --show-error --fail --location --retry 3 --connect-timeout 10 --max-time 60 \
        --output "$tarball" \
        "${TRUNK_RELEASES_URL}/${TRUNK_CLI_VERSION}/trunk-analytics-cli-${platform}.tar.gz" || return 1

    actual_sha256="$(openssl dgst -sha256 "$tarball" | awk '{print $NF}')"
    if [ "$actual_sha256" != "$expected_sha256" ]; then
        echo "Trunk CLI checksum mismatch for ${platform}: expected ${expected_sha256}, got ${actual_sha256}" >&2
        return 1
    fi
    tar -xz -C "$destination" -f "$tarball"
}

convert_report() {
    local -r junit_dir="$1"
    if [ ! -s "$TRUNK_REPORT_FILE" ]; then
        echo "No report at '$TRUNK_REPORT_FILE'; nothing to convert"
        return 0
    fi
    if grep -q '"no_tests_ran_placeholder"' "$TRUNK_REPORT_FILE"; then
        echo "Report only holds the placeholder result; nothing to convert"
        return 0
    fi
    # The suite is part of the Trunk test id: the display task name if the task is part of a
    # display task, else the task name.
    local -r suite="${TRUNK_DISPLAY_TASK_NAME:-${TRUNK_TASK_NAME:-}}"
    if [ -z "$suite" ]; then
        echo "No task name known for '$TRUNK_REPORT_FILE'; skipping" >&2
        return 1
    fi
    python3 "${TRUNK_CONVERTER:-${SCRIPT_DIR}/../buildscripts/evergreen_report_to_junit.py}" "$TRUNK_REPORT_FILE" \
        --suite "$suite" \
        >"${junit_dir}/report.xml" || return 1
    if [ -n "${TRUNK_JUNIT_PATHS:-}" ]; then
        TRUNK_JUNIT_PATHS="${TRUNK_JUNIT_PATHS},${junit_dir}/report.xml"
    else
        TRUNK_JUNIT_PATHS="${junit_dir}/report.xml"
    fi
}

upload() {
    local -r cli_dir="$1"
    local platform variant
    local -a variant_args=()
    [ -n "${TRUNK_JUNIT_PATHS:-}" ] || return 0
    platform="$(detect_platform)" || return 1
    if ! download_cli "$platform" "$cli_dir"; then
        echo "Could not download or verify the Trunk CLI; skipping upload" >&2
        return 0
    fi
    variant="$(echo "${build_variant:-}" | cut -c "1-${TRUNK_VARIANT_MAX_LENGTH}")"
    [ -n "$variant" ] && variant_args=(--variant "$variant")
    local status=0
    "$cli_dir/trunk-analytics-cli" upload \
        --junit-paths "$TRUNK_JUNIT_PATHS" \
        --org-url-slug "$TRUNK_ORG_URL_SLUG" \
        --test-collection-id "$TRUNK_TEST_COLLECTION_ID" \
        --allow-empty-test-results \
        ${variant_args[@]+"${variant_args[@]}"} || status=$?
    if [ "$status" -ne 0 ]; then
        # The CLI also exits non-zero when a test failed that is not quarantined in Trunk. That
        # must not change the task result, so the CLI's own output above is the only detail.
        echo "Trunk CLI exited with status ${status} (expected when a test failed that is not quarantined in Trunk, otherwise see the output above); continuing" >&2
    fi
    return 0
}

export_repo_info() {
    if [ "${is_patch:-}" = "true" ]; then
        # Evergreen does not expose an accurate commit SHA for patches: they may have uncommitted
        # changes, and for merge queue patches both revision and the SHA in github_head_branch are
        # the merge group's base, not its head. Instead, report a fake SHA based on the Evergreen
        # version ID, which is unique per patch.
        TRUNK_REPO_HEAD_SHA="$(printf '%s' "$version_id" | sha1sum | awk '{print $1}')"
        export TRUNK_REPO_HEAD_SHA
        if [ "${is_commit_queue:-}" = "true" ]; then
            # Evergreen always sets github_head_branch for merge queue patches, e.g.
            # gh-readonly-queue/master/pr-123-<base sha>.
            export TRUNK_REPO_HEAD_BRANCH="$github_head_branch"
        else
            export TRUNK_REPO_HEAD_BRANCH="evergreen-patch/${version_id}"
        fi
    else
        export TRUNK_REPO_HEAD_SHA="${revision:-}"
        export TRUNK_REPO_HEAD_BRANCH="${branch_name:-}"
    fi
    if [ -n "${TRUNK_REPO_URL:-}" ] && [ -n "${TRUNK_REPO_HEAD_SHA:-}" ] &&
        [ -n "${TRUNK_REPO_HEAD_BRANCH:-}" ] && [ -n "${TRUNK_REPO_HEAD_AUTHOR_NAME:-}" ]; then
        export TRUNK_USE_UNCLONED_REPO=true
    fi
}

main() {
    if [ "${DISABLE_TRUNK_TEST_ANALYSIS:-}" = "true" ]; then
        echo "DISABLE_TRUNK_TEST_ANALYSIS is set; skipping Trunk upload"
        return 0
    fi
    if [ -n "${workdir:-}" ]; then
        cd "$workdir" || {
            echo "Cannot cd to '$workdir'; skipping Trunk upload" >&2
            return 0
        }
    fi
    local platform
    platform="$(detect_platform || echo "$(uname -s)-$(uname -m)")"
    if ! cli_sha256_for_platform "$platform" >/dev/null; then
        echo "Trunk CLI is not available for ${platform}; skipping"
        return 0
    fi
    export_repo_info
    if [ -z "${TRUNK_API_TOKEN:-}" ] || [ -z "${TRUNK_ORG_URL_SLUG:-}" ] ||
        [ -z "${TRUNK_TEST_COLLECTION_ID:-}" ]; then
        echo "Trunk token, org slug or collection id are not configured; skipping"
        return 0
    fi
    if [ -z "${TRUNK_REPORT_FILE:-}" ] && [ -z "${TRUNK_JUNIT_PATHS:-}" ]; then
        echo "No report or JUnit paths given; skipping"
        return 0
    fi
    local work_dir
    work_dir="$(mktemp -d)"
    if [ -n "${TRUNK_REPORT_FILE:-}" ]; then
        convert_report "$work_dir" || echo "Converting the report for Trunk failed; continuing" >&2
    fi
    upload "$work_dir" || echo "Trunk upload could not run on this host; continuing" >&2
    rm -rf "$work_dir"
    return 0
}

main "$@"
