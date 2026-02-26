#!/bin/sh
ln -sf ../../git-hooks/pre-commit.sh "$(git rev-parse --show-toplevel)/.git/hooks/pre-commit"

