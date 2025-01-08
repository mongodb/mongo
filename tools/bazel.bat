@echo off
echo common --//bazel/config:running_through_bazelisk > .bazelrc.bazelisk
"%BAZEL_REAL%" %*
