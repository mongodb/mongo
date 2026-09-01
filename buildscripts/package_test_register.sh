#!/bin/bash
# Register the RHEL package-test host so its entitlement can be mounted into UBI.
# This script must be invoked by root or with sudo.

set -o errexit
set -o nounset
set -o pipefail

if ! grep --quiet "Red Hat Enterprise Linux" /etc/os-release; then
    echo "Distro is not RHEL; skipping registration"
    exit 0
fi

usage() {
    cat <<EOF
usage: $0 -a <add|remove>
EOF
}

while getopts "a:?" option; do
    case "$option" in
    a)
        action="$OPTARG"
        ;;
    \? | *)
        usage
        exit 1
        ;;
    esac
done

case "${action:-}" in
add)
    : "${RHN_USER:?RHN_USER must be set}"
    : "${RHN_PASS:?RHN_PASS must be set}"
    source /etc/os-release
    sudo subscription-manager register \
        --auto-attach \
        --release="$VERSION_ID" \
        --username="$RHN_USER" \
        --password="$RHN_PASS" \
        --force
    ;;
remove)
    sudo subscription-manager remove --all
    sudo subscription-manager unregister
    sudo subscription-manager clean
    ;;
*)
    usage
    exit 1
    ;;
esac
