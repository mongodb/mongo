# Advanced Dev Container Usage

This guide covers advanced topics for power users who want to customize and extend their devcontainer setup.

## Table of Contents

- [Customizing the Container](#customizing-the-container)
- [Working with Multiple Containers](#working-with-multiple-containers)
- [Adding Custom Features](#adding-custom-features)
- [VS Code Customization](#vs-code-customization)
- [Performance Tuning](#performance-tuning)
- [Backup and Migration](#backup-and-migration)
- [Development Workflows](#development-workflows)

## Customizing the Container

### Persistent Dotfiles

[Learn more about personalizing with dotfiles →](https://code.visualstudio.com/docs/devcontainers/containers#_personalizing-with-dotfile-repositories)

### Always Installed Features

[Learn more about adding a set of personalized features](https://code.visualstudio.com/docs/devcontainers/containers#_always-installed-features)

## Working with Multiple Containers

### Running Multiple Instances

You can run multiple devcontainers for different branches:

```bash
# Clone same repo with different volume names
Dev Containers: Clone Repository in Named Container Volume...
# Volume 1: mongo-main
# Volume 2: mongo-feature-branch
# Volume 3: mongo-bugfix
```

Each gets its own:

- Container instance
- Cache volume
- Python venv

**Switch between them:**

- VS Code → File → Recent
- Select the container you want

### EngFlow Telemetry

The devcontainer automatically reports metadata to EngFlow via Bazel keywords:

```bash
# These are added automatically in Dockerfile and postCreateCommand
common --bes_keywords=devcontainer:use=true
common --bes_keywords=devcontainer:image=<image_tag>
common --bes_keywords=devcontainer:docker_server_platform=<platform>
```

This helps the team understand devcontainer adoption and troubleshoot issues.

## Adding Custom Features

### Creating a Custom Feature

Features are modular additions to your devcontainer. Create one:

```bash
mkdir -p .devcontainer/features/my-feature
cd .devcontainer/features/my-feature
```

**Create `devcontainer-feature.json`:**

```json
{
  "id": "my-feature",
  "version": "1.0.0",
  "name": "My Custom Feature",
  "description": "Adds my custom tools and configuration",
  "options": {
    "version": {
      "type": "string",
      "default": "latest",
      "description": "Version to install"
    }
  }
}
```

**Create `install.sh`:**

```bash
#!/usr/bin/env bash
set -e

VERSION="${VERSION:-latest}"

echo "Installing my-feature version $VERSION..."

# Your installation logic here
apt-get update
apt-get install -y my-package

echo "my-feature installation complete!"
```

**Note**: Custom features are typically contributed to the main devcontainer configuration through pull requests rather than added individually.

### Example: MongoDB Compass Feature

```bash
# .devcontainer/features/compass/install.sh
#!/usr/bin/env bash
set -e

echo "Installing MongoDB Compass..."

wget https://downloads.mongodb.com/compass/mongodb-compass_latest_amd64.deb
sudo dpkg -i mongodb-compass_latest_amd64.deb || true
sudo apt-get install -f -y
rm mongodb-compass_latest_amd64.deb

echo "Compass installed!"
```

### Example: Database Tools Feature

```bash
# .devcontainer/features/db-tools/install.sh
#!/usr/bin/env bash
set -e

echo "Installing MongoDB Database Tools..."

wget https://fastdl.mongodb.org/tools/db/mongodb-database-tools-ubuntu2204-x86_64-100.9.4.deb
sudo dpkg -i mongodb-database-tools-*.deb
rm mongodb-database-tools-*.deb

echo "Database tools installed: mongodump, mongorestore, etc."
```

## VS Code Customization

### User-Specific Settings

Override devcontainer settings in user `settings.json`.

[Learn more about container-specific settings →](https://code.visualstudio.com/docs/devcontainers/containers#_container-specific-settings)

```json
{
  // Your personal preferences
  "editor.fontSize": 14,
  "editor.tabSize": 2,
  "terminal.integrated.fontSize": 13,

  // Override theme
  "workbench.colorTheme": "Monokai",

  // Additional formatters
  "editor.defaultFormatter": "esbenp.prettier-vscode"
}
```

### Additional Extensions

Install extra extensions without modifying `devcontainer.json`:

```bash
# Via command line
code --install-extension ms-vscode.hexeditor

# Or manually in Extensions panel
```

### Port Forwarding

VS Code automatically forwards ports from the container to your host machine. When a service listens on a port inside the container, VS Code detects it and makes it accessible from your browser.

```bash
# Start mongod on default port 27017
./bazel-bin/src/mongo/mongod --dbpath /data/db

# VS Code will automatically forward port 27017
# Access from host: localhost:27017
```

**Manual port forwarding:**

- Click on the Ports tab in VS Code terminal panel
- Add port → Enter port number
- Access forwarded ports from your host browser or tools
- **Note**: Some firewall configurations may block forwarded ports

[Learn more about port forwarding →](https://code.visualstudio.com/docs/devcontainers/containers#_forwarding-or-publishing-a-port)

## Performance Tuning

### Docker Performance

**macOS:**

- Use VirtioFS instead of osxfs (Docker Desktop 4.6+)
- Settings → Experimental → VirtioFS

**All platforms:**

- Allocate as many resources as possible to Docker (see [Getting Started](./getting-started.md))
- Use named volumes (not bind mounts)
- Enable BuildKit (for faster image builds):
  ```bash
  export DOCKER_BUILDKIT=1
  ```

### File Watching

Reduce file watcher overhead:

```json
// Add to VS Code settings.json
{
  "files.watcherExclude": {
    "**/bazel-*/**": true,
    "**/node_modules/**": true,
    "**/.cache/**": true,
    "**/python3-venv/**": true
  }
}
```

## Backup and Migration

### Backing Up Volumes

```bash
# Backup a volume to tarball
docker run --rm \
  -v engflow_auth:/data \
  -v $(pwd):/backup \
  ubuntu tar czf /backup/engflow_auth_backup.tar.gz -C /data .

# Backup all MongoDB dev volumes
for vol in engflow_auth mongo-cache mongo-python3-venv; do
  docker run --rm \
    -v $vol:/data \
    -v $(pwd):/backup \
    ubuntu tar czf /backup/${vol}_backup.tar.gz -C /data .
done
```

### Restoring Volumes

```bash
# Create volume
docker volume create engflow_auth

# Restore from backup
docker run --rm \
  -v engflow_auth:/data \
  -v $(pwd):/backup \
  ubuntu tar xzf /backup/engflow_auth_backup.tar.gz -C /data
```

### Migrating to New Machine

**Option 1: Export and Import Volumes**

On old machine:

```bash
# Backup volumes (see above)
# Copy .tar.gz files to new machine
```

On new machine:

```bash
# Restore volumes (see above)
# Clone repository and open devcontainer
```

**Option 2: Use Docker Save/Load**

```bash
# Old machine: Save container image
docker save -o mongo-devcontainer.tar <image_id>

# New machine: Load image
docker load -i mongo-devcontainer.tar
```

## Development Workflows

### Debugging Workflow

**With GDB:**

```bash
# Build with debug symbols
bazel build --config=dbg install-mongod

# Run with GDB
gdb bazel-bin/install-mongod/bin/mongod
(gdb) run --dbpath /data/db
(gdb) break my_function
(gdb) continue
```

## Tips and Tricks

### Quick Commands

```bash
# Rebuild devcontainer from terminal
# Cmd/Ctrl+Shift+P → "Dev Containers: Rebuild Container"

# Attach to running container
docker exec -it <container_name> /bin/bash

# Copy compile_commands.json to host (for external IDE)
docker cp <container_id>:/workspaces/mongo/compile_commands.json ~/Desktop/

# Check what's using disk space
du -sh ~/.cache/*
du -sh /opt/mongodbtoolchain/*
```

---

**See Also:**

- [Architecture](./architecture.md) - How it all works
- [Troubleshooting](./troubleshooting.md) - Fix issues
- [FAQ](./faq.md) - Common questions
