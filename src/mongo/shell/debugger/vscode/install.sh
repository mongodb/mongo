#!/bin/bash

cd $MONGO_REPO
cd src/mongo/shell/debugger/vscode

npm install

# Get version from package.json
VERSION=$(node -p "require('./package.json').version")
EXT_NAME=$(node -p "require('./package.json').name")

# Install vsce to package extensions
npm install -g @vscode/vsce

# Package extension
vsce package --skip-license

# Install via VSCode
code --install-extension $EXT_NAME-$VERSION.vsix
rm $EXT_NAME-$VERSION.vsix

cd $MONGO_REPO
