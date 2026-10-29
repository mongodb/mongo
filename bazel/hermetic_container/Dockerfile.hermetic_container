FROM debian:buster-slim

ENV BAZEL_VERSION 3.7.1

# Creating the man pages directory to deal with the slim variants not having it.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
 && echo "deb [arch=amd64] http://storage.googleapis.com/bazel-apt stable jdk1.8" > \
         /etc/apt/sources.list.d/bazel.list \
 && curl https://bazel.build/bazel-release.pub.gpg | apt-key add - \
 && apt-get update && apt-get install -y --no-install-recommends bazel=${BAZEL_VERSION} \
 && apt-get purge --auto-remove -y curl gnupg \
 && rm -rf /etc/apt/sources.list.d/bazel.list \
 && rm -rf /var/lib/apt/lists/*
