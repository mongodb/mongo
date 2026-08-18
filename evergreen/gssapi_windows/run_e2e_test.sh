#!/bin/bash
# Driver for the Windows GSSAPI/SSPI end-to-end test, run on the Windows driver host.
#
# A separate host is used because promoting to an AD domain controller reboots the host, which an
# Evergreen task host cannot do. Evergreen allows one host.create per task, so that single host is
# made the whole world: its own AD forest (WINGSSAPI.LOCAL) running the KDC, mongod, and the client.
# Steps: ship binaries+scripts over ssh -> promote (reboot) -> configure -> run the test.
#
# $workdir is an Evergreen expansion (the task root); the repo lives in $workdir/$k_src (k_src="src").
set -o errexit
set -o verbose

readonly k_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
# prelude.sh (in the top-level evergreen/ dir) sets $workdir and defines activate_venv.
# shellcheck disable=SC1091
. "$k_dir/../prelude.sh"
activate_venv
readonly k_src="src"

readonly REALM="WINGSSAPI.LOCAL"
readonly SERVICE_NAME="mockservice"
readonly SERVICE_HOSTNAME="localhost"
readonly DSRM_PASSWORD="Secret123!" # AD safe-mode (DSRM) password
# Dedicated test principal (created by configure_dc.ps1). The client authenticates with its
# password, not a logon-session ticket: public-key ssh never gives the session a Kerberos key.
readonly USER_NAME="mockuser"
# Must satisfy the domain complexity policy, which rejects passwords containing the account name.
readonly USER_PASSWORD="Str0ngE2E#Kerb"
readonly USER_PRINCIPAL="${USER_NAME}@${REALM}"

# --- Locate the Windows host --------------------------------------------------------------------
# Use the private ipv4_address from hosts.yml: it is routable from the driver inside the Evergreen
# VPC, whereas the public dns_name is not.
cat "$workdir"/$k_src/hosts.yml
win_host="$(tr -d '"{}[]' <"$workdir"/$k_src/hosts.yml | tr ',' '\n' | awk -F : '/ipv4_address/{print $2}')"
win_user="Administrator"
win_target="${win_user}@${win_host}"

# --- SSH key (same source as selinux_run_test.sh) -----------------------------------------------
# Use the venv python explicitly: the driver's system python lacks pyyaml and yields an empty key.
ssh_key="$workdir/gssapi_windows.pem"
"$python" "$workdir"/$k_src/buildscripts/yaml_key_value.py --yamlFile="$workdir"/expansions.yml \
    --yamlKey=__project_aws_ssh_key_value >"$ssh_key"
if [ ! -s "$ssh_key" ]; then
    echo "ERROR: failed to extract the ssh key from expansions.yml" >&2
    exit 1
fi
chmod 600 "$ssh_key"
ssh_opts="-i $ssh_key -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o ConnectTimeout=30"

remote() { ssh $ssh_opts "$win_target" "$@"; }

# LastBootUpTime as a monotonic integer; empty when the host is unreachable (rebooting).
boot_time() {
    remote "powershell -NoProfile -Command \"(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToFileTimeUtc()\"" 2>/dev/null | tr -dc '0-9'
}

wait_for_ssh() {
    # A fresh Windows host is slow to finish sshd provisioning, so allow ~30 min. Grep for a token
    # rather than check exit status: a bare remote command returns nonzero even when ssh is fine.
    local attempts=0
    until remote "cmd /c echo sshready" 2>/dev/null | grep -q sshready; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 120 ]; then
            echo "ERROR: Windows host $win_host not reachable over ssh; last handshake:" >&2
            ssh -v $ssh_opts "$win_target" "cmd /c echo sshready" || true
            exit 1
        fi
        sleep 15
    done
}

# Wait for sshd on the freshly provisioned host.
wait_for_ssh

# Log what the created host provides (default shell + the POSIX tools later steps rely on).
echo "--- remote environment ---"
remote "cmd /c ver" || true
remote "where bash tar cygpath ssh 2>NUL" || true
echo "---------------------------"

# --- Ship the binaries + the paths the test needs -----------------------------------------------
# Ship only the binaries, the jstest, and the infra scripts (a tarball over scp), not the whole repo.
payload="$workdir/gssapi_e2e_payload.tar.gz"
tar -czf "$payload" -C "$workdir/$k_src" \
    dist-test \
    evergreen/gssapi_windows \
    src/mongo/db/modules/enterprise/jstests/external_auth/gssapi_windows_sspi_e2e.js
remote "cmd /c \"if not exist work mkdir work\"" || true
scp $ssh_opts "$payload" "$win_target":work/payload.tar.gz
remote "cd work && tar -xzf payload.tar.gz"

# --- Promote to its own AD DC forest ------------------------------------------------------------
# promote_to_dc.ps1 uses -NoRebootOnCompletion so promotion does not drop our ssh; we reboot below.
remote "powershell -ExecutionPolicy Bypass -File work/evergreen/gssapi_windows/promote_to_dc.ps1" \
    "-Realm '$REALM' -SafeModePassword '$DSRM_PASSWORD'"

# Reboot and wait until LastBootUpTime actually changes, so configure_dc.ps1 runs on the live DC.
echo "Rebooting to finalize DC promotion..."
pre_boot="$(boot_time)"
echo "Pre-reboot boot time: $pre_boot"
remote "shutdown /r /t 0 /f" || true
attempts=0
until
    b="$(boot_time)"
    [ -n "$b" ] && [ "$b" != "$pre_boot" ]
do
    attempts=$((attempts + 1))
    if [ "$attempts" -ge 80 ]; then
        echo "ERROR: host did not reboot (boot time unchanged after ~20m)" >&2
        exit 1
    fi
    sleep 15
done
echo "Reboot confirmed; new boot time: $b"

# --- Register the service SPN and create the client principal now that the DC is up -------------
# PowerShell exit codes do not survive the ssh -> cmd -> powershell chain, so assert on the script's
# final "DC configured:" line instead.
configure_out="$(remote "powershell -ExecutionPolicy Bypass -File work/evergreen/gssapi_windows/configure_dc.ps1" \
    "-Realm '$REALM' -ServiceName '$SERVICE_NAME' -ServiceHostname '$SERVICE_HOSTNAME'" \
    "-UserName '$USER_NAME' -UserPassword '$USER_PASSWORD'" 2>&1 | tee /dev/stderr)"
if ! grep -q "DC configured:" <<<"$configure_out"; then
    echo "ERROR: configure_dc.ps1 did not complete; see output above" >&2
    exit 1
fi

# --- Run the E2E jstest as SYSTEM (see run_jstest_system.ps1) ------------------------------------
# mongod must run as SYSTEM (the machine account) to match the SPN owner; the ssh session has no
# Kerberos key. Create MongoRunner's default data dir (/data/db -> C:\data\db) first.
remote "cmd /c \"if not exist C:\\data\\db mkdir C:\\data\\db\"" || true
remote "powershell -ExecutionPolicy Bypass -File work/evergreen/gssapi_windows/run_jstest_system.ps1" \
    "-Realm '$REALM' -ServiceName '$SERVICE_NAME' -ServiceHostname '$SERVICE_HOSTNAME'" \
    "-UserPrincipal '$USER_PRINCIPAL' -UserPassword '$USER_PASSWORD'"
