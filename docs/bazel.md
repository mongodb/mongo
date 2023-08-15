(Note: This is a work-in-progress for the SDP team; contact #server-dev-platform for questions)

To perform a Bazel build via SCons:
* You must be on a arm64 virtual workstation
* Build the Bazel-compatible target: `python3 ./buildscripts/scons.py BAZEL_BUILD_ENABLED=1 --build-profile=fast --ninja=disabled --link-model=static -j 200  --modules= build/fast/mongo/db/commands/libfsync_locked.a`

To perform a Bazel build and *bypass* SCons:
* Install Bazelisk: `curl -L https://github.com/bazelbuild/bazelisk/releases/download/v1.17.0/bazelisk-linux-arm64 --output /tmp/bazelisk && chmod +x /tmp/bazelisk`
* Build the Bazel-compatible target: `/tmp/bazelisk build --verbose_failures  src/mongo/db/commands:fsync_locked`
