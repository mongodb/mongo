#!/bin/bash

# Push the base images used by bazel remote execution hosts.

timestamp=$(date +%s)

for dir in */; do
    cd "$dir" || continue

    echo "Building Docker image in $dir..."

    # Run docker buildx commands
    docker buildx create --use default
    docker buildx build --push \
        --platform linux/arm64/v8,linux/amd64 \
        --tag quay.io/mongodb/bazel-remote-execution:${dir%?}-${timestamp} .

    echo "Build completed in $dir"

    cd ..
done
