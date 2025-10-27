# syntax=docker/dockerfile:1
ARG BASE_IMAGE=quay.io/mongodb/bazel-remote-execution:ubuntu24-2025_09_05-17_18_29
FROM $BASE_IMAGE
ARG BASE_IMAGE

ARG USERNAME=mongo-dev
ARG USER_UID=1000
ARG USER_GID=$USER_UID

# Create the user
RUN groupadd $USERNAME && useradd -s /bin/bash --gid $USER_GID -m $USERNAME

RUN apt-get update && apt-get install -y sudo curl ca-certificates xdg-utils

# Install xdg-open wrapper for browser integration
COPY .devcontainer/xdg-open-wrapper.sh /usr/local/bin/xdg-open-wrapper.sh
RUN chmod +x /usr/local/bin/xdg-open-wrapper.sh && \
  if [ -f /usr/bin/xdg-open ]; then \
    mv /usr/bin/xdg-open /usr/bin/xdg-open.real; \
  fi && \
  ln -s /usr/local/bin/xdg-open-wrapper.sh /usr/bin/xdg-open

# Give user sudo access
RUN echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/devcontaineruser && chmod 0440 /etc/sudoers.d/devcontaineruser

# Persistent bash history
RUN SNIPPET="export PROMPT_COMMAND='history -a' && export HISTFILE=/commandhistory/.bash_history" \
  && mkdir /commandhistory \
  && touch /commandhistory/.bash_history \
  && chown -R $USERNAME /commandhistory \
  && echo "$SNIPPET" >> "/home/$USERNAME/.bashrc"

# Toolchain installation with SHA256 verification
# Run "python3 toolchain.py generate" to update toolchain_config.env
ARG TARGETPLATFORM
COPY .devcontainer/toolchain_config.env /tmp/toolchain_config.env
RUN set -e; \
  . /tmp/toolchain_config.env; \
  if [ "$TARGETPLATFORM" = "linux/arm64" ]; then \
    TOOLCHAIN_URL="$TOOLCHAIN_ARM64_URL"; \
    TOOLCHAIN_SHA256="$TOOLCHAIN_ARM64_SHA256"; \
    ARCH="arm64"; \
  elif [ "$TARGETPLATFORM" = "linux/amd64" ]; then \
    TOOLCHAIN_URL="$TOOLCHAIN_AMD64_URL"; \
    TOOLCHAIN_SHA256="$TOOLCHAIN_AMD64_SHA256"; \
    ARCH="amd64"; \
  else \
    echo "Unsupported platform: $TARGETPLATFORM"; \
    exit 1; \
  fi; \
  echo "Target platform: $TARGETPLATFORM"; \
  echo "Architecture: $ARCH"; \
  echo "Installing toolchain from: $TOOLCHAIN_URL"; \
  echo "Expected SHA256: $TOOLCHAIN_SHA256"; \
  curl -fSL "$TOOLCHAIN_URL" -o /tmp/toolchain.tar.gz; \
  echo "Verifying checksum..."; \
  echo "$TOOLCHAIN_SHA256  /tmp/toolchain.tar.gz" | sha256sum -c -;
RUN echo "Extracting toolchain..."; \
  mkdir -p /opt/mongodbtoolchain/revisions && tar -xzf /tmp/toolchain.tar.gz -C /opt/mongodbtoolchain/revisions; \
  rm /tmp/toolchain.tar.gz; \
  chown -R ${USERNAME} /opt/mongodbtoolchain;

USER $USERNAME
ENV USER=${USERNAME}
RUN /opt/mongodbtoolchain/revisions/*/scripts/install.sh; echo "Toolchain installation complete"

# Add MongoDB toolchain to PATH
ENV PATH="/opt/mongodbtoolchain/v5/bin:${PATH}"

# Bazel telemetry
RUN echo "common --bes_keywords=devcontainer:use=true" >> "$HOME/.bazelrc" && \
  echo "common --bes_keywords=devcontainer:image=$BASE_IMAGE" >> "$HOME/.bazelrc" && \
  echo "common --bes_keywords=devcontainer:username=$USERNAME" >> "$HOME/.bazelrc"
