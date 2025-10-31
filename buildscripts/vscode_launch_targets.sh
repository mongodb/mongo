#!/usr/bin/env bash
set -euo pipefail

# 1) what we want to query
QUERY=${1:-"attr(tags, \"mongo_binary\", //...) union attr(tags, \"mongo_unittest\", //...)"}

# 2) assume VSCode runs this from the repo root
WORKSPACE=$(pwd)
BAZELOUT="${WORKSPACE}/bazel-out/"

# 3) run bazel query in background and give it, say, 2 seconds to finish
OUT_FILE=$(mktemp)
ERR_FILE=$(mktemp)

(bazel query "$QUERY" >"$OUT_FILE" 2>"$ERR_FILE") &
BAZEL_PID=$!

# how long we let it run (in seconds)
TIME_BUDGET=5
SLEEP_SLICE=0.1

elapsed=0
busy=0
while kill -0 "$BAZEL_PID" 2>/dev/null; do
    # still running
    if (($(echo "$elapsed >= $TIME_BUDGET" | bc))); then
        busy=1
        break
    fi
    sleep "$SLEEP_SLICE"
    elapsed=$(echo "$elapsed + $SLEEP_SLICE" | bc)
done

if ((busy)); then
    # kill the stuck query
    kill "$BAZEL_PID" >/dev/null 2>&1 || true
    echo "# bazel query did not finish in $TIME_BUDGET seconds (maybe a build is running?)."
    rm -f "$OUT_FILE" "$ERR_FILE"
    exit 0
fi

# wait for query to exit and get status
wait "$BAZEL_PID"
Q_STATUS=$?

if ((Q_STATUS != 0)); then
    echo "# bazel query failed: $(cat "$ERR_FILE")"
    rm -f "$OUT_FILE" "$ERR_FILE"
    exit 0
fi

# 4) now we have labels in $OUT_FILE
mapfile -t RAW_LABELS <"$OUT_FILE"
rm -f "$OUT_FILE" "$ERR_FILE"

FOUND=()

for label in "${RAW_LABELS[@]}"; do
    [[ -z "$label" ]] && continue
    [[ "$label" == *"_with_debug"* ]] && continue
    [[ "$label" == *"_ci_wrapper"* ]] && continue
    [[ "$label" == *"third_party"* ]] && continue
    [[ "$label" != //* ]] && continue

    rest=${label#//}
    if [[ "$rest" == *:* ]]; then
        pkg=${rest%%:*}
        name=${rest##*:}
    else
        pkg=$rest
        name=${rest##*/}
    fi

    # ONLY look in real bazel-out
    if [[ -d "$BAZELOUT" ]]; then
        while IFS= read -r -d '' cfg; do
            [[ -d "$cfg/bin" ]] || continue

            cand="$cfg/bin/$pkg/$name"
            cand_exe="$cand.exe"

            if [[ -f "$cand" ]]; then
                rel=${cand#"$WORKSPACE/"}
                FOUND+=("$rel")
            elif [[ -f "$cand_exe" ]]; then
                rel=${cand_exe#"$WORKSPACE/"}
                FOUND+=("$rel")
            fi
        done < <(find "$BAZELOUT" -mindepth 1 -maxdepth 1 -type d -print0)
    fi
done

if ((${#FOUND[@]} == 0)); then
    echo "# no bazel-out artifacts found for query: $QUERY"
    exit 0
fi

printf '%s\n' "${FOUND[@]}" | sort -u
