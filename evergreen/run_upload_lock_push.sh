#!/usr/bin/env bash

# in the future we will want to errexit, but only once we remove
# continue_on_err from the command

# executables and source archive are always expected on every build
# source archives should be fine to be uploaded by whichever variant gets
# there first
declare -A ARTIFACTS=(
    [${SERVER_TARBALL_PATH}]=${SERVER_TARBALL_KEY}
    [${SOURCE_TARBALL_PATH}]=${SOURCE_TARBALL_KEY}
    [${SERVER_TARBALL_SIGNATURE_PATH}]=${SERVER_TARBALL_SIGNATURE_KEY}
    [${SOURCE_TARBALL_SIGNATURE_PATH}]=${SOURCE_TARBALL_SIGNATURE_KEY}
    [${SERVER_TARBALL_SHA1_PATH}]=${SERVER_TARBALL_SHA1_KEY}
    [${SOURCE_TARBALL_SHA1_PATH}]=${SOURCE_TARBALL_SHA1_KEY}
    [${SERVER_TARBALL_SHA256_PATH}]=${SERVER_TARBALL_SHA256_KEY}
    [${SOURCE_TARBALL_SHA256_PATH}]=${SOURCE_TARBALL_SHA256_KEY}
    [${SERVER_TARBALL_MD5_PATH}]=${SERVER_TARBALL_MD5_KEY}
    [${SOURCE_TARBALL_MD5_PATH}]=${SOURCE_TARBALL_MD5_KEY}
)

# mongocryptd is only built for enterprise variants
if [ -f "${CRYPTD_TARBALL_PATH}" ]; then
    ARTIFACTS[${CRYPTD_TARBALL_PATH}]=${CRYPTD_TARBALL_KEY}
    ARTIFACTS[${CRYPTD_TARBALL_SIGNATURE_PATH}]=${CRYPTD_TARBALL_SIGNATURE_KEY}
    ARTIFACTS[${CRYPTD_TARBALL_SHA1_PATH}]=${CRYPTD_TARBALL_SHA1_KEY}
    ARTIFACTS[${CRYPTD_TARBALL_SHA256_PATH}]=${CRYPTD_TARBALL_SHA256_KEY}
    ARTIFACTS[${CRYPTD_TARBALL_MD5_PATH}]=${CRYPTD_TARBALL_MD5_KEY}
fi

# mongohouse only built sometimes
# we do not sign mongohouse, so no detached signature and no checksums
if [ -f "${MONGOHOUSE_TARBALL_PATH}" ]; then
    ARTIFACTS[${MONGOHOUSE_TARBALL_PATH}]=${MONGOHOUSE_TARBALL_KEY}
fi

# debug symbols are only built sometimes
# not clear which variants that is the case for
if [ -f "${DEBUG_SYMBOLS_TARBALL_PATH}" ]; then
    ARTIFACTS[${DEBUG_SYMBOLS_TARBALL_PATH}]=${DEBUG_SYMBOLS_TARBALL_KEY}
    ARTIFACTS[${DEBUG_SYMBOLS_TARBALL_SIGNATURE_PATH}]=${DEBUG_SYMBOLS_TARBALL_SIGNATURE_KEY}
    ARTIFACTS[${DEBUG_SYMBOLS_TARBALL_SHA1_PATH}]=${DEBUG_SYMBOLS_TARBALL_SHA1_KEY}
    ARTIFACTS[${DEBUG_SYMBOLS_TARBALL_SHA256_PATH}]=${DEBUG_SYMBOLS_TARBALL_SHA256_KEY}
    ARTIFACTS[${DEBUG_SYMBOLS_TARBALL_MD5_PATH}]=${DEBUG_SYMBOLS_TARBALL_MD5_KEY}
fi

# MSIs are only built on windows
# note there is no detached signature file
if [ -f "${MSI_PATH}" ]; then
    ARTIFACTS[${MSI_PATH}]=${MSI_KEY}
    ARTIFACTS[${MSI_SHA1_PATH}]=${MSI_SHA1_KEY}
    ARTIFACTS[${MSI_SHA256_PATH}]=${MSI_SHA256_KEY}
    ARTIFACTS[${MSI_MD5_PATH}]=${MSI_MD5_KEY}
fi

set -o verbose

for path in "${!ARTIFACTS[@]}"; do

    key=${ARTIFACTS[${path}]}
    podman run \
        -v $(pwd):$(pwd) \
        -w $(pwd) \
        --env-host \
        ${UPLOAD_LOCK_IMAGE} \
        -key=${key} -tag=task-id=${EVERGREEN_TASK_ID} ${path}

done
