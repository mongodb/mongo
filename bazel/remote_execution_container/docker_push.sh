# Push the base image used by bazel remote execution hosts.

docker buildx create --use default

docker buildx build --push \
    --platform linux/arm64/v8,linux/amd64 \
    --tag quay.io/mongodb/bazel-remote-execution:latest .
