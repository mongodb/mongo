# This file tells Docker how to build an image for both Buildfarm server and Builder shard-worker.
# Note that this file is referenced in docker-compose.yml for the aforementioned processes.

FROM ubuntu:22.04 AS toolchain
ENV USER="root"

# Install necessary tools/libraries
RUN apt-get update && apt-get install -y curl perl libxml2-dev libssl-dev git wget openjdk-19-jdk g++ gcc

# Pull in the Buildfarm repository
# Note: We are not verifying the commit hash, because the buildfarm solution is temporary and thus not worth it.
RUN git clone -b 2.3.1 https://github.com/bazelbuild/bazel-buildfarm.git

# Switch into the cloned Buildfarm repository
WORKDIR /bazel-buildfarm

# Obtain Bazelisk and make it executable
RUN wget https://github.com/bazelbuild/bazelisk/releases/download/v1.17.0/bazelisk-linux-arm64 -O bazelisk && chmod +x bazelisk

# Build the Buildfarm server and shard-worker in advance. (Note that this is not strictly necessary, since Bazel run will perform a build if necessary)
RUN ./bazelisk build //src/main/java/build/buildfarm:buildfarm-server //src/main/java/build/buildfarm:buildfarm-shard-worker

# Ensure that Buildform's configuration files are availabel at runtime:
COPY config.yml config.yml
COPY logging.properties logging.properties
