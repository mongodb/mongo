FROM ubuntu:22.04

EXPOSE 27017
EXPOSE 27018
EXPOSE 27019

RUN mkdir -p /scripts
RUN mkdir -p /var/log/mongodb

# Copy retry script for handling transient apt failures
COPY retry_apt.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/retry_apt.sh

# Install dependencies with retries (including llvm and libc6-dbg)
# Retry up to 6 times with exponential backoff if mirrors are syncing
ARG LLVM_VERSION=12
RUN retry_apt.sh "APT package installation" sh -c "\
    apt-get clean && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        curl \
        git \
        software-properties-common \
        llvm-${LLVM_VERSION} \
        libc6-dbg"

# Set up Python 3.13 via deadsnakes PPA (project requires >=3.13)
# Retry entire setup (PPA addition with GPG key import + package install)
RUN retry_apt.sh "Python 3.13 installation" sh -c "\
    add-apt-repository ppa:deadsnakes/ppa && \
    apt-get clean && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        python3.13 \
        python3.13-venv \
        python3.13-dev \
        build-essential" && \
    python3.13 -m ensurepip --upgrade
RUN ln -f $(which python3.13) /usr/bin/python
RUN ln -f $(which python3.13) /usr/bin/python3

RUN retry_apt.sh "pip/wheel upgrade" python -m pip install --upgrade pip wheel

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

# Copy mongodb python deps first (uv_version.txt is the shared version
# pin — same file that venv_setup.sh, uv_sync.sh, and the devcontainer
# Dockerfile all read from).
COPY src/pyproject.toml src/uv.lock src/buildscripts/uv_version.txt ./

# Install mongodb python deps with uv, pinned via uv_version.txt.
RUN retry_apt.sh "uv installation" sh -c 'python -m pip install "uv==$(cat uv_version.txt)"'
# UV_PROJECT_ENVIRONMENT points uv at /opt/venv as the install target so the
# resulting venv has a stable, well-known path independent of uv's default
# `.venv` layout.
RUN retry_apt.sh "python dependency sync" sh -c "\
    python -m venv /opt/venv && \
    UV_PROJECT_ENVIRONMENT=/opt/venv python -m uv sync --locked --all-groups --no-install-project"

# Clean up retry script after use
RUN rm /usr/local/bin/retry_apt.sh
ENV PATH="/opt/venv/bin:$PATH"

# copy mongo
COPY src /mongo
WORKDIR /mongo

# create symlinks to complete setup
RUN ln -s /jstestfuzz /mongo/jstestfuzz
RUN ln -s /QA /mongo/jstests/qa_tests
RUN ln -s /mongo/src/mongo/db/modules/enterprise /mongo/jstests/enterprise_tests
