#!/usr/bin/env bash

set -euo pipefail

repo_root="${TEST_SRCDIR}/${TEST_WORKSPACE}"
test_root="${TEST_TMPDIR}/gcp oidc"
mkdir -p "${test_root}/src/evergreen" "${TEST_TMPDIR}/gcp-home"
cp "${repo_root}/evergreen/external_auth_gcp_setup.sh" "${test_root}/src/evergreen/"
cp "${repo_root}/evergreen/external_auth_gcp_teardown.sh" "${test_root}/src/evergreen/"
# Redirect the scripts' home-relative artifacts into the test fixture.
sed -i 's/\bHOME\b/GCP_TEST_HOME/g' "${test_root}/src/evergreen/"external_auth_gcp_{setup,teardown}.sh
if [[ -f "${repo_root}/evergreen/external_auth_gcp_venv.sh" ]]; then
    cp "${repo_root}/evergreen/external_auth_gcp_venv.sh" "${test_root}/src/evergreen/"
fi
cp "${repo_root}/evergreen/external_auth_gcp_requirements.txt" "${test_root}/src/evergreen/"

cat >"${test_root}/src/evergreen/prelude.sh" <<'EOF'
workdir="$GCP_TEST_ROOT"
python="$GCP_TEST_PYTHON"
activate_venv() { :; }
ssh() { :; }
EOF

# Reject any installation into the shared resmoke environment.
cat >"${TEST_TMPDIR}/resmoke-python" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == "-I" && "$2" == "-m" && "$3" == "venv" ]] || exit 91
printf 'venv\n' >>"$GCP_TEST_LOG"
mkdir -p "$4/bin"
cp "$GCP_TEST_ROOT/gcp-python" "$4/bin/python"
EOF

cat >"${test_root}/gcp-python" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
# Isolated mode must be used for pip and the VM helper, since Evergreen's
# PYTHONPATH can include resmoke's site-packages.
[[ "$1" == "-I" ]] || exit 92
shift
if [[ "$1" == "-m" && "$2" == "pip" && "$3" == "install" ]]; then
    [[ "$4" == "-r" && -f "$5" ]] || exit 94
    printf 'pip\n' >>"$GCP_TEST_LOG"
    exit "${GCP_TEST_PIP_STATUS:-0}"
fi
[[ "$1" == *"/gce_vm_manager.py" ]] || exit 93
printf '%s\n' "$2" >>"$GCP_TEST_LOG"
if [[ "$2" == "create" ]]; then
    touch "$GCP_TEST_HOME/gce_vm_info.json"
fi
exit "${GCP_TEST_HELPER_STATUS:-0}"
EOF
chmod +x "${TEST_TMPDIR}/resmoke-python" "${test_root}/gcp-python"

export GCP_TEST_ROOT="${test_root}"
export GCP_TEST_LOG="${test_root}/calls"
export GCP_TEST_PYTHON="${TEST_TMPDIR}/resmoke-python"
export GCP_TEST_HOME="${TEST_TMPDIR}/gcp-home"
export GOOGLE_APPLICATION_CREDENTIALS="${GCP_TEST_HOME}/gcp_credentials.json"
export oidc_gcp_vm_id_token_audience="test"
export oidc_gcp_project_id="test"
export oidc_gcp_zone="test"
export oidc_gcp_vm_instance_template_url="test"
export oidc_gcp_ssh_key="dGVzdA=="
export oidc_gcp_service_account_key="dGVzdA=="
export task_name="external_auth_oidc_gcp"
export PYTHONPATH="${test_root}/resmoke-site-packages"

run_script() {
    local script="$1"
    local expected_status="$2"
    local status=0
    (cd "${test_root}" && bash "src/evergreen/${script}") >"${test_root}/output" 2>&1 || status=$?
    if [[ "$status" -ne "$expected_status" ]]; then
        cat "${test_root}/output" >&2
        echo "${script}: expected ${expected_status}, got ${status}" >&2
        exit 1
    fi
}

assert_calls() {
    local expected="$1"
    local actual
    actual="$(cat "${GCP_TEST_LOG}")"
    if [[ "$actual" != "$expected" ]]; then
        echo "Expected calls: ${expected}; got: ${actual}" >&2
        exit 1
    fi
}

# Setup and teardown must invoke only the isolated environment for pip and GCE.
run_script external_auth_gcp_setup.sh 0
assert_calls $'venv\npip\ncreate'
: >"${GCP_TEST_LOG}"
run_script external_auth_gcp_teardown.sh 0
assert_calls $'venv\npip\ndelete'
[[ ! -f "${GOOGLE_APPLICATION_CREDENTIALS}" && ! -f "${GCP_TEST_HOME}/gcp_ssh_key" ]]

# Dependency installation failure must stop before creating a VM.
: >"${GCP_TEST_LOG}"
export GCP_TEST_PIP_STATUS=42
run_script external_auth_gcp_setup.sh 42
assert_calls $'venv\npip'
unset GCP_TEST_PIP_STATUS

# A VM helper failure must propagate to Evergreen.
: >"${GCP_TEST_LOG}"
export GCP_TEST_HELPER_STATUS=43
run_script external_auth_gcp_setup.sh 43
assert_calls $'venv\npip\ncreate'
: >"${GCP_TEST_LOG}"
run_script external_auth_gcp_teardown.sh 43
assert_calls $'venv\npip\ndelete'
unset GCP_TEST_HELPER_STATUS

# Other tasks must not install dependencies or attempt GCP teardown.
: >"${GCP_TEST_LOG}"
export task_name="other_task"
run_script external_auth_gcp_teardown.sh 0
assert_calls ''
echo "GCP OIDC environment regression tests passed"
