#!/bin/bash
# Script used to manage redhat subscription. Only used for test_packages! (ask runtime environments for other use cases)
# This script must be invoked either by a root user or with sudo.
# Only works on redhat enterprise linux.
# See _usage_ for how this script should be invoked.

set -o errexit


echo $0 $@


# _usage_: Provides usage infomation
function _usage_ {
  cat << EOF
usage: $0 options
  -a <action>,    REQUIRED, Must be one of "add" or "remove"
EOF
}


# Parse command line options
while getopts "a:?" option
do
  case $option in
    a)
      action=$OPTARG
      ;;
    \?|*)
      echo "missing args"
      _usage_
      exit 1
      ;;
  esac
done


case $action in

  add)
    if [ -z $RHN_USER ] || [ -z $RHN_PASS ] 
      then
              echo "missing rhn username or password"
              exit 1
    fi
    echo "add subscription"
    source /etc/os-release
    sudo subscription-manager register --auto-attach --release=$VERSION_ID --username=$RHN_USER --password=$RHN_PASS --force
    ;;

  remove)
    echo "remove subscription"
    sudo subscription-manager remove --all
    sudo subscription-manager unregister
    sudo subscription-manager clean
    ;;

  *)
    echo "invalid action"
    _usage_
    exit 1
    ;;
esac

