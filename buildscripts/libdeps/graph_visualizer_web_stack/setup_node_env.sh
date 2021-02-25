#!/bin/bash

SCRIPTPATH="$( cd "$(dirname "$BASH_SOURCE")" >/dev/null 2>&1 ; pwd -P )"
pushd $SCRIPTPATH > /dev/null

function quit {
  popd > /dev/null
}
trap quit EXIT
trap quit SIGINT
trap quit SIGTERM

export NVM_DIR="$HOME/.nvm"
if [ -s "$NVM_DIR/nvm.sh" ]
then
    \. "$NVM_DIR/nvm.sh"
else
    curl -o- https://raw.githubusercontent.com/creationix/nvm/v0.34.0/install.sh | sh
    \. "$NVM_DIR/nvm.sh"
fi

nvm install 12

if [ "$1" = "install" ]
then
    npm install
fi

if [ "$1" = "start" ]
then
    npm start
fi

if [ "$1" = "build" ]
then
    npm run build
fi

if [ "$1" = "update" ]
then
    set -u
    git -C "$NVM_DIR" fetch --tags
    TAG=$(git -C "$NVM_DIR" describe --tags `git -C "$NVM_DIR" rev-list --tags --max-count=1`)
    echo "Checking out tag $TAG..."
    git -C "$NVM_DIR" checkout "$TAG"

    . "$NVM_DIR/nvm.sh"
fi
popd > /dev/null