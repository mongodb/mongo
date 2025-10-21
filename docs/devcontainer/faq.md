# Dev Container FAQ

Frequently asked questions about MongoDB development with dev containers.

## General Questions

### What is a dev container?

A dev container (development container) is a Docker container configured specifically for development. It includes:

- All build tools and dependencies
- IDE configuration and extensions
- Persistent storage for caches and settings
- Consistent environment across all developers

Think of it as a portable, reproducible development environment that runs on any machine with Docker.

[Learn more about dev containers →](https://containers.dev/)

### Is this production-ready or still experimental?

The MongoDB devcontainer is currently in **Beta**. This means:

- Core functionality works well
- Most developers can use it for daily work
- Some edge cases may need refinement
- Active development and improvements ongoing

Report issues to help improve it for everyone!

## Setup and Installation

### Do I need SSH keys to use devcontainers?

**No, but SSH is recommended** for contributors who will be pushing code.

**You can use:**

- ✅ **SSH** (recommended): `git@github.com:10gen/mongo.git`
  - Pros: More secure, no password prompts, required for pushing code
  - Requires: SSH keys configured with GitHub
- ✅ **HTTPS**: `https://github.com/mongodb/mongo.git`
  - Pros: Works without SSH keys, simpler for read-only access
  - Cons: May require password/token for push operations

See the [Getting Started guide SSH setup section](./getting-started.md#4-configure-ssh-keys-recommended) for details.

### How do SSH keys work with devcontainers?

VS Code automatically forwards your SSH agent to the container, so you don't need to copy keys into the container.

**Requirements:**

1. SSH keys must be on your **host machine** (not in container)
2. SSH agent must be running with keys loaded
3. Keys should be in default location (`~/.ssh/`)

**Verify on host before opening container:**

```bash
# Check ssh-agent has your keys
ssh-add -l

# Test GitHub connection
ssh -T git@github.com
```

**Inside the container**, Git commands will automatically use your host's SSH keys through agent forwarding.

[Learn more about SSH agent forwarding →](https://code.visualstudio.com/remote/advancedcontainers/sharing-git-credentials)

### Why does git push ask for credentials in the container?

This usually means SSH agent forwarding isn't working.

**Fix:**

```bash
# On HOST machine (outside container):
# Ensure ssh-agent is running and has your key
ssh-add -l

# If empty, add your key
ssh-add ~/.ssh/id_ed25519  # or id_rsa

# Rebuild container to pick up agent forwarding
# Command Palette → "Dev Containers: Rebuild Container"
```

### Which Docker provider should I use?

**Recommended: Rancher Desktop**

- Free and open source
- Easy to configure
- Good performance
- Works well with devcontainers

**Alternatives:**

- Docker Desktop: Popular, user-friendly, requires license for large companies
- OrbStack (macOS): Lightweight, fast, newer
- Docker Engine (Linux): Direct, no GUI overhead

See [Getting Started](./getting-started.md#1-install-docker) for details.

### Why does the setup take so long?

First-time setup includes:

1. Downloading base image (~2 GB)
2. Building custom container (~5-10 min)
3. Downloading toolchain (~3 GB)
4. Installing Python dependencies (~5-10 min)
5. Building clangd index (~5 min)

**Total:** ~20-30 minutes depending on internet speed

**Subsequent rebuilds** are much faster due to Docker layer caching.

### Can I use this on Windows?

**Yes!** Requirements:

- Windows 10/11
- WSL2 installed and configured
- Docker Desktop with WSL2 integration enabled

**Important:** Clone repository in WSL2 filesystem (not `/mnt/c/`), not Windows filesystem, for best performance.

### Can I use this on Apple Silicon (M1/M2/M3)?

**Yes!** The devcontainer supports ARM64 architecture. Ensure:

- Docker provider supports ARM64 (Rancher Desktop, Docker Desktop do)
- Base image has ARM64 variant (current MongoDB image does)
- Rosetta 2 is enabled if needed (Rancher Desktop setting)

## Usage Questions

### Where is my code stored?

Your code lives in a **Docker volume**, not your local filesystem.

- **Inside container**: `/workspaces/mongo`
- **On host**: Managed by Docker (see `docker volume inspect <volume_name>`)

This is by design for performance, especially on macOS.

### How do I access files from my host OS?

**Option 1: Copy files out**

```bash
docker cp <container_id>:/workspaces/mongo/file.txt ~/Downloads/
```

**Option 2: Download from VS Code**

- Right-click file → Download...

**Option 3: Use bind mount** (sacrifices performance)

Open your existing local repository in VS Code and use "Dev Containers: Reopen in Container". This uses a bind mount which allows direct host filesystem access but is slower, especially on macOS.

### Can I use my existing local clone?

**Yes**, but not recommended for best performance.

**Option A: Reopen in container** (bind mount - slower)

1. Open your local repo in VS Code
2. Command Palette → "Dev Containers: Reopen in Container"

**Option B: Clone into volume** (recommended - faster)

1. Use "Clone Repository in Named Container Volume"
2. Delete local clone or keep for reference

### How do I switch between branches?

**Same as normal Git:**

```bash
git checkout main
git checkout -b feature/new-feature
git switch other-branch
```

Everything works the same; Git is inside the container.

### Can I run multiple containers simultaneously?

**Yes!** Clone the repository multiple times with different volume names:

1. Container 1: `mongo-main` (main branch)
2. Container 2: `mongo-feature` (feature branch)
3. Container 3: `mongo-review` (PR review)

Each runs independently with its own cache and environment.

### How do I update the devcontainer?

**To get latest changes:**

```bash
# Pull latest changes
git checkout main
git pull

# Rebuild container
# Command Palette → "Dev Containers: Rebuild Container"
```

This rebuilds the container with any updates to Dockerfile, toolchain, or features.

### Will my data persist after closing the container?

**Yes!** Data in volumes persists:

- ✅ Source code (in workspace volume)
- ✅ Bazel cache (in cache volume)
- ✅ Python venv (in venv volume)
- ✅ Shell history (in history volume)
- ✅ EngFlow credentials (in engflow_auth volume)

**What doesn't persist:**

- ❌ Processes (stopped on container stop)
- ❌ System packages installed with `apt-get` (unless in Dockerfile)
- ❌ Files in `/tmp`

### What happens if I delete the container?

**Volumes are preserved**, so you won't lose:

- Your source code
- Build caches
- Python environment
- Credentials

Just reopen the container and everything is back.

## Development Questions

### How do I build MongoDB?

See [MongoDB Building Guide](../../building.md).

### How do I debug code?

See [Advanced Usage - Debugging Workflow](./advanced.md#debugging-workflow)

### Why is my first build so slow?

First build downloads and compiles everything:

- Third-party dependencies
- MongoDB source
- Generates build files

**First build:** 30-60 minutes  
**Incremental builds:** 1-5 minutes

**To speed up:**

- Ensure cache volume is mounted
- Allocate more CPU/RAM to Docker

### How do I format code?

**Automatic (on save):**

- C/C++: clang-format runs automatically
- Python: Ruff runs automatically
- JavaScript: Prettier runs automatically

**Manual:**

```bash
# Format all files
bazel run format
```

### How do I use clangd for IntelliSense?

**It's automatic!** The devcontainer:

1. Builds `compile_commands.json` during setup
2. Configures VS Code to use clangd
3. Disables Microsoft C++ extension

**If not working:**

```bash
# Rebuild compile database
bazel build compiledb --config=local

# Restart clangd
# Command Palette → "clangd: Restart language server"
```

## Troubleshooting

### My build is very slow on macOS

**Check you're using a named volume:**

```bash
# Inside container
df -h /workspaces/mongo
```

If you see a mount from `/Users/...`, you're using a bind mount.

**Solution:** Clone in a named volume (see [Getting Started](./getting-started.md)).

### Python packages are missing

**Activate the virtual environment:**

```bash
source python3-venv/bin/activate

# Should show (python3-venv) in prompt
which python
# Should show: /workspaces/mongo/python3-venv/bin/python
```

**Reinstall packages:**

```bash
poetry install --no-root --sync
```

### VS Code extensions aren't working

**Reload window:**

- Command Palette → "Developer: Reload Window"

**Reinstall extension:**

- Extensions panel → Click extension → Uninstall → Install

**Check it's a container extension:**

- Some extensions only work on host, not in containers
- Look for "Install in Dev Container" button

### Bazel fails with toolchain errors

**Verify toolchain:**

```bash
ls -la /opt/mongodbtoolchain/revisions/
gcc --version  # Should show the MongoDB toolchain GCC version
```

**Rebuild container:**

- Command Palette → "Dev Containers: Rebuild Container"

### I can't connect to EngFlow

**Check credentials:**

```bash
ls -la ~/.config/engflow_auth/
```

**Re-authenticate:**
Contact MongoDB team for authentication flow.

**Build locally instead:**

```bash
bazel build --config=local install-mongod
```

### The container won't start

**Check Docker is running:**

```bash
docker info
docker ps
```

**View logs:**

- Command Palette → "Dev Containers: Show Container Log"
- Or: `docker logs <container_id>`

**Rebuild from scratch:**

- Command Palette → "Dev Containers: Rebuild Container Without Cache"

## Performance and Resources

### How much disk space do I need?

Allocate as much disk space as you can comfortably spare. We recommend at least 60GB

### How much RAM should I allocate to Docker?

**Allocate as much as possible** while leaving enough for your host OS to function (~4-8 GB).

More RAM = faster builds with more parallel jobs. MongoDB builds are resource-intensive and benefit greatly from additional memory.

### How many CPU cores should I allocate?

**Allocate as many cores as possible** while leaving a couple for your host OS (1-2 cores).

Bazel parallelizes well; more cores = significantly faster builds. If you have 8+ cores available, MongoDB builds will complete much faster.

### Can I reduce resource usage?

**Yes**, with trade-offs in build speed:

**Reduce Bazel parallelism:**

```bash
bazel build --jobs=N  # Replace N with fewer parallel jobs
```

**Limit memory:**

```bash
bazel build --local_ram_resources=HOST_RAM*0.5  # Use only 50% of available RAM
```

**Clear cache periodically:**

```bash
bazel clean  # Clear build outputs
bazel clean --expunge  # Clear everything (reclaim disk space)
```

> **Note:** Reducing resources will make builds slower. If possible, it's better to allocate more resources to Docker instead.

### How do I monitor resource usage?

```bash
docker stats  # Live resource usage
```

**Inside container:**

```bash
htop   # If installed
top    # Always available
df -h  # Disk usage
```

## Advanced Topics

### Can I customize the container?

**Yes!** See [Advanced Usage](./advanced.md) for:

- Using dotfiles in your containers
- Creating custom features
- Modifying VS Code settings

### Can I run the container without VS Code?

**Yes!** Use Docker directly:

```bash
# Build image
docker build -t mongo-dev -f .devcontainer/Dockerfile .

# Run container
docker run -it --rm \
  -v mongo-workspace:/workspaces/mongo \
  -v mongo-cache:/home/user/.cache \
  mongo-dev /bin/bash

# Now you're in the container
cd /workspaces/mongo
bazel build install-mongod
```

But you lose VS Code integration, extensions, and convenience features.

## Getting Help

### Where can I find more information?

- **Getting Started**: [getting-started.md](./getting-started.md)
- **Architecture Details**: [architecture.md](./architecture.md)
- **Troubleshooting**: [troubleshooting.md](./troubleshooting.md)
- **Advanced Topics**: [advanced.md](./advanced.md)
- **VS Code Docs**: [code.visualstudio.com/docs/devcontainers](https://code.visualstudio.com/docs/devcontainers/containers)

### Who do I contact for help?

1. **Documentation**: Check this guide first
2. **Search Issues**: Jira issues
3. **Internal Teams**: #server-local-dev channel in Slack (for employees)
4. **File Bug**: Create Jira issue with details
