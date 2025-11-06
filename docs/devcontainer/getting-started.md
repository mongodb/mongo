# Getting Started with MongoDB Dev Containers

This guide will walk you through setting up your MongoDB development environment using Dev Containers.

## Prerequisites

### 1. Install Docker

Dev Containers require Docker to be installed and running on your system. Choose one of the following Docker providers:

#### Option A: Rancher Desktop (Recommended)

[Rancher Desktop](https://rancherdesktop.io/) is our recommended Docker provider for devcontainer development.

**Installation:**

1. Download and install Rancher Desktop from [rancherdesktop.io](https://rancherdesktop.io/)
2. On first launch, configure these settings:
   - **Kubernetes**: Choose any option (not required for devcontainers)
   - **Container Engine**: Select `dockerd (moby)` ⚠️ **Important!**
   - **Configure Path**: Select "Automatic"

**Recommended Settings:**
After installation, increase resources for better build performance:

1. Open Rancher Desktop → Preferences → Virtual Machine
2. **Memory**: Allocate as much as your system allows (leave ~4-8 GB for your host OS)
3. **CPUs**: Allocate as many cores as possible (leave 1-2 for your host OS)
4. **Disk**: Rancher Desktop doesn't have a UI for disk size. To increase it, see [Troubleshooting - Increase Docker disk allocation](./troubleshooting.md#build-fails-with-no-space-left-on-device) for instructions.
5. Apply changes and restart Rancher Desktop

> **Tip:** More resources = faster builds. MongoDB builds benefit significantly from additional CPU cores and memory.

**IMPORTANT!**: If you already have VSCode open when you install Rancher Desktop, make sure to restart VSCode otherwise it may not find the Docker socket and VSCode will prompt you to install Docker Desktop instead.

#### Option B: Docker Desktop

[Docker Desktop](https://www.docker.com/products/docker-desktop/) is a popular alternative.

> **Note on Licensing**: Docker Desktop may require a paid license for commercial use. Please review the licensing terms to ensure compliance with your use case.

**Installation:**

1. Download from [docker.com/products/docker-desktop](https://www.docker.com/products/docker-desktop/)
2. Install and start Docker Desktop
3. Go to Settings → Resources and allocate generously:
   - **Memory**: Allocate as much as possible (leave ~4-8 GB for your host OS)
   - **CPUs**: Allocate as many cores as possible (leave 1-2 for your host OS)
   - **Disk**: Ensure plenty of space available (60+ GB recommended)

#### Option C: OrbStack (macOS)

[OrbStack](https://orbstack.dev/) is a lightweight, fast Docker alternative for macOS.

> **Note on Licensing**: OrbStack may require a paid license for commercial use. Please review the licensing terms to ensure compliance with your use case.

**Installation:**

1. Download from [orbstack.dev](https://orbstack.dev/)
2. Install and launch OrbStack
3. OrbStack automatically manages resources efficiently

#### Option D: Docker Engine (Linux only)

For Linux users, you can use Docker Engine directly.

**Installation:**
Follow the official guide: [docs.docker.com/engine/install](https://docs.docker.com/engine/install/)

### 2. Create SSH Directory (Required)

> **⚠️ Critical:** You **must** have a `~/.ssh` directory on your host machine before building the devcontainer. The devcontainer requires this directory to exist, regardless of whether you use SSH or HTTPS to clone the repository.

```bash
# On your HOST machine (not inside the container)
mkdir -p ~/.ssh
```

If you skip this step, you'll encounter bind mount errors when trying to start the devcontainer.

### 3. Install Visual Studio Code

Download and install VS Code from [code.visualstudio.com](https://code.visualstudio.com/)

### 4. Install Dev Containers Extension

1. Open VS Code
2. Go to Extensions (⌘/Ctrl+Shift+X)
3. Search for "Dev Containers"
4. Install the [Dev Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) extension by Microsoft

### 5. Configure SSH Keys (Recommended)

To clone the repository using SSH (recommended for contributors), you'll need SSH keys configured with GitHub.

> **⚠️ Important:** Run all commands in this section on your **host machine** (not inside the container). SSH keys need to be set up before cloning the repository into the container.

#### Check if you have SSH keys

```bash
# Check for existing SSH keys
ls -la ~/.ssh/id_*.pub

# If you see id_rsa.pub, id_ed25519.pub, or similar, you have keys
```

#### Generate SSH keys (if needed)

If you don't have SSH keys, generate them:

```bash
# Generate a new ED25519 key (recommended)
ssh-keygen -t ed25519 -C "your_email@example.com"

# Or generate RSA key (if ED25519 not supported)
ssh-keygen -t rsa -b 4096 -C "your_email@example.com"

# Press Enter to accept default location
# Enter a passphrase (recommended) or press Enter for no passphrase
```

#### Add SSH key to GitHub

1. **Copy your public key:**

   ```bash
   # For ED25519
   cat ~/.ssh/id_ed25519.pub

   # For RSA
   cat ~/.ssh/id_rsa.pub
   ```

2. **Add to GitHub:**

   - Go to [GitHub SSH Settings](https://github.com/settings/keys)
   - Click "New SSH key"
   - Paste your public key
   - Give it a descriptive title (e.g., "Work Laptop")
   - Click "Add SSH key"

3. **Test your connection:**
   ```bash
   ssh -T git@github.com
   # Should see: "Hi username! You've successfully authenticated..."
   ```

#### SSH Agent (for keys with passphrases)

If your SSH key has a passphrase, add it to the SSH agent:

```bash
# Start ssh-agent (macOS/Linux)
eval "$(ssh-agent -s)"

# Add your key to the agent
ssh-add ~/.ssh/id_ed25519  # or id_rsa

# Verify key is loaded
ssh-add -l
```

**macOS users:** Add this to `~/.ssh/config` to automatically load keys:

```
Host *
  AddKeysToAgent yes
  UseKeychain yes
  IdentityFile ~/.ssh/id_ed25519
```

For passphrase-protected keys, also add to `~/.zshrc` or `~/.bashrc`:

```bash
ssh-add --apple-use-keychain ~/.ssh/id_ed25519 2>/dev/null
```

**Windows users:** The ssh-agent service should start automatically. If not:

```powershell
# In PowerShell (as Administrator)
Get-Service ssh-agent | Set-Service -StartupType Automatic
Start-Service ssh-agent
```

> **Note:** VS Code automatically forwards your SSH agent to the container, so your keys will be available inside the devcontainer.

[Learn more about using SSH keys with GitHub →](https://docs.github.com/en/authentication/connecting-to-github-with-ssh)

## Setup Instructions

### Step 1: Clone Repository in Named Container Volume

For **optimal performance**, especially on macOS, clone the repository directly into a Docker volume rather than your local filesystem. This is crucial for Bazel performance.

#### Why Named Volumes?

- ✅ **Better I/O Performance**: Native filesystem speed inside container
- ✅ **Bazel Compatibility**: Avoids macOS filesystem case-sensitivity issues
- ✅ **Consistent Behavior**: Same experience across all platforms
- ✅ **Isolation**: Keeps container data separate from host

#### Cloning Steps:

1. **Open VS Code Command Palette**

   - macOS: `Cmd+Shift+P`
   - Windows/Linux: `Ctrl+Shift+P`

2. **Run Clone Command**

   - Type: `Dev Containers: Clone Repository in Named Container Volume...`
   - Select it from the list
   - [Learn more about improving performance with container volumes →](https://code.visualstudio.com/remote/advancedcontainers/improve-performance#_use-a-targeted-named-volume)

3. **Enter Repository URL**

##### For use with the internal mongodb repo:

```
git@github.com:10gen/mongo.git
```

Or use HTTPS:

```
https://github.com/mongodb/mongo.git
```

##### For use with the public mongodb repo:

```
git@github.com:mongodb/mongo.git
```

Or use HTTPS:

```
https://github.com/mongodb/mongo.git
```

> **Tip**: SSH URLs are recommended if you have SSH keys configured

4. **Choose Volume Name**

   - Enter a name like: `mongo-workspace`
   - This creates a Docker volume with that name
   - You can reference it later: `docker volume ls`

5. **Wait for Initial Setup**
   - VS Code will:
     - Clone the repository to the volume
     - Build the devcontainer image
     - Start the container
     - Install VS Code extensions
     - Run post-creation commands

### Step 2: Verify Your Setup

Let's make sure everything is working correctly.

#### 2.1 Check Toolchain Installation

```bash
# Verify GCC version
gcc --version

# Verify Python version
python3 --version
```

#### 2.2 Verify Python Virtual Environment

The devcontainer automatically sets up a Python virtual environment:

```bash
# Should already be activated (check for (python3-venv) in prompt)
which python
# Should show: /workspaces/mongo/python3-venv/bin/python

# Check poetry is available
poetry --version
```

#### 2.3 Test Bazel Build

Try building a target:

```bash
bazel build install-mongod
```

This may take a while on first run but verifies:

- Bazel is configured correctly
- Toolchain is working
- Build system is functional

#### 2.4 Check VS Code Extensions

The following extensions should be installed and active:

- clangd (C++ IntelliSense)
- ESLint (JavaScript linting)
- Ruff (Python formatting)
- Bazel (Build system support)

Check: View → Extensions and verify they're enabled.

### Step 3: Understanding Your Environment

#### Workspace Location

Your code lives in a Docker volume, not your local filesystem. To access it:

- **Inside Container**: `/workspaces/mongo` (default workspace folder)

#### Persistent Volumes

Several volumes persist data across container restarts:

1. **`engflow_auth`** → `~/.config/engflow_auth`

   - EngFlow remote execution credentials
   - Survives container rebuilds

2. **`mongo-cache`** → `~/.cache`

   - Bazel cache
   - Tool caches
   - Significantly speeds up rebuilds

3. **`mongo-python3-venv`** → `/workspaces/mongo/python3-venv`

   - Python virtual environment
   - Poetry-managed dependencies
   - Persists across container updates

4. **`mongo-bashhistory`** → `/commandhistory`
   - Terminal command history
   - Available across sessions

#### Data Directory

MongoDB data directory is automatically created at `/data/db` with proper permissions.

## Next Steps

### Explore the Documentation

- [Architecture Details](./architecture.md) - Learn how the devcontainer works
- [Advanced Usage](./advanced.md) - Customize and extend your setup
- [Troubleshooting](./troubleshooting.md) - Fix common issues

## Common First-Time Issues

### SSH Clone Fails

**Problem**: Clone fails with "Permission denied (publickey)" or "git@github.com: Permission denied"

**Solution**:

```bash
# Test SSH connection from your HOST machine (before starting container)
ssh -T git@github.com

# If this fails, your SSH keys aren't set up correctly
# Follow the SSH key setup instructions above

# Check if ssh-agent has your key
ssh-add -l

# If empty, add your key
ssh-add ~/.ssh/id_ed25519  # or id_rsa

# If you see "Could not open a connection to your authentication agent"
eval "$(ssh-agent -s)"
ssh-add ~/.ssh/id_ed25519
```

### SSH Works Locally But Not in Container

**Problem**: SSH works on your host machine but fails inside the container

**Cause**: SSH agent forwarding may not be working

**Solution**:

```bash
# On your HOST machine, ensure ssh-agent is running with your keys
ssh-add -l  # Should list your keys

# If not listed, add them
ssh-add ~/.ssh/id_ed25519

# In VS Code, rebuild the container
# Command Palette → "Dev Containers: Rebuild Container"
```

**VS Code SSH Agent Forwarding**: The Dev Containers extension automatically forwards your SSH agent, but this requires:

- SSH agent running on host with keys loaded
- SSH key files in default location (`~/.ssh/`)

[Learn more about sharing git credentials →](https://code.visualstudio.com/remote/advancedcontainers/sharing-git-credentials)

### Container Build Fails

**Problem**: Docker build fails with "no space left on device"

**Solution**:

```bash
# Clean up Docker
docker system prune

# Increase Docker disk space in Docker Desktop/Rancher Desktop settings
```

### Slow Performance on macOS

**Problem**: Bazel builds are very slow

**Solution**:

- ✅ Ensure you cloned into a named volume (not bind mount)
- ✅ Allocate more CPU/Memory to Docker
- ✅ Use EngFlow remote execution (see [Advanced Usage](./advanced.md))

### Python Virtual Environment Not Activated

**Problem**: Python commands not found or using wrong version

**Solution**:

```bash
# Manually activate
source /workspaces/mongo/python3-venv/bin/activate

# Reinstall dependencies
poetry install --no-root --sync
```

### VS Code Extensions Not Working

**Problem**: clangd, ESLint, or other extensions show errors

**Solution**:

```bash
# Rebuild compile_commands.json for clangd
bazel build compiledb --config=local

# Restart VS Code window
# Cmd/Ctrl+Shift+P → "Developer: Reload Window"
```

## Alternative Setup: Local Clone + Reopen in Container

If you prefer to clone locally first (not recommended for best performance):

1. Clone the repository to your local machine:

   ```bash
   git clone git@github.com:10gen/mongo.git
   cd mongo
   ```

2. Open in VS Code:

   ```bash
   code .
   ```

3. Open Command Palette (`Cmd/Ctrl+Shift+P`)

4. Run: `Dev Containers: Reopen in Container`

⚠️ **Note**: This uses bind mounts and may have performance issues on macOS with Bazel.

## Getting Help

- **Questions**: See the [FAQ](./faq.md)
- **Issues**: Check [Troubleshooting Guide](./troubleshooting.md)
- **Bug Reports**: Open an issue in the MongoDB repository

---

**Next**: [Learn about the Architecture →](./architecture.md)
