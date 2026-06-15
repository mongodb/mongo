#!/usr/bin/env bash
# Extract (and optionally suppress) PW.*/RW.* findings from Coverity Connect.
#
# These parse-phase checkers are valuable for identifying Coverity analysis
# black holes — functions and identifiers where taint paths from network input
# silently terminate — but clutter the default Connect view alongside real bugs.
#
# Workflow:
#   Step 1 — Extract to CSV for review (run without --suppress):
#     ./extract_pw_rw_findings.sh --output pw_rw_findings.csv
#
#   Step 2 — Review the CSV. Pay particular attention to:
#     - taintRelevant=true entries in src/mongo/transport/ or src/mongo/rpc/
#       (these are taint-path black holes worth modeling)
#     - PW.PARAMETER_HIDDEN entries (64 confirmed bugs historically — consider
#       keeping these unresolved in Connect rather than suppressing them)
#
#   Step 3 — Suppress reviewed findings in Connect (run with --suppress):
#     ./extract_pw_rw_findings.sh --suppress
#
#   After suppression:
#     - Current 919 findings are marked Intentional in Connect and leave the
#       default "outstanding" view.
#     - NEW code introducing PW.*/RW.* at new locations gets fresh CIDs
#       and surfaces immediately — regressions are still caught.
#     - Suppressed findings remain in Connect and can be re-queried at any
#       time (e.g., for taint analysis) using this script with --include-suppressed.
#     - The suppression is reversible and carries an audit trail (who/when/why).
#
# Environment variables:
#   COVERITY_URL       Coverity Connect URL (default: https://coverity.prod.corp.mongodb.com)
#   COVERITY_AUTH_KEY  Path to auth-key-file (default: ~/.coverity-auth-key)
#   STREAM             Stream name (default: mongo.master)
#
# Usage:
#   extract_pw_rw_findings.sh [--output <csv>] [--suppress] [--include-suppressed] [--confirm]
#
# SAFETY GUARD: suppression is a dry-run by default. Pass --confirm to execute.

set -euo pipefail

DRY_RUN=true

COVERITY_URL="${COVERITY_URL:-https://coverity.prod.corp.mongodb.com}"
COVERITY_AUTH_KEY="${COVERITY_AUTH_KEY:-${HOME}/.coverity-auth-key}"
COVERITY_USER="${COVERITY_USER:-}"
COVERITY_PASSWORD="${COVERITY_PASSWORD:-}"
STREAM="${STREAM:-mongo.master}"
OUTPUT="coverity_pw_rw_findings.csv"
SUPPRESS=false
INCLUDE_SUPPRESSED=false

while [[ $# -gt 0 ]]; do
    case "$1" in
    --output)
        OUTPUT="$2"
        shift 2
        ;;
    --suppress)
        SUPPRESS=true
        shift
        ;;
    --confirm)
        DRY_RUN=false
        shift
        ;;
    --include-suppressed)
        INCLUDE_SUPPRESSED=true
        shift
        ;;
    --user)
        COVERITY_USER="$2"
        shift 2
        ;;
    --password)
        COVERITY_PASSWORD="$2"
        shift 2
        ;;
    --auth-key-file)
        COVERITY_AUTH_KEY="$2"
        shift 2
        ;;
    --stream)
        STREAM="$2"
        shift 2
        ;;
    --url)
        COVERITY_URL="$2"
        shift 2
        ;;
    *)
        echo "Unknown option: $1"
        exit 1
        ;;
    esac
done

COV_BIN="${COVERITY_BIN:-/home/jason/cov-analysis-linux64-2025.6.0/bin}"

# Build auth args — prefer auth-key-file if it exists, fall back to user/password
if [ -f "$COVERITY_AUTH_KEY" ]; then
    AUTH_ARGS=(--auth-key-file "$COVERITY_AUTH_KEY")
elif [ -n "$COVERITY_USER" ] && [ -n "$COVERITY_PASSWORD" ]; then
    AUTH_ARGS=(--user "$COVERITY_USER" --password "$COVERITY_PASSWORD")
