ARG BASE_IMAGE=quay.io/mongodb/bazel-remote-execution:ubuntu24-2025_09_05-17_18_29
FROM $BASE_IMAGE
ARG BASE_IMAGE

ARG USERNAME=mongo-dev
ARG USER_UID=1000
ARG USER_GID=$USER_UID

# Create the user
RUN groupadd $USERNAME && useradd -s /bin/bash --gid $USER_GID -m $USERNAME

RUN apt-get update && apt-get install -y sudo curl

# Give user sudo access
RUN echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/devcontaineruser && chmod 0440 /etc/sudoers.d/devcontaineruser

# Persistent bash history
RUN SNIPPET="export PROMPT_COMMAND='history -a' && export HISTFILE=/commandhistory/.bash_history" \
  && mkdir /commandhistory \
  && touch /commandhistory/.bash_history \
  && chown -R $USERNAME /commandhistory \
  && echo "$SNIPPET" >> "/home/$USERNAME/.bashrc"

# Toolchain installation
RUN curl -o /toolchain_installer.sh http://mongodbtoolchain.build.10gen.cc/installer.sh && chmod a+x /toolchain_installer.sh 
USER $USERNAME
ENV USER ${USERNAME}
RUN /toolchain_installer.sh

# Bazel telemetry
RUN echo "common --bes_keywords=devcontainer:use=true" >> "$HOME/.bazelrc" && \
  echo "common --bes_keywords=devcontainer:image=$BASE_IMAGE" >> "$HOME/.bazelrc" && \
  echo "common --bes_keywords=devcontainer:username=$USERNAME" >> "$HOME/.bazelrc"
