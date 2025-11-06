# Customizing Your Dev Container

This guide covers personal customizations you can make to your MongoDB devcontainer **without modifying the repository's devcontainer configuration**. These are user-level settings that only affect your development environment.

**Want to modify the devcontainer setup for everyone?** See [Contributing Customizations](#contributing-customizations) at the bottom.

**For general VS Code settings** (themes, fonts, keybindings), see the [VS Code documentation](https://code.visualstudio.com/docs/getstarted/settings).

## Table of Contents

- [Persistent Dotfiles](#persistent-dotfiles)
- [Always-Installed Features](#always-installed-features)
- [Contributing Customizations](#contributing-customizations)

## Persistent Dotfiles

VS Code supports automatically cloning and applying your dotfiles when creating a devcontainer.

[Learn more about personalizing with dotfiles →](https://code.visualstudio.com/docs/devcontainers/containers#_personalizing-with-dotfile-repositories)

**How it works:**

1. Create a dotfiles repository (e.g., `github.com/yourusername/dotfiles`)
2. Add your configuration files (`.bashrc`, `.gitconfig`, `.vimrc`, etc.)
3. Configure VS Code to use your dotfiles:

```json
// In your user settings.json
{
  "dotfiles.repository": "yourusername/dotfiles",
  "dotfiles.targetPath": "~/dotfiles",
  "dotfiles.installCommand": "install.sh"
}
```

**Example dotfiles structure:**

```
dotfiles/
├── .bashrc
├── .gitconfig
├── .vimrc
├── .bash_aliases
└── install.sh
```

**Example `install.sh`:**

```bash
#!/bin/bash
ln -sf ~/dotfiles/.bashrc ~/.bashrc
ln -sf ~/dotfiles/.gitconfig ~/.gitconfig
ln -sf ~/dotfiles/.vimrc ~/.vimrc
source ~/.bashrc
```

## Always-Installed Features

You can configure VS Code to always install certain features in all your devcontainers.

[Learn more about always-installed features →](https://code.visualstudio.com/docs/devcontainers/containers#_always-installed-features)

```json
// In your user settings.json
{
  "dev.containers.defaultFeatures": {
    "ghcr.io/devcontainers/features/git:1": {},
    "ghcr.io/devcontainers/features/github-cli:1": {}
  }
}
```

This applies to all devcontainers you work with, not just MongoDB.

**Browse available features:** [https://containers.dev/features](https://containers.dev/features)

## Contributing Customizations

The customizations above are all user-level and don't require changes to the repository. If you want to modify the devcontainer setup itself to benefit all MongoDB developers, you'll need to submit a PR.

**Examples of repository-level customizations:**

- Adding new devcontainer features (tools, languages, etc.)
- Configuring default port forwarding
- Adding environment variables for all developers
- Setting up bind mounts to host directories
- Modifying container lifecycle hooks
- Improving build performance or caching

**How to contribute:**

1. Make your changes to `.devcontainer/devcontainer.json` or related files
2. Test thoroughly - rebuild your container and verify everything works
3. Document your changes clearly in your PR description
4. Update relevant documentation (like this guide)
5. Submit a PR to the main repository

**Related documentation:**

- [Architecture Guide](./architecture.md) - Understand the devcontainer setup before modifying it
- [VS Code devcontainer.json reference](https://code.visualstudio.com/docs/devcontainers/containers#_devcontainerjson-reference)

---

**See Also:**

- [Getting Started](./getting-started.md) - Initial setup
- [Architecture](./architecture.md) - How devcontainers work
- [Advanced Usage](./advanced.md) - Multiple containers, backups, workflows
- [Troubleshooting](./troubleshooting.md) - Fix issues
- [VS Code Dev Containers Documentation](https://code.visualstudio.com/docs/devcontainers/containers) - General VS Code features
