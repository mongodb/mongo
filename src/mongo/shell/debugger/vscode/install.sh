#!/bin/bash

cd $MONGO_REPO
cd src/mongo/shell/debugger/vscode

npm install

# Get version from package.json
VERSION=$(node -p "require('./package.json').version")
EXT_NAME=$(node -p "require('./package.json').name")

# Package extension
npm run package

# Install via VSCode
code --install-extension $EXT_NAME-$VERSION.vsix
rm $EXT_NAME-$VERSION.vsix

cd $MONGO_REPO
