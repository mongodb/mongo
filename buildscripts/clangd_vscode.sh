#!/usr/bin/env bash
set -euo pipefail

# Collect all passed arguments
ARGS=("$@")

# Ordered list of possible clangd locations (each candidate must be a single path)
CANDIDATES=(
    "$(command -v custom-clangd || true)"
    "$(find .compiledb -path '*/compiledb-*/external/mongo_toolchain_v5/v5/bin/clangd' -type f -print -quit 2>/dev/null || true)"
    "$(find . -path './bazel-*/external/mongo_toolchain_v5/v5/bin/clangd' -type f -print -quit 2>/dev/null || true)"
    "/opt/mongodbtoolchain/v5/bin/clangd"
)

# Find the first available clangd
CLANGD=""
for CANDIDATE in "${CANDIDATES[@]}"; do
    if [[ -n "$CANDIDATE" && -x "$CANDIDATE" ]]; then
        CLANGD="$CANDIDATE"
        echo "[INFO] Using clangd at: $CLANGD" >&2
        break
    fi
done

SKIP_SWAP="${SKIP_SWAP:-0}"

# Fail if no clangd was found
if [[ -z "$CLANGD" ]]; then
    echo "[ERROR] clangd not found in any of the expected locations." >&2
    exit 1
fi

if ! sudo -n true >/dev/null 2>&1; then
    echo "[WARN] sudo would prompt; skipping swap setup." >&2
    SKIP_SWAP=1
fi

if [[ -f /.dockerenv ]] || grep -qaE '(docker|kubepods|containerd)' /proc/1/cgroup 2>/dev/null; then
    echo "[INFO] Container detected; skipping swapon (usually not permitted)." >&2
    SKIP_SWAP=1
fi

FINAL_ARGS=(
    "${ARGS[@]}"
    "--query-driver=./**/*{clang,gcc,g++}*" # allow any clang or gcc binary in the repo
    "--header-insertion=never"
)

# Log the full command (optional)
echo "[INFO] Executing: $CLANGD ${FINAL_ARGS[*]}" >&2

UNIT="clangd-vscode-${PPID}-${RANDOM}"
TMPDIR="$PWD/.tmp"
mkdir -p "$TMPDIR"
export TMPDIR # ensure clangd uses this for tmp

SWAPFILE="$TMPDIR/clangd.swap"
SWAPSIZE_GB=16

have_swap() {
    awk 'NR>1 {found=1} END{exit(found?0:1)}' /proc/swaps
}

sudo_noninteractive_ok() {
    sudo -n true >/dev/null 2>&1
}

swapfile_already_on() {
    # /proc/swaps: Filename Type Size Used Priority
    awk -v sf="$SWAPFILE" 'NR>1 && $1==sf {found=1} END{exit(found?0:1)}' /proc/swaps
}

if [[ "$SKIP_SWAP" != "1" ]]; then
    if ! have_swap; then
        echo "[INFO] No swap enabled; attempting to create $SWAPFILE (${SWAPSIZE_GB}G)..." >&2

        fstype="$(stat -f -c %T "$TMPDIR" 2>/dev/null || echo unknown)"
        case "$fstype" in
        nfs | cifs | smb2 | fuse.* | overlayfs | overlay | 9p | tmpfs)
            echo "[WARN] Filesystem type '$fstype' under $TMPDIR may not support swapfiles. Skipping." >&2
            ;;
        btrfs)
            echo "[WARN] btrfs swapfiles require special setup; skipping auto-swapfile under $TMPDIR." >&2
            ;;
        *)
            if ! sudo_noninteractive_ok; then
                echo "[WARN] sudo would prompt; skipping swap setup to avoid hang." >&2
            else
                # Create only if missing (or size change desired)
                if [[ ! -f "$SWAPFILE" ]]; then
                    rm -f "$SWAPFILE"
                    if ! fallocate -l "${SWAPSIZE_GB}G" "$SWAPFILE" 2>/dev/null; then
                        dd if=/dev/zero of="$SWAPFILE" bs=1M count=$((SWAPSIZE_GB * 1024)) status=progress
                    fi
                    chmod 600 "$SWAPFILE"
                    mkswap "$SWAPFILE" >/dev/null 2>&1 || true
                fi

                # Enable swap from this file (non-interactive, never hangs)
                if ! swapfile_already_on; then
                    if sudo -n swapon "$SWAPFILE" 2>/dev/null; then
                        echo "[INFO] Swap enabled from $SWAPFILE" >&2
                    else
                        echo "[WARN] Could not enable swap (disallowed/error)." >&2
                    fi
                fi
            fi
            ;;
        esac
    fi
else
    echo "[INFO] Swap setup skipped (SKIP_SWAP=1)." >&2
fi

if command -v systemd-run >/dev/null 2>&1; then
    exec systemd-run --user --quiet --collect --pipe \
        --unit="$UNIT" \
        -p MemoryHigh=4G \
        -p MemoryMax=8G \
        -p MemorySwapMax=16G \
        -p IOWeight=10 \
        -p IOSchedulingClass=idle \
        -p IOSchedulingPriority=7 \
        -- "$CLANGD" "${FINAL_ARGS[@]}"
else
    echo "[WARN] systemd-run not available (common in containers). Running clangd without systemd limits." >&2

    # Optional: still be “polite”
    if command -v ionice >/dev/null 2>&1; then
        exec ionice -c3 nice -n 19 "$CLANGD" "${FINAL_ARGS[@]}"
    else
        exec nice -n 19 "$CLANGD" "${FINAL_ARGS[@]}"
    fi
fi