else
    echo "ERROR: no credentials found. Provide one of:"
    echo "  1. Auth key file at $COVERITY_AUTH_KEY (JSON with username+key):"
    echo "     Format: {\"type\":\"Coverity authentication key\",\"version\":2,\"id\":1,"
    echo "              \"domain\":\"\",\"username\":\"you@mongodb.com\",\"key\":\"TOKEN\"}"
    echo "     Create: umask 0177 && echo '{...}' > $COVERITY_AUTH_KEY"
    echo "  2. --user <email> --password <api-token> on the command line"
    echo "  3. COVERITY_USER and COVERITY_PASSWORD environment variables"
    exit 1
fi

COMMON_ARGS=(
    --url "$COVERITY_URL"
    "${AUTH_ARGS[@]}"
    --on-new-cert distrust
)

# All PW.*/RW.* checkers seen in MongoDB nightly scans, grouped by category
TAINT_BLACK_HOLE_CHECKERS=(
    # Functions/identifiers Coverity cannot trace — taint paths terminate here
    "RW.UNDEFINED_IDENTIFIER"
    "RW.NO_MATCHING_FUNCTION"
    "RW.NO_MATCHING_OPERATOR_FUNCTION"
    "RW.NO_MATCHING_CONSTRUCTOR"
    "RW.ENTITY_NOT_EMITTED"
    "RW.ROUTINE_NOT_EMITTED"
)
PARSE_QUALITY_CHECKERS=(
    # Parse-phase quality signals (64 confirmed PW.PARAMETER_HIDDEN bugs historically)
    "PW.PARAMETER_HIDDEN"
    "RW.NARROWING_CONVERSION"
    "RW.BAD_CAST"
    "RW.BAD_INITIALIZER_TYPE"
    "RW.ASSIGNED_GOTO_REQUIRES_VOID_PTR"
    "RW.CONSTINIT_VARIABLE_HAS_DYNAMIC_INIT"
    "RW.AMBIGUOUS_OPERATOR_FUNCTION"
    "RW.NO_MATCH_FOR_TYPE_OF_OVERLOADED_FUNCTION"
    "RW.EXPR_NOT_CONSTANT"
    "RW.NOT_A_MEMBER"
    "RW.DELETED_FUNCTION"
    "RW.INCOMPLETE_TYPE_NOT_ALLOWED"
    "RW.INCOMPLETE_VAR_TYPE"
    "RW.EXPANSION_CONTAINS_NO_PACKS"
    "RW.CONSTEVAL_CALL_NONCONSTANT"
    "RW.NAME_NOT_FOUND_IN_FILE_SCOPE"
    "RW.NOT_A_FIELD_OR_BASE_CLASS"
    "RW.STATIC_ASSERT"
    "RW.TOO_FEW_TEMPLATE_ARGS"
    "RW.TYPE_IDENTIFIER_NOT_ALLOWED"
    "RW.ID_MUST_BE_CLASS_OR_NAMESPACE_NAME"
    "RW.CANNOT_DEDUCE_TYPE_IN_RANGE_BASED_FOR"
    "RW.DESTRUCTOR_QUALIFIER_TYPE_MISMATCH"
    "RW.BAD_CALL_OF_CLASS_OBJECT"
    "RW.NOT_A_TEMPLATE"
    "PW.SWITCH_SELECTOR_EXPR_IS_CONSTANT"
    "PW.CONVERSION_FUNCTION_NOT_USABLE"
    "PW.NEVER_DEFINED"
    "PW.DIVIDE_BY_ZERO"
    "PW.NORETURN_FUNCTION_DOES_RETURN"
    "PW.SIGNED_ONE_BIT_FIELD"
    "PW.SHIFT_COUNT_TOO_LARGE"
)

ALL_CHECKERS=("${TAINT_BLACK_HOLE_CHECKERS[@]}" "${PARSE_QUALITY_CHECKERS[@]}")
TAINT_SET=" ${TAINT_BLACK_HOLE_CHECKERS[*]} "

