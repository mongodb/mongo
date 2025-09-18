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

### 1. Clone the MongoDB Repository

```bash
git clone https://github.com/mongodb/mongo.git
cd mongo
```

### 2. Install the Remote Containers VSCode Extension

Install the [Dev Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) extension from the VSCode marketplace.

### 3. Open the clone in VSCode

You may be automatically prompted to open the devcontainer and can confirm. If this does not happen, open the VSCode command palette and enter ">Dev Containers: Reopen in Container".

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
