#!/bin/bash

set -o errexit

SCRIPT_DIR=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
pushd $SCRIPT_DIR >/dev/null

# Check npm version is at least 10
NPM_VERSION=$(npm --version | cut -d. -f1)
if [ "$NPM_VERSION" -lt 10 ]; then
    echo "Error: npm version 10 or higher is required (found: $(npm --version))"
    exit 1
fi

npm install

# Get version from package.json
VERSION=$(node -p "require('./package.json').version")
EXT_NAME=$(node -p "require('./package.json').name")

# Package extension
npm run package

# Install via VSCode
code --install-extension $EXT_NAME-$VERSION.vsix
rm $EXT_NAME-$VERSION.vsix

# Cleanup node_modules (otherwise buildtools expect bazel files to be present)
rm -rf node_modules

popd >/dev/null
