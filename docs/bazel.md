(Note: This is a work-in-progress for the SDP team; contact #server-dev-platform for questions)

To perform a Bazel build via SCons:
* You must be on a arm64 virtual workstation
* Build the Bazel-compatible target: `python3 ./buildscripts/scons.py BAZEL_BUILD_ENABLED=1 --build-profile=fast --ninja=disabled --link-model=static -j 200  --modules= build/fast/mongo/db/commands/libfsync_locked.a`

To perform a Bazel build and *bypass* SCons:
* Install Bazelisk: `curl -L https://github.com/bazelbuild/bazelisk/releases/download/v1.17.0/bazelisk-linux-arm64 --output /tmp/bazelisk && chmod +x /tmp/bazelisk`
* Build the Bazel-compatible target: `/tmp/bazelisk build --verbose_failures  src/mongo/db/commands:fsync_locked`

To perform a Bazel build using a local Buildfarm (to test remote execution capability):
* For more details on Buildfarm, see https://bazelbuild.github.io/bazel-buildfarm
* (One time only) Build and start the Buildfarm:
** Change into the `buildfarm` directory: `cd buildfarm`
** Build the image: `docker-compose build`
** Start the container: `docker-compose up --detach`
** Poll until the containers report status `running`: `docker ps --filter status=running --filter name=buildfarm`
* (Whenever you build): 
** Build the Bazel-compatible target with remote execution enabled: `/tmp/bazelisk build --verbose_failures --remote_executor=grpc://localhost:8980 src/mongo/db/commands:fsync_locked`
