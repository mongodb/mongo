#!/bin/env bash
# update_import.sh [git repo directory]
# 
# Update the import.sh script with a fresh libmongogrypt git hash
#

set -euo pipefail
IFS=$'\n\t'

if [ "$#" -ne 1 ]; then
    echo "This script takes one argument: directory for libmongocrypt repo"
    exit 1
fi

GIT_REPO=$1

if [ ! -d "$GIT_REPO" ]; then
  echo "ERROR: $GIT_REPO is not a directory"
  exit 1
fi

echo "Updating repo"
git -C "$GIT_REPO" fetch origin

GIT_HASH=$(git -C "$GIT_REPO" rev-parse origin/master~0)

echo Git hash: "$GIT_HASH"
 
sed -i "s/REVISION=.*/REVISION=$GIT_HASH/" import.sh

echo Done updating import.sh

