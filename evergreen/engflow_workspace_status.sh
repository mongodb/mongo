#!/bin/bash

# https://docs.engflow.com/web_ui/index.html#workspace-status
echo BUILD_SCM_BRANCH $(git rev-parse --abbrev-ref HEAD)
echo BUILD_SCM_REVISION $(git rev-parse --verify HEAD)

git diff-index --quiet HEAD --
if [[ $? == 0 ]]; then
    status="clean"
else
    status="modified"
fi
echo BUILD_SCM_STATUS $status
echo BUILD_SCM_REMOTE git@github.com:10gen/mongo.git
