#!/bin/bash

# Notes on how to run this manually:
# - repo must be unpacked into source tree
#
# export SSH_KEY=$HOME/.ssh/id_rsa
# export SELINUX_HOSTNAME=ec2-3-91-230-150.compute-1.amazonaws.com
# export SELINUX_USER=ec2-user
# export BYPASS_PRELUDE=yes
# export SRC="$(basename $(pwd) | tee /dev/stderr)"
# export TEST_LIST='jstests/selinux/*.js'
# export workdir="$(dirname $(pwd) | tee /dev/stderr)"
# evergreen/selinux_run_test.sh

set -o errexit

readonly k_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

if [ "$BYPASS_PRELUDE" != "yes" ]; then
    . "$k_dir/prelude.sh"
    activate_venv
    readonly k_src="src"
else
    readonly k_src="$SRC"
fi

# If no selinux hostname is defined by external env, then we are running through evergreen, which has dumped spawn host
# properties about this host into hosts.yml via host.list
# (https://github.com/evergreen-ci/evergreen/blob/main/docs/Project-Configuration/Project-Commands.md#hostlist),
# from which we can derive the hostname of the remote host
# Also note that $workdir here is a built-in expansion from evergreen: see more info at
# https://github.com/evergreen-ci/evergreen/blob/main/docs/Project-Configuration/Project-Configuration-Files.md#default-expansions
if [ "$SELINUX_HOSTNAME" == "" ]; then
    readonly k_selinux_hostname="$(tr -d '"[]{}' <"$workdir"/$k_src/hosts.yml | cut -d , -f 1 | awk -F : '{print $2}')"
    cat "$workdir"/$k_src/hosts.yml
else
    readonly k_selinux_hostname="$SELINUX_HOSTNAME"
fi

# SELINUX_USER injected from evergreen config, do not change
readonly k_host="${SELINUX_USER}@${k_selinux_hostname}"

# Obtain the ssh key and properties from expansions.yml, output from evergreen via the expansions.write command
# (https://github.com/evergreen-ci/evergreen/blob/main/docs/Project-Configuration/Project-Commands.md#expansionswrite)
if [ "$SSH_KEY" == "" ]; then
    readonly k_ssh_key="$workdir/selinux.pem"

    "$workdir"/$k_src/buildscripts/yaml_key_value.py --yamlFile="$workdir"/expansions.yml \
        --yamlKey=__project_aws_ssh_key_value >"$k_ssh_key"

    chmod 600 "$k_ssh_key"

    result="$(openssl rsa -in "$k_ssh_key" -check -noout | tee /dev/stderr)"

    if [ "$result" != "RSA key ok" ]; then
        exit 1
    fi
else
    readonly k_ssh_key="$SSH_KEY"
fi

readonly k_ssh_options="-i $k_ssh_key -o IdentitiesOnly=yes -o StrictHostKeyChecking=no"

function copy_sources_to_target() {

    rsync -ar -e "ssh $k_ssh_options" \
        --exclude 'tmp' --exclude 'build' --exclude '.*' \
        "$workdir"/$k_src/* "$k_host":

    return $?
}

function configure_target_machine() {
    ssh $k_ssh_options "$k_host" evergreen/selinux_test_setup.sh
    return $?
}

function execute_tests_on_target() {
    ssh $k_ssh_options "$k_host" evergreen/selinux_test_executor.sh "$1"
    return $?
}

function check_remote_connectivity() {
    ssh -q $k_ssh_options -o ConnectTimeout=10 "$k_host" echo "I am working"
    return $?
}

function retry_command() {

    local connection_attempts=$1
    local cmd="$2"
    shift 2 #eat the first 2 parameters to pass on any remaining to the calling function

    local attempts=0
    set +o errexit

    while true; do
        "$cmd" "$@"

        local result=$?

        if [[ $result -eq 0 ]]; then
            set -o errexit
            return $result
        fi

        if [[ $attempts -ge $connection_attempts ]]; then
            printf "%s failed after %d attempts with final error code %s.\n" "$cmd" "$attempts" "$result"
            exit 1
        fi

        sleep 10
        ((attempts++))

    done
}

echo "===> Checking for remote connectivity..."
retry_command 20 check_remote_connectivity

echo "===> Copying sources to target..."
retry_command 5 copy_sources_to_target

echo "===> Configuring target machine..."
retry_command 5 configure_target_machine

echo "===> Executing tests..."
readonly list="$(
    cd src

    # $TEST_LIST defined in evegreen "run selinux tests" function, do not change
    for x in $TEST_LIST; do echo "$x"; done
)"

for test in $list; do
    execute_tests_on_target "$test"
    res="$?"
    if [[ $res -ne 0 ]]; then
        exit "$res"
    fi
done
