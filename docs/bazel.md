(Note: This is a work-in-progress for the SDP team; contact #server-dev-platform for questions)

To perform a Bazel build via SCons:

-   You must be on a arm64 virtual workstation
-   You must generate engflow credentials and store them in the correct location (see below)
-   Build the Bazel-compatible target: `python3 ./buildscripts/scons.py BAZEL_BUILD_ENABLED=1 --build-profile=fast --ninja=disabled --link-model=static -j 200  --modules= build/fast/mongo/db/commands/libfsync_locked.a`

To generate and install the engflow credentials:

-   Navigate to and log in with your mongodb gmail account: https://sodalite.cluster.engflow.com/gettingstarted
-   Generate and download the credentials; you will need to move them to the workstation machine (scp, copy paste plain text, etc...)
-   Store them (the same filename they downloaded as) on your machine at the default location our build expects: `/engflow/creds/`
-   You should run `chmod 600` on them to make sure they are readable only by your user
-   If you don't want to use the cluster you can pass `BAZEL_FLAGS=--config=local` on the SCons command line or `--config=local` on the bazel command line

To perform a Bazel build and _bypass_ SCons:

-   Install Bazelisk: `curl -L https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-linux-arm64 --output /tmp/bazelisk && chmod +x /tmp/bazelisk`
-   Build the Bazel-compatible target: `/tmp/bazelisk build --verbose_failures  src/mongo/db/commands:fsync_locked`

To perform a Bazel build using a local Buildfarm (to test remote execution capability):

-   For more details on Buildfarm, see https://bazelbuild.github.io/bazel-buildfarm
-   (One time only) Build and start the Buildfarm:
    ** Change into the `buildfarm` directory: `cd buildfarm`
    ** Build the image: `docker-compose build`
    ** Start the container: `docker-compose up --detach`
    ** Poll until the containers report status `running`: `docker ps --filter status=running --filter name=buildfarm`
-   (Whenever you build):
    \*\* Build the Bazel-compatible target with remote execution enabled: `/tmp/bazelisk build --verbose_failures --remote_executor=grpc://localhost:8980 src/mongo/db/commands:fsync_locked`
