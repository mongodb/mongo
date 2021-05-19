#!/bin/bash

# For FCV testing only.
# Tag the local branch with the new tag before running tests.

set -o errexit
set -o verbose

cd src
git config user.name "Evergreen patch build"
git config user.email "evergreen@mongodb.com"
git tag -a r5.1.0-alpha -m 5.1.0-alpha
git describe
