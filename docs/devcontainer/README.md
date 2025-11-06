# MongoDB Development with Dev Containers

**⚠️ BETA:** The devcontainer setup is currently in Beta stage. Please report issues and feedback to the team.

## 📚 Documentation Index

This is the comprehensive guide for developing MongoDB using Dev Containers. Choose the guide that best fits your needs:

### 🚀 [Getting Started](./getting-started.md)

**Start here if you're new to devcontainers or MongoDB development**

- Prerequisites and system requirements
- Step-by-step setup instructions
- First-time configuration
- Verifying your setup

### 🎨 [Customization](./customization.md)

**Personal customizations without modifying the devcontainer setup**

- Persistent dotfiles
- Always-installed features

### 🏗️ [Architecture & Technical Details](./architecture.md)

**Understand how the devcontainer works under the hood**

- Container architecture overview
- Dockerfile breakdown
- Volume management and persistence
- Toolchain installation process
- VS Code integration details
- Feature system explained

### 🔧 [Troubleshooting](./troubleshooting.md)

**Having issues? Check here for solutions**

- Common problems and fixes
- Performance optimization
- Platform-specific issues (macOS, Windows, Linux)
- Docker provider comparison
- Debugging tips

### 💡 [Advanced Usage](./advanced.md)

**Power user workflows and complex scenarios**

- Working with multiple containers
- Backup and migration strategies
- Development workflows and debugging
- EngFlow telemetry integration

### ❓ [FAQ](./faq.md)

**Quick answers to common questions**

- Why use devcontainers?
- Named volumes vs bind mounts
- Updating the container
- Data persistence
- And more...

## Quick Links

### VS Code Documentation

- [Dev Containers Overview](https://code.visualstudio.com/docs/devcontainers/containers)
- [devcontainer.json Reference](https://code.visualstudio.com/docs/devcontainers/containers#_devcontainerjson-reference)
- [Create a Dev Container](https://code.visualstudio.com/docs/devcontainers/create-dev-container)
- [Advanced Container Configuration](https://code.visualstudio.com/docs/devcontainers/containers#_advanced-container-configuration)
- [Dev Containers FAQ](https://code.visualstudio.com/docs/devcontainers/faq)
- [Dev Containers Tips and Tricks](https://code.visualstudio.com/docs/devcontainers/tips-and-tricks)

### MongoDB Documentation

- [MongoDB Build Documentation](../../building.md)
- [MongoDB Contributing Guide](../../CONTRIBUTING.rst)

## What are Dev Containers?

Dev Containers provide a consistent, reproducible development environment using Docker containers. This ensures:

- ✅ **Consistency**: Everyone works with identical tooling and dependencies
- ✅ **Isolation**: Your host system stays clean
- ✅ **Portability**: Develop from any machine with Docker
- ✅ **Quick Setup**: Get started in minutes, not hours

## Benefits for MongoDB Development

The MongoDB devcontainer provides:

1. **Pre-configured Build Environment**: All build tools, compilers, and dependencies ready to use
2. **MongoDB Toolchain**: Specific GCC/Clang versions required for building MongoDB
3. **IDE Integration**: VS Code settings optimized for C++, Python, JavaScript, and Bazel
4. **Persistent Caching**: Build artifacts and Python environments preserved across sessions
5. **EngFlow Support**: Built-in support for remote execution and caching

## System Requirements

- **Docker**: Allocate as much RAM as possible to Docker (leave ~4-8 GB for host OS)
- **CPU**: Allocate as many cores as possible to Docker (leave 1-2 for host OS)
- **Disk Space**: 60+ GB recommended for container, tools, and build artifacts
- **VS Code**: Latest version with Remote - Containers extension
- **Operating System**:
  - macOS (ARM64 or x86_64)
  - Windows 10/11 with WSL2
  - Linux (x86_64 or ARM64)

## Getting Help

- **Documentation Issues**: Open an issue or PR in the MongoDB repository
- **Devcontainer Problems**: Check [Troubleshooting Guide](./troubleshooting.md)
- **General Questions**: See [FAQ](./faq.md)

---

**Ready to get started?** → [Follow the Getting Started Guide](./getting-started.md)
