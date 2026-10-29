#!/usr/bin/env bash

set -euo pipefail

test_root="${TEST_TMPDIR}/venv_adjust"
# Keep native drive-letter colons out of PATH so the fake commands are found.
if command -v cygpath >/dev/null 2>&1; then
    test_root=$(cygpath -u "$test_root")
fi
mkdir -p "$test_root/src/evergreen/functions" "$test_root/venv/Scripts" "$test_root/venv/Lib/site-packages"
cp "${TEST_SRCDIR}/${TEST_WORKSPACE}/evergreen/functions/venv_adjust.sh" \
    "$test_root/src/evergreen/functions/venv_adjust.sh"

cat >"$test_root/python.exe" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" >"$FAKE_PYTHON_LOG"
EOF
chmod +x "$test_root/python.exe"
cat >"$test_root/src/evergreen/prelude_python.sh" <<'EOF'
python="$FAKE_PYTHON"
EOF
printf '%s\n' 'VIRTUAL_ENV="C:\build-host\venv"' >"$test_root/venv/Scripts/activate"
printf '%s\n' 'home = C:\build-host\bazel-output\dist' >"$test_root/venv/pyvenv.cfg"
printf '%s\n' 'installed dependency' >"$test_root/venv/Lib/site-packages/dependency.py"

export FAKE_PYTHON="$test_root/python.exe"
export FAKE_PYTHON_LOG="$test_root/python.log"
mkdir -p "$test_root/bin"
cat >"$test_root/bin/cygpath" <<'EOF'
#!/usr/bin/env bash
[[ "$1" == '-w' ]] || exit 1
printf '%s\n' 'C:/receiving host/venv'
EOF
chmod +x "$test_root/bin/cygpath"
cat >"$test_root/bin/uname" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' 'x86_64'
EOF
chmod +x "$test_root/bin/uname"
(
    cd "$test_root"
    OS=Windows_NT PATH="$test_root/bin:$PATH" bash src/evergreen/functions/venv_adjust.sh
)

# Refresh from the receiving host's runtime without pip or clearing packages.
printf '%s\n' '-m' 'venv' '--upgrade' '--without-pip' 'C:/receiving host/venv' >"$test_root/expected.log"
cmp "$test_root/expected.log" "$FAKE_PYTHON_LOG"
grep -Fq "$test_root/venv" "$test_root/venv/Scripts/activate"
grep -q 'installed dependency' "$test_root/venv/Lib/site-packages/dependency.py"
