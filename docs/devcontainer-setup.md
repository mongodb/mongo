# MongoDB Development with Devcontainers

**IMPORTANT** The devcontainer setup is currently in the Beta stage

This guide walks you through setting up a MongoDB development environment using devcontainers as the underlying container orchestration tool.

## Prerequisites

### Install Docker

Devcontainers requires Docker to be installed and running on your system, some examples of docker providers are:

- [Rancher Desktop](https://rancherdesktop.io/) (Recommended)
- [Docker Desktop](https://www.docker.com/products/docker-desktop/)
- [Orbstack](https://orbstack.dev/)
- [Docker Engine](https://docs.docker.com/engine/install/) (Linux only)

#### Rancher Desktop

If you choose to use Rancher Desktop, you will be prompted to fill in some settings when you first launch the application. You can use the following settings:

- Kubernetes: Choose whatever you like, this isn't required for devcontainers
- Container Engine: `dockerd (moby)`
- Configure Path: "Automatic"

Afterwards, it is recommended to increase the amount of CPU and Memory available to the container engine. You can do this by going to Preferences > Virtual Machine.

## Setup Instructions

### 1. Install the Remote Containers VSCode Extension

Install the [Remote Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) extension from the VSCode marketplace.

### 2. Clone the MongoDB Repository in a Named Container Volume

For best performance and compatibility (especially with Bazel on macOS), **do not clone the repository to your local filesystem first**. Instead, open VSCode and use the VSCode command palette:

1. Open the command palette (`Ctrl+Shift+P` or `Cmd+Shift+P` on Mac).
2. Run **"Dev Containers: Clone Repository in Named Container Volume..."**
3. Enter the repository URL (it is recommended you use the ssh url)
4. Use whatever name you like for the named volume, this name will be presented in the docker volume list if you need to reference it later.
5. VS Code will automatically set up the devcontainer in an isolated volume, improving performance and resolving compatibility issues with Bazel on macOS filesystems over using a bind-mounted workspace.

If you have already cloned the repository locally, you can still use the command above for a better experience.

### 3. Fetch Complete Git History (Recommended)

After the container opens for the first time, it's recommended to run the following command in the terminal to fetch the complete git history since the above clone workflow may only do a shallow clone:

```bash
git fetch --unshallow --all
```

This ensures you have access to the full repository history and all branches, which may be needed for certain development workflows (e.g., `bazel build install-dist`).

### 4. Access Your Development Environment

Once setup is complete, VS Code will automatically open with your containerized development environment. You'll have access to:

- **MongoDB source code** mounted in the container
- **Persistent volumes** for caches and configurations
- **Pre-configured VS Code settings** for MongoDB development
- **All development tools** ready to use

## Container Features

### Persistent Storage

The devcontainer uses several persistent volumes to maintain state across container restarts:

- **engflow_auth**: Authentication credentials for EngFlow remote execution
- **python3-venv**: Python virtual environment and dependencies
- **cache**: Build caches and other temporary files

### VS Code Integration

The container includes pre-configured VS Code settings for:

- **C/C++ development** with clangd and clang-format
- **Python development** with ruff and mypy
- **JavaScript development** with ESLint and Prettier
- **Bazel integration** for build system support
- **MongoDB-specific** file associations and schemas

## Additional Resources

- [VS Code Devcontainer Documentation](https://code.visualstudio.com/docs/devcontainers/containers)
