(Note: This is a work-in-progress for the SDP team; contact #server-dev-platform for questions)

To perform a Bazel build:
* You must be on a arm64 virtual workstation
* Build the Bazel-compatible target: `python3 ./buildscripts/scons.py BAZEL_BUILD_ENABLED=1 --build-profile=fast --ninja=disabled --link-model=static -j 200  --modules= build/fast/mongo/db/commands/libfsync_locked.a`
