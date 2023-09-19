#!/bin/bash
#
# The script is used to setup the environment for a spawn host. Currently the script
# unzips the test artifacts, prepend the toolchain path to the PATH environment and exports the
# LD_LIBRARY_PATH.
#
# Once setup, the script will notify the user who spawned the host that it is finished.
#
cd $HOME || exit 1
TOOLCHAIN_ROOT=/opt/mongodbtoolchain/v4

# Communicate to users that logged in before the script started that nothing is ready.
wall "The setup_spawn_host script has just started setting up the debugging environment."

# Make a directory on the larger volume. Soft-link it under the home directory.
mkdir -p /data/wiredtiger
ln -s /data/wiredtiger .
cd wiredtiger || exit 1

# Find the test artifacts.
WT_ARCHIVE=$(ls /data/mci/artifacts-*/*.tgz | grep -v "compile" | head -n 1 2>/dev/null)
if [[ -n $WT_ARCHIVE ]]; then
    tar --wildcards -xzf $WT_ARCHIVE
fi

# Setup the gdb environment if there are core dumps present in the artefacts.
GDB_CORE_DUMP=$(find ${HOME}/wiredtiger/cmake_build -name "*.core" | head -n 1 2>/dev/null)
if [[ -n $GDB_CORE_DUMP ]]; then
    # Read the CMakeCache txt file to find the old source directory and fix up the expected path to
    # the correct path on the new machine.
    OLD_WT_PATH=$(grep "WiredTiger_SOURCE_DIR" ${HOME}/wiredtiger/cmake_build/CMakeCache.txt | sed -e 's/[^\/]*//')

    # GDB debug symbols includes a path to the source code we originally compiled. However since we
    # compiled on a different machine and copied the artefacts to this spawn host, the file paths
    # have changed. substitute-path command tells gdb the new location of the source code. Therefore
    # set up the shared library paths and add another source directory to look under using
    # substitute path.
    cat >> ~/.gdbinit << EOF
set solib-search-path ${HOME}/wiredtiger/cmake_build/lang/python:${HOME}/wiredtiger/cmake_build:${HOME}/wiredtiger/TCMALLOC_LIB/lib
set substitute-path ${OLD_WT_PATH} ${HOME}/wiredtiger
set print pretty on
EOF

fi

# Install CMake into the machine.
. test/evergreen/find_cmake.sh

cat >> ~/.profile << EOF
# Prepend the toolchain to the PATH environment and export the TCMALLOC and WT library into the
# library path.
export PATH="${TOOLCHAIN_ROOT}/bin:\$PATH"
export LD_LIBRARY_PATH="${HOME}/wiredtiger/cmake_build:${HOME}/wiredtiger/TCMALLOC_LIB/lib:\$LD_LIBRARY_PATH"
EOF

echo 'if [ -f ~/.profile ]; then
. ~/.profile
fi' >> ~/.bash_profile

# Send a Slack notification as the very last thing the setup_spawn_host script does.
ssh_user=$(whoami)
evg_credentials_pathname=~/.evergreen.yml
evg_binary_pathname=evergreen

wall "The setup_spawn_host script has completed, please relogin to ensure the right environment variables are set."

slack_user=$(awk '{if ($1 == "user:") print $2}' "$evg_credentials_pathname")
# Refer to the https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/instancedata-data-retrieval.html
# documentation for more information on the AWS instance metadata endpoints.
aws_metadata_svc="http://169.254.169.254"
aws_token=$(curl -s -X PUT "$aws_metadata_svc/latest/api/token" -H 'X-aws-ec2-metadata-token-ttl-seconds: 60')
ssh_host=$(curl -s -H "X-aws-ec2-metadata-token: $aws_token" "$aws_metadata_svc/latest/meta-data/public-hostname")
slack_message="The setup_spawn_host script has finished setting things up. Please run "'```'"ssh $ssh_user@$ssh_host"'```'" to log in."

# The Evergreen spawn host is expected to be provisioned with the user's .evergreen.yml credentials.
# But in case something unexpected happens we don't want the setup_spawn_host script itself
# to error.
if [[ -n "${slack_user}" ]]; then
    "$evg_binary_pathname" --config "$evg_credentials_pathname" notify slack -t "@$slack_user" -m "$slack_message"
fi
