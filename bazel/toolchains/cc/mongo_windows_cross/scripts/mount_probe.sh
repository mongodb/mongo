#!/bin/sh
# $1/$2: output-base/repository probes, $3: sandbox probe, $4: optional NTFS
# symlink probe, $5: expected target. Write checks run in subshells so an expected
# read-only redirection failure cannot abort the remaining checks.
# Use the container utilities' PATH without changing the tool action's PATH.
PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
status=0
present() {
    if [ -e "$1" ]; then echo "present: $1"; else
        echo "MISSING: $1"
        status=1
    fi
}
can_write() {
    (: >"$1") 2>/dev/null
}
present "$1"
present "$2"
present "$3"
if can_write "$1.container-write"; then
    rm -f "$1.container-write"
    echo "WRITABLE (expected read-only): ${1%/*}"
    status=1
else
    echo "read-only: ${1%/*}"
fi
if can_write "$3.container-write"; then
    rm -f "$3.container-write"
    echo "writable: ${3%/*}"
else
    echo "NOT WRITABLE: ${3%/*}"
    status=1
fi
if [ -n "$4" ]; then
    target="$(readlink "$4" 2>&1)"
    echo "symlink: $4 -> $target"
    if [ -f "$4" ]; then
        echo "symlink resolves: $4"
    else
        echo "SYMLINK DOES NOT RESOLVE (expected target $5): $4"
        status=1
    fi
fi
exit $status
