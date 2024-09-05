FROM ubuntu:22.04

# Expose a port range because the mongo binaries that are eventually started using this
# image will use the ports determined by resmoke. Resmoke determines the port to use
# for the mongo{d,s} processes by starting at 20000 and incrementing the port number
# for every new mongo{d,s} process.
EXPOSE 20000-20100

# prep the environment
RUN mkdir -p /data/db
RUN mkdir -p /data/configdb
RUN mkdir -p /var/log/mongodb
RUN mkdir -p /scripts

# Install dependencies of MongoDB Server
RUN apt-get update
RUN apt-get install -y curl python3

# Ensure the symbolizer is present (llvm)
ARG LLVM_VERSION=12
RUN apt-get install -y llvm-${LLVM_VERSION}

# Copy TSAN suppressions
COPY tsan.suppressions /etc/tsan.suppressions

# Sanitizer options from build args. We append certain extra options like the symbolizer path,
# which is dependent on the image's LLVM version.
ARG ASAN_OPTIONS
ARG EXTENDED_ASAN_OPTIONS="${ASAN_OPTIONS}:external_symbolizer_path=/usr/lib/llvm-${LLVM_VERSION}/bin/llvm-symbolizer"
ENV ASAN_OPTIONS="${ASAN_OPTIONS:+${EXTENDED_ASAN_OPTIONS}}"

ARG UBSAN_OPTIONS
ARG EXTENDED_UBSAN_OPTIONS="${UBSAN_OPTIONS}:external_symbolizer_path=/usr/lib/llvm-${LLVM_VERSION}/bin/llvm-symbolizer"
ENV UBSAN_OPTIONS="${UBSAN_OPTIONS:+${EXTENDED_UBSAN_OPTIONS}}"

# Also append suppressions path for TSAN
ARG TSAN_OPTIONS
ARG EXTENDED_TSAN_OPTIONS="${TSAN_OPTIONS}:external_symbolizer_path=/usr/lib/llvm-${LLVM_VERSION}/bin/llvm-symbolizer:suppressions=/etc/tsan.suppressions"
ENV TSAN_OPTIONS="${TSAN_OPTIONS:+${EXTENDED_TSAN_OPTIONS}}"

# -------------------
# Everything above this line should be common image setup
# Everything below should be specific to the version of mongodb being installed

COPY bin/* /usr/bin/
RUN chmod +x /usr/bin/*
COPY lib/* /usr/lib/
COPY libvoidstar.so /usr/lib/libvoidstar.so

# copy mongo repo
COPY src /mongo
WORKDIR /mongo
RUN chmod 500 src/mongo/db/modules/enterprise/jstests/encryptdb/libs/ekf2

RUN /usr/bin/mongo --version
