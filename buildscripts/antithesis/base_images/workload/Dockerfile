FROM ubuntu:22.04

EXPOSE 27017
EXPOSE 27018
EXPOSE 27019

RUN mkdir -p /scripts
RUN mkdir -p /var/log/mongodb

RUN apt-get update

# Install mongo dependencies
RUN apt-get install -y curl

# Install git
RUN apt-get install -y git

# Set up Python
RUN apt-get install -y python3.10 python3-pip python3.10-venv
RUN ln -f $(which python3.10) /usr/bin/python
RUN ln -f $(which python3.10) /usr/bin/python3

RUN python -m pip install --upgrade pip wheel

# Ensure the symbolizer is present (llvm)
ARG LLVM_VERSION=12
RUN apt-get install -y llvm-${LLVM_VERSION}

# Ensure that libc debugsymbols are on the machine
RUN apt-get install -y libc6-dbg

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

# copy the mongo binary -- make sure it is executable
COPY bin/mongo /usr/bin
RUN chmod +x /usr/bin/mongo

# Copy libraries
COPY lib/* /usr/lib/

COPY libvoidstar.so /usr/lib/libvoidstar.so

RUN /usr/bin/mongo --version

# Copy the QA directory
COPY QA /QA

# Initialize the jstestfuzz repository
WORKDIR /jstestfuzz
COPY jstestfuzz /jstestfuzz

# Install jstestfuzz deps
RUN ./src/scripts/npm_run.sh jstestfuzz -- --help

# Initialize the MongoDB repository
WORKDIR /mongo

# Copy mongodb python deps first
COPY src/pyproject.toml src/poetry.lock src/poetry_requirements.txt ./

# Install mongodb python deps with poetry
RUN python -m pip install -r poetry_requirements.txt
RUN python -m poetry install --no-root --sync

# Add the poetry venv to the $PATH so that it's activated by default
# (We use a symlink because the path to the poetry venv is unknown & generated at runtime)
RUN ln -s $(dirname $(dirname $(poetry run which python))) /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# copy mongo
COPY src /mongo
WORKDIR /mongo

# create symlinks to complete setup
RUN ln -s /jstestfuzz /mongo/jstestfuzz
RUN ln -s /QA /mongo/jstests/qa_tests
RUN ln -s /mongo/src/mongo/db/modules/enterprise /mongo/jstests/enterprise_tests
