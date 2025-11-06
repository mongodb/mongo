# Advanced Dev Container Usage

This guide covers advanced workflows and power user features for managing multiple containers, backups, and complex development scenarios.

**Looking to customize your devcontainer?** See the [Customization Guide](./customization.md) for dotfiles, VS Code settings, extensions, and performance tuning.

## Table of Contents

- [Working with Multiple Containers](#working-with-multiple-containers)
- [Backup and Migration](#backup-and-migration)
- [Development Workflows](#development-workflows)

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

- [Customization](./customization.md) - Personalize your devcontainer
- [Architecture](./architecture.md) - How it all works
- [Troubleshooting](./troubleshooting.md) - Fix issues
- [FAQ](./faq.md) - Common questions