SUPPRESS_COMMENT="Parse-phase noise (PW.*/RW.*): bulk-suppressed after review. \
These checkers indicate Coverity analysis black holes but do not represent \
actionable defects in this instance. New occurrences in future code will \
surface as new findings. See etc/coverity_models/extract_pw_rw_findings.sh \
for taint analysis workflow."

TMPDIR_LOCAL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_LOCAL"' EXIT

echo "Coverity Connect: ${COVERITY_URL}"
echo "Stream          : ${STREAM}"
if [ "$SUPPRESS" = true ] && [ "$DRY_RUN" = true ]; then
    MODE="extract + suppress (DRY RUN — pass --confirm to execute)"
elif [ "$SUPPRESS" = true ]; then
    MODE="extract + suppress"
else
    MODE="extract only"
fi
echo "Mode            : ${MODE}"
echo "Output CSV      : ${OUTPUT}"
echo

# Write CSV header
echo "cid,checker,file,function,occurrenceCount,firstDetected,classification,taintRelevant" >"$OUTPUT"

total_found=0
total_suppressed=0

for checker in "${ALL_CHECKERS[@]}"; do
    tmpfile="$TMPDIR_LOCAL/${checker}.csv"
    is_taint=false
    [[ "$TAINT_SET" == *" ${checker} "* ]] && is_taint=true

    printf "  %-45s " "$checker"

    # Build show args
    show_args=(
        "${COMMON_ARGS[@]}"
        --mode defects --show
        --stream "$STREAM"
        --checker "$checker"
        --fields cid,checker,file,function,occurrenceCount,firstDetected,classification
    )
    # Include dismissed/intentional findings if requested
    if [ "$INCLUDE_SUPPRESSED" = true ]; then
        show_args+=(--status any)
    fi

    if ! "$COV_BIN/cov-manage-im" "${show_args[@]}" >"$tmpfile" 2>/dev/null; then
        echo "WARN (query failed)"
        continue
    fi

    count=$(tail -n +2 "$tmpfile" | wc -l | tr -d ' ')
    echo -n "${count} findings"
    total_found=$((total_found + count))

    # Append to CSV with taintRelevant column
    tail -n +2 "$tmpfile" | while IFS= read -r line; do
        echo "${line},${is_taint}"
    done >>"$OUTPUT"

    # Suppress if requested (and not dry-run)
    if [ "$SUPPRESS" = true ] && [ "$count" -gt 0 ]; then
        if [ "$DRY_RUN" = true ]; then
            echo -n "  → would suppress (dry run — pass --confirm to execute)"
        elif "$COV_BIN/cov-manage-im" \
            "${COMMON_ARGS[@]}" \
            --mode defects --update \
            --stream "$STREAM" \
            --checker "$checker" \
            --classification "Intentional" \
            --action Ignore \
            --comment "$SUPPRESS_COMMENT" \
            >/dev/null 2>&1; then
            echo -n "  → suppressed"
            total_suppressed=$((total_suppressed + count))
        else
            echo -n "  → suppress FAILED"
        fi
    fi

    echo
done

echo
echo "────────────────────────────────────────────────"
echo "Total PW.*/RW.* findings extracted : ${total_found}"
[ "$SUPPRESS" = true ] && echo "Total suppressed in Connect        : ${total_suppressed}"
echo "CSV written to                     : ${OUTPUT}"
echo
echo "Taint-path black holes (${#TAINT_BLACK_HOLE_CHECKERS[@]} checkers):"
grep ",true$" "$OUTPUT" | wc -l | xargs echo "  Total findings:"
echo "  Transport/RPC subset:"
grep -c "src/mongo/transport\|src/mongo/rpc" "$OUTPUT" 2>/dev/null &&
    grep "src/mongo/transport\|src/mongo/rpc" "$OUTPUT" | grep ",true$" | wc -l | xargs echo "    Taint-relevant in transport+rpc:" ||
    echo "    (none in transport/rpc)"
echo
if [ "$SUPPRESS" = false ]; then
    echo "Next step: review ${OUTPUT}, then run with --suppress to bulk-triage in Connect."
    echo "  Note: PW.PARAMETER_HIDDEN has 64 confirmed historical bugs — consider"
    echo "  keeping those unresolved rather than suppressing them."
fi
