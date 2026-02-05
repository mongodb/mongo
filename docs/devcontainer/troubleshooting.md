# Troubleshooting Dev Containers

This guide covers common issues and their solutions when working with MongoDB dev containers.

## Table of Contents

- [Rancher Desktop Problems](#rancher-desktop-problems)
- [Container Build Issues](#container-build-issues)
- [Performance Problems](#performance-problems)
- [VS Code Issues](#vs-code-issues)
- [Git and SSH Issues](#git-and-ssh-issues)
- [Build System Issues](#build-system-issues)
- [Python Environment Issues](#python-environment-issues)
- [Volume and Persistence Issues](#volume-and-persistence-issues)
- [Platform-Specific Issues](#platform-specific-issues)
- [Docker Provider Issues](#docker-provider-issues)
- [Debugging Tips](#debugging-tips)

## Rancher Desktop Problems

### VSCode prompts for installation of Docker Desktop when I have Rancher Desktop installed

**Symptoms**

```
Docker version <version> or later is required
```

**Solution**

Restart VSCode. If you install Rancher Desktop while you already have VSCode open, it doesn't properly detect the Docker socket and prompts you to install Docker Desktop by mistake.

## Container Build Issues

### Build Fails with SSH Bind Mount Error

**Symptoms:**

```
Error response from daemon: invalid mount config for type "bind": bind source path does not exist: /Users/username/.ssh
```

Or on macOS/Linux systems using certain Docker providers:

```
Error response from daemon: invalid mount config for type "bind": bind source path does not exist: /socket_mnt/...
```

**Root Cause:**

The devcontainer configuration mounts your `~/.ssh` directory to enable Git operations over SSH. If this directory doesn't exist on your host machine, the container fails to start. **This directory is required even if you plan to use HTTPS instead of SSH for cloning.**

**Solutions:**

1. **Create the .ssh directory on your host machine:**

   ```bash
   # On your HOST machine (not in container)
   mkdir -p ~/.ssh
   ```

2. **Rebuild the container:**

   - Command Palette → "Dev Containers: Rebuild Container"

**Note on SSH Agent Forwarding:**

SSH agent forwarding behavior varies by Docker provider on macOS:

- **Docker Desktop**: Automatic SSH agent forwarding built-in
- **OrbStack**: Automatic SSH agent forwarding built-in
- **Rancher Desktop**:
  - With dockerd runtime: Automatic agent forwarding
  - With containerd runtime: Agent forwarding requires additional setup

To use SSH agent forwarding, ensure your SSH keys are added to your host's SSH agent before starting the container:

```bash
ssh-add ~/.ssh/id_ed25519  # or your key name
ssh-add -l  # verify keys are loaded
```

### Build Fails with "No Space Left on Device"

**Symptoms:**

```
Error: failed to solve: write /var/lib/docker/...: no space left on device
```

**Solutions:**

1. **Clean up Docker resources:**

   ```bash
   # Remove unused containers, images, and volumes
   docker system prune -a --volumes

   # Check disk usage
   docker system df
   ```

2. **Increase Docker disk allocation:**

   **Rancher Desktop:**

   Rancher Desktop does not have a UI for increasing disk size. To increase it:

   **On macOS or Linux:**

   1. Stop Rancher Desktop completely
   2. Create or edit the VM configuration file:
      - **macOS**: `~/Library/Application Support/rancher-desktop/lima/_config/override.yaml`
      - **Linux**: `~/.config/rancher-desktop/lima/_config/override.yaml`
   3. Add or modify the disk size setting:
      ```yaml
      disk: 100GB
      ```
   4. Start Rancher Desktop
   5. If Rancher Desktop was previously initialized, you may need to perform a factory reset (Preferences → Troubleshooting → Reset Kubernetes) for the disk size change to take effect.

   **On Windows (WSL2):**

   The disk is managed by WSL2:

   1. Stop Rancher Desktop
   2. Run: `wsl --shutdown`
   3. Follow Microsoft's guide to increase WSL2 disk size: https://learn.microsoft.com/en-us/windows/wsl/disk-space

   **Docker Desktop:**

   1. Open Docker Desktop
   2. Go to Settings → Resources → Disk image size
   3. Increase to at least 60 GB (100+ GB recommended for MongoDB development)
   4. Click "Apply & Restart"

3. **Remove old dev containers:**

   ```bash
   # List all containers
   docker ps -a

   # Remove specific container
   docker rm <container_id>

   # Remove all stopped containers
   docker container prune
   ```

### Build Fails with Toolchain Download Error

**Symptoms:**

```
Error: curl: (22) The requested URL returned error: 404
Error: Failed to download toolchain
```

**Solutions:**

1. **Check internet connection**: Ensure you can access S3:

   ```bash
   curl -I https://s3.amazonaws.com/boxes.10gen.com/
   ```

2. **Verify toolchain URL:**

   ```bash
   # Check what's configured
   cat .devcontainer/toolchain_config.env

   # Try downloading manually to test
   curl -I "$(grep TOOLCHAIN_URL .devcontainer/toolchain_config.env | cut -d'"' -f2)"
   ```

3. **If toolchain URL is broken**, report it to the MongoDB team. This is a devcontainer configuration issue that needs to be fixed upstream.

### Build Fails with Checksum Mismatch

**Symptoms:**

```
Error: SHA256 checksum mismatch
Expected: abc123...
Got: def456...
```

**This typically indicates the toolchain was updated but the config file wasn't.**

**Solutions:**

1. **Pull latest changes** from the repository (the maintainers may have already fixed this):

   ```bash
   git pull
   # Then rebuild container
   ```

2. **Clear Docker cache and rebuild:**

   ```bash
   # Command Palette → "Dev Containers: Rebuild Container Without Cache"
   ```

3. **If problem persists**, this is likely a devcontainer configuration issue - report it to the MongoDB team.

### Container Fails to Start

**Symptoms:**

- VS Code shows "Container failed to start"
- No error message visible

**Solutions:**

1. **Check Docker logs:**

   ```bash
   # Find container ID
   docker ps -a

   # View logs
   docker logs <container_id>
   ```

2. **Rebuild container:**

   - Command Palette → "Dev Containers: Rebuild Container"
   - Or: "Dev Containers: Rebuild Container Without Cache"

3. **Check Docker daemon status:**
   ```bash
   docker info
   docker version
   ```

## Performance Problems

### Slow Build Times

**Symptoms:**

- Bazel builds taking 30+ minutes for incremental changes
- File operations feel sluggish

**Solutions:**

1. **Verify you're using a named volume (not bind mount):**

   ```bash
   # Inside container
   df -h /workspaces/mongo

   # Should NOT show a mount from host filesystem
   # Should be part of container's internal filesystem
   ```

   If using bind mount, migrate to named volume:

   - Clone repository in new named volume
   - See [Getting Started](./getting-started.md#step-1-clone-repository-in-named-container-volume)

2. **Increase Docker resources:**

   - **CPUs**: 6+ cores recommended
   - **Memory**: 16 GB recommended
   - **Swap**: 2-4 GB

3. **Check cache volume is mounted:**

   ```bash
   # Inside container
   ls -la ~/.cache/bazel

   # Should have bazel cache directory
   ```

4. **Verify no antivirus scanning Docker:**
   - Exclude Docker Desktop directory from antivirus
   - Exclude devcontainer volumes

### Slow File Operations on macOS

**Symptoms:**

- `git status` takes 5+ seconds
- File save is delayed
- Terminal autocomplete is slow

**Root Cause:**
Bind mounts on macOS use osxfs which has high latency for filesystem operations.

**Solution:**
✅ **Use named volumes instead of bind mounts** (see Getting Started guide)

### High CPU Usage

**Symptoms:**

- Docker process using 100%+ CPU
- System becomes unresponsive

**Solutions:**

1. **Check for runaway processes:**

   ```bash
   # Inside container
   top
   htop  # If available
   ```

2. **Check for file watcher issues:**
   ```bash
   # Limit file watchers (Linux)
   echo fs.inotify.max_user_watches=524288 | sudo tee -a /etc/sysctl.conf
   sudo sysctl -p
   ```

## VS Code Issues

For additional VS Code-specific troubleshooting, see:

- [VS Code Dev Containers FAQ](https://code.visualstudio.com/docs/devcontainers/faq)
- [VS Code Dev Containers Tips and Tricks](https://code.visualstudio.com/docs/devcontainers/tips-and-tricks)

### Extensions Not Installing

**Symptoms:**

- Recommended extensions don't install automatically
- Extension list is empty

**Solutions:**

1. **Manually install extensions:**

   - View → Extensions
   - Search for each recommended extension
   - Click Install in Container

2. **Check extension compatibility:**

   - Some extensions don't support containers
   - Look for "This extension is enabled globally" message

3. **Reinstall extensions:**
   ```bash
   # Command Palette
   > Developer: Reinstall Extension...
   ```

### clangd Not Working

**Symptoms:**

- No C++ IntelliSense
- "clangd: Server not running" error
- Red squiggles everywhere

**Solutions:**

1. **Generate compile_commands.json:**

   ```bash
   bazel build compiledb --config=local

   # Verify it exists
   ls -lh compile_commands.json
   ```

2. **Check clangd path:**

   ```bash
   # Verify the wrapper script exists
   ls -l buildscripts/clangd_vscode.sh

   # Test it
   ./buildscripts/clangd_vscode.sh --version
   ```

3. **Restart clangd:**

   - Command Palette → "clangd: Restart language server"

4. **Check clangd output:**

   - Output → clangd (dropdown)
   - Look for errors

5. **Clear clangd cache:**
   ```bash
   rm -rf ~/.cache/clangd
   ```

### Python Extension Not Finding Interpreter

**Symptoms:**

- "Select Python Interpreter" notification
- Python imports not recognized
- Linting/formatting not working

**Solutions:**

1. **Verify venv exists:**

   ```bash
   ls -la python3-venv/bin/python
   source python3-venv/bin/activate
   which python
   ```

2. **Select interpreter in VS Code:**

   - Command Palette → "Python: Select Interpreter"
   - Choose `python3-venv/bin/python`

3. **Rebuild venv:**

   ```bash
   rm -rf python3-venv
   /opt/mongodbtoolchain/v5/bin/python3.13 -m venv python3-venv
   source python3-venv/bin/activate
   poetry install --no-root --sync
   ```

4. **Check settings.json:**
   ```json
   {
     "python.defaultInterpreterPath": "python3-venv/bin/python"
   }
   ```

### Format on Save Not Working

**Symptoms:**

- Files don't format when saved
- Manual format works

**Solutions:**

1. **Check settings:**

   ```json
   {
     "editor.formatOnSave": true,
     "[cpp]": {
       "editor.defaultFormatter": "xaver.clang-format",
       "editor.formatOnSave": true
     }
   }
   ```

2. **Verify formatter is installed:**

   - clang-format: Check extension is active
   - Ruff: Check extension is active
   - Prettier: Verify path in settings

3. **Test formatter manually:**
   - Right-click → Format Document
   - Check for errors in Output panel

## Git and SSH Issues

### SSH Clone Fails: Permission Denied

**Symptoms:**

```
git@github.com: Permission denied (publickey).
fatal: Could not read from remote repository.
```

**Solutions:**

1. **Verify SSH keys exist on host:**

   ```bash
   # On your HOST machine (not in container)
   ls -la ~/.ssh/id_*.pub

   # Should see id_ed25519.pub, id_rsa.pub, or similar
   ```

2. **Test SSH connection to GitHub:**

   ```bash
   # On HOST machine
   ssh -T git@github.com

   # Should see: "Hi username! You've successfully authenticated..."
   # If this fails, your SSH key isn't added to GitHub
   ```

3. **Add SSH key to GitHub:**

   ```bash
   # Copy your public key
   cat ~/.ssh/id_ed25519.pub  # or id_rsa.pub

   # Go to https://github.com/settings/keys
   # Click "New SSH key" and paste
   ```

4. **Ensure ssh-agent has your key:**

   ```bash
   # On HOST machine
   ssh-add -l

   # If empty or shows "Could not open connection"
   eval "$(ssh-agent -s)"
   ssh-add ~/.ssh/id_ed25519  # or id_rsa
   ```

See [Getting Started - SSH Setup](./getting-started.md#4-configure-ssh-keys-recommended) for detailed instructions.

### SSH Works on Host But Not in Container

**Symptoms:**

- Can clone/push from host machine
- Same operations fail inside devcontainer
- "Permission denied" or asks for password

**Root Cause:**
SSH agent forwarding isn't working properly.

**Solutions:**

1. **Verify agent forwarding requirements:**

   ```bash
   # On HOST machine (before opening container)
   # SSH agent must be running
   echo $SSH_AUTH_SOCK
   # Should show a path, not empty

   # Agent must have keys loaded
   ssh-add -l
   # Should list your SSH keys
   ```

2. **Add keys to agent if missing:**

   ```bash
   # On HOST machine
   ssh-add ~/.ssh/id_ed25519  # or id_rsa

   # Verify
   ssh-add -l
   ```

3. **Restart VS Code and rebuild container:**

   - Close VS Code completely
   - Restart VS Code
   - Command Palette → "Dev Containers: Rebuild Container"

4. **Check SSH config (macOS):**

   ```bash
   # On HOST machine
   # Add to ~/.ssh/config
   Host *
     AddKeysToAgent yes
     UseKeychain yes
     IdentityFile ~/.ssh/id_ed25519
   ```

5. **Start ssh-agent automatically (Linux):**

   ```bash
   # Add to ~/.bashrc or ~/.zshrc on HOST
   if [ -z "$SSH_AUTH_SOCK" ]; then
     eval "$(ssh-agent -s)"
     ssh-add ~/.ssh/id_ed25519
   fi
   ```

6. **Windows: Ensure ssh-agent service is running:**

   ```powershell
   # In PowerShell as Administrator (on HOST)
   Get-Service ssh-agent | Set-Service -StartupType Automatic
   Start-Service ssh-agent

   # Then add your key
   ssh-add $env:USERPROFILE\.ssh\id_ed25519
   ```

### Git Push Asks for Username/Password

**Symptoms:**

```
Username for 'https://github.com':
Password for 'https://user@github.com':
```

**Causes:**

1. Repository was cloned with HTTPS instead of SSH
2. SSH agent forwarding not working

**Solutions:**

**Option 1: Switch to SSH** (recommended):

```bash
# Check current remote URL
git remote -v

# If using HTTPS, switch to SSH
git remote set-url origin <ssh url>

# Verify
git remote -v
```

**Option 2: Use Personal Access Token** (for HTTPS):

```bash
# Generate token at https://github.com/settings/tokens
# Use token as password when prompted

# Or configure credential helper
git config --global credential.helper store
# Next time you enter credentials, they'll be saved
```

**Option 3: Fix SSH agent forwarding**:
See "SSH Works on Host But Not in Container" section above.

### Multiple SSH Keys (Personal + Work)

**Problem:** Have multiple GitHub accounts or SSH keys

**Solution:** Use SSH config to manage multiple keys:

```bash
# On HOST machine, edit ~/.ssh/config
Host github.com-work
  HostName github.com
  User git
  IdentityFile ~/.ssh/id_ed25519_work

Host github.com-personal
  HostName github.com
  User git
  IdentityFile ~/.ssh/id_ed25519_personal

# Add both keys to agent
ssh-add ~/.ssh/id_ed25519_work
ssh-add ~/.ssh/id_ed25519_personal

# Clone using specific host alias
git clone git@github.com-work:<repo>
```

### Cannot Sign Commits with GPG

**Symptoms:**

```
error: gpg failed to sign the data
fatal: failed to write commit object
```

**Solution:**

GPG signing requires additional setup in devcontainers.

**Use SSH signing** (GitHub now supports this):

```bash
# Configure git to use SSH for signing
git config --global gpg.format ssh
git config --global user.signingkey ~/.ssh/id_ed25519.pub
git config --global commit.gpgsign true
```

## Build System Issues

### Bazel Fails with "Server terminated abruptly"

**Symptoms:**

```
ERROR: Bazel server terminated abruptly
```

**Solutions:**

1. **Clean Bazel cache:**

   ```bash
   bazel clean --expunge
   ```

2. **Check disk space:**

   ```bash
   df -h
   ```

3. **Restart container:**
   - Command Palette → "Dev Containers: Rebuild Container"

### Bazel Build Fails with Toolchain Errors

**Symptoms:**

```
ERROR: No matching toolchains found
ERROR: Cannot find compiler
```

**Solutions:**

1. **Verify toolchain installation:**

   ```bash
   ls -la /opt/mongodbtoolchain/revisions/

   # Check compiler
   /opt/mongodbtoolchain/v5/bin/gcc --version
   ```

2. **Source toolchain environment:**

   ```bash
   source /opt/mongodbtoolchain/revisions/*/activate
   ```

3. **Rebuild container** to reinstall toolchain

### EngFlow Authentication Fails

**Symptoms:**

```
ERROR: Failed to authenticate with EngFlow
ERROR: Build Event Service upload failed
```

**Solutions:**

1. **Check if credentials exist:**

   ```bash
   ls -la ~/.config/engflow_auth/
   ```

2. **Re-authenticate with EngFlow:**

   ```bash
   rm -r ~/.config/engflow_auth/*
   bazel run engflow_auth
   ```

3. **Build without EngFlow:**
   ```bash
   bazel build --config=local install-mongod
   ```

## Python Environment Issues

### Poetry Install Fails

**Symptoms:**

```
ERROR: Failed to install packages
KeyringError: ...
```

**Solutions:**

1. **Set keyring backend:**

   ```bash
   export PYTHON_KEYRING_BACKEND=keyring.backends.null.Keyring
   poetry install --no-root --sync
   ```

2. **Clear Poetry cache:**

   ```bash
   poetry cache clear --all pypi
   poetry install --no-root --sync
   ```

3. **Verify Poetry version:**
   ```bash
   poetry --version
   # Should be version specified in poetry_requirements.txt
   ```

### Virtual Environment Not Activating

**Symptoms:**

- `(python3-venv)` not in prompt
- `which python` shows system Python

**Solutions:**

1. **Manually activate:**

   ```bash
   source python3-venv/bin/activate
   ```

2. **Check shell config:**

   ```bash
   cat ~/.bashrc | grep python3-venv
   cat ~/.zshrc | grep python3-venv
   ```

3. **Re-source config:**
   ```bash
   source ~/.bashrc  # or ~/.zshrc
   ```

### Import Errors in Python Scripts

**Symptoms:**

```
ModuleNotFoundError: No module named 'pymongo'
```

**Solutions:**

1. **Ensure venv is activated:**

   ```bash
   which python
   # Should show: /workspaces/mongo/python3-venv/bin/python
   ```

2. **Reinstall dependencies:**

   ```bash
   source python3-venv/bin/activate
   poetry install --no-root --sync
   ```

3. **Check Poetry lock file:**
   ```bash
   poetry check
   poetry lock --check
   ```

## Volume and Persistence Issues

### Data Lost After Container Restart

**Symptoms:**

- Bazel cache gone
- History cleared
- Python venv empty

**Root Cause:**
Volumes not mounting correctly

**Solutions:**

1. **Check volumes are mounted:**

   ```bash
   docker inspect <container_id> | grep -A 10 Mounts
   ```

2. **Verify volumes exist:**

   ```bash
   docker volume ls | grep mongo
   ```

3. **Check devcontainer.json mounts:**
   ```json
   "mounts": [
     {
       "source": "mongo-cache",
       "target": "/home/youruser/.cache",
       "type": "volume"
     }
   ]
   ```

### Cannot Access Files from Host

**Symptoms:**

- Can't open files in host OS
- Need to copy files out of container

**Solution:**

Files in named volumes are in Docker's VM, not directly accessible.

**To access:**

```bash
# Copy file from container to host
docker cp <container_id>:/workspaces/mongo/file.txt ~/Downloads/

# Or use VS Code
# Right-click file → Download...
```

**To edit with external tools:**
Use bind mounts instead of named volumes (but sacrifices performance).

### Volume Fills Up Disk

**Symptoms:**

```bash
docker system df
# Shows huge SIZE for volumes
```

**Solutions:**

1. **Clean Bazel cache:**

   ```bash
   # Inside container
   bazel clean --expunge
   ```

2. **Remove old volumes:**

   ```bash
   # List volumes
   docker volume ls

   # Remove specific volume if needed (WARNING: loses data!)
   docker volume rm old-cache-volume
   ```

3. **Limit Bazel cache size:**
   ```bash
   # Add to ~/.bazelrc
   echo "build --disk_cache=~/.cache/bazel --disk_cache_size=10G" >> ~/.bazelrc
   ```

## Platform-Specific Issues

### macOS: "Docker Desktop Is Not Running"

**Solutions:**

1. **Start Docker Desktop/Rancher Desktop:**

   - Check menu bar for Docker icon
   - Launch the application

2. **Reset Docker:**

   - Rancher Desktop → Troubleshooting → Reset Kubernetes
   - Docker Desktop → Troubleshoot → Reset to factory defaults

3. **Check Docker context:**
   ```bash
   docker context ls
   docker context use default
   ```

### macOS: M1/M2 ARM Issues

**Symptoms:**

- "exec format error"
- Build fails with architecture mismatch

**Solutions:**

1. **Verify base image supports ARM:**

   ```bash
   docker pull quay.io/mongodb/bazel-remote-execution:ubuntu24-...
   docker inspect --format='{{.Architecture}}' <image_id>
   ```

2. **Use platform flag if needed:**

   ```dockerfile
   FROM --platform=linux/amd64 <base_image>
   ```

3. **Check Rosetta 2 is enabled** (Rancher Desktop)

### Windows: WSL2 Integration Issues

**Symptoms:**

- Container won't start on Windows
- File permission errors

**Solutions:**

1. **Enable WSL2 integration:**

   - Docker Desktop → Settings → Resources → WSL Integration
   - Enable integration for your WSL distro

2. **Use WSL2 terminal:**

   - Open Ubuntu (or other WSL distro)
   - Clone and work from WSL filesystem, not `/mnt/c/`

3. **Check WSL version:**
   ```bash
   wsl --list --verbose
   # Should show VERSION 2
   ```

### Linux: Permission Denied Errors

**Symptoms:**

```
permission denied while trying to connect to Docker daemon
```

**Solutions:**

1. **Add user to docker group:**

   ```bash
   sudo usermod -aG docker $USER
   newgrp docker  # Or logout/login
   ```

2. **Check Docker socket permissions:**
   ```bash
   ls -l /var/run/docker.sock
   sudo chmod 666 /var/run/docker.sock  # Temporary
   ```

## Docker Provider Issues

### Rancher Desktop: Container Engine Not dockerd

**Symptoms:**

- Cannot build devcontainer
- Unexpected behavior

**Solution:**

- Rancher Desktop → Preferences → Container Engine
- Select "dockerd (moby)"
- Restart Rancher Desktop

### Docker Desktop: Resource Limits Too Low

**Symptoms:**

- Slow builds
- Out of memory errors

**Solution:**
Go to Docker Desktop → Settings → Resources and allocate generously:

- **CPUs**: Allocate as many as possible (leave 1-2 for host OS)
- **Memory**: Allocate as much as possible (leave ~4-8 GB for host OS)
- **Swap**: Optional but can help (2-4 GB if you have disk space)
- **Disk**: Ensure plenty available (60+ GB recommended)

> **Note:** MongoDB builds are resource-intensive. More resources = significantly faster builds.

### OrbStack: Features Not Working

**Symptoms:**

- Docker-outside-of-docker doesn't work
- Volume mounts fail

**Solution:**
OrbStack has some limitations with devcontainer features. Try:

1. Update to latest OrbStack version
2. Check OrbStack documentation for devcontainer compatibility
3. Consider switching to Rancher Desktop for full feature support

## Debugging Tips

### Enable Verbose Logging

**VS Code Dev Container logs:**

1. Command Palette → "Dev Containers: Show Container Log"
2. Check for errors during build/start

**Docker logs:**

```bash
# Container logs
docker logs <container_id>

# Follow logs in real-time
docker logs -f <container_id>
```

**Bazel verbose:**

```bash
bazel build --verbose_failures --sandbox_debug install-mongod
```

### Inspect Running Container

```bash
# Get container ID
docker ps

# Exec into container
docker exec -it <container_id> /bin/bash

# Check processes
docker exec <container_id> ps aux

# Check environment
docker exec <container_id> env
```

### Check Resource Usage

```bash
# Inside container
df -h          # Disk usage
free -h        # Memory
top            # CPU/Memory by process

# From host
docker stats   # Live resource usage
```

### Rebuild from Scratch

Sometimes the best fix is a clean rebuild:

```bash
# Stop and remove container
docker stop <container_id>
docker rm <container_id>

# Rebuild without cache
# Command Palette → "Dev Containers: Rebuild Container Without Cache"
```

### Test Outside Devcontainer

To isolate whether an issue is devcontainer-specific:

```bash
# Clone locally
git clone git@github.com:10gen/mongo.git
cd mongo

# Try building without devcontainer
# (Requires local toolchain setup)
```

## Getting More Help

If your issue isn't covered here:

1. **Check VS Code Docs**: [code.visualstudio.com/docs/devcontainers](https://code.visualstudio.com/docs/devcontainers/containers)
2. **Search Issues**: MongoDB GitHub repository issues
3. **Ask the Team**: MongoDB developers Slack/chat
4. **File a Bug**: Include:
   - Error messages
   - Container logs
   - Steps to reproduce
   - OS and Docker version
   - devcontainer.json and Dockerfile (if modified)

---

**See Also:**

- [Architecture](./architecture.md) - Understand how things work
- [Advanced Usage](./advanced.md) - Customize your setup
- [FAQ](./faq.md) - Common questions
