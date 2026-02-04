# Extensions

This module provides utilities for setting up and configuring MongoDB extensions in resmoke test suites.

## Overview

Extensions are dynamically loaded shared objects (`.so` files) that provide additional functionality to MongoDB. The utilities in this folder can handle:

1. Discovering extension `.so` files in build directories
1. Generating `.conf` configuration files for extensions
1. Downloading external extensions (e.g. `mongot-extension`) from S3.
1. Cleaning up configuration files after tests

## Configuration File Generation in Tests

Extension `.conf` files are YAML configuration files that tell the server how to load an extension. They contain:

- `sharedLibraryPath`: Path to the `.so` file
- `extensionOptions`: Optional configuration parameters for the extension

For example:

```yaml
# foo.conf
sharedLibraryPath: /path/to/libfoo_mongo_extension.so
extensionOptions:
  someSetting: <value>
```

### How Config Files Are Generated

The `generate_extension_configs.py` module creates `.conf` files:

1. Receives a list of `.so` file paths (either from automatic discovery via `find_and_generate_extension_configs.py`, or manually via `--so-files` command-line argument)
1. For each `.so`, creates a `.conf` file in the temp directory (`/tmp/mongo/extensions/`)
1. Looks up corresponding extension options from `src/mongo/db/extension/test_examples/configurations.yml`, if any are specified
1. Writes the config file with `sharedLibraryPath` and any `extensionOptions`

### Automatic Discovery and Generation

The `find_and_generate_extension_configs.py` module combines discovery and generation:

1. Searches for `*_mongo_extension.so` files in build directories:
   - **Evergreen:** `dist-test/lib/`
   - **Local:** `bazel-bin/install-dist-test/lib/` or `bazel-bin/install-extensions/lib/`
1. Generates `.conf` files with a unique UUID suffix to avoid collisions
1. Adds the `loadExtensions` parameter to mongod/mongos options

## `mongot-extension` Setup

The `setup_mongot_extension.py` script downloads and configures the Rust `mongot-extension` binary for extension $vectorSearch testing.
All `vector_search_extension_*` test suites use this framework to run server Extensions code with the Rust `mongot-extension.so`.

### How It Works

1. Downloads the appropriate `mongot-extension` tarball from S3 based on platform and architecture
1. Verifies the download using SHA256 checksums from `buildscripts/s3_binary/hashes.py`
1. Extracts the `.so` file and creates a configuration file

### Updating Checksums

The `mongot-extension` binaries are stored in S3 and verified using hardcoded SHA256 checksums in `buildscripts/s3_binary/hashes.py`. When the binary is updated on S3, that file can fall out of date.
To prevent unexpected test failures caused by untracked updates, all executable code downloaded from S3 must undergo checksum verification.
This ensures that every change is explicitly documented in the commit history, providing full visibility into what has changed between runs.

> [!IMPORTANT]
> You may see a `ValueError: Hash mismatch` when running resmoke because `hashes.py` does not contain the expected (now outdated) hash. If you are in that situation, update the checksums as follows.

This cross-repo test infrastructure is temporary for the initial rollout of extension `$vectorSearch`. Long-term, all extension testing will live outside the server repository.

**To update the checksums:**

1. Run the following commands to get the current SHA256 hash for each platform/architecture variant. Use **all four** even if you only need one for your local runs, so that Evergreen and other developers have correct hashes.

```bash
# Get the SHA256 hash for each platform/architecture combination
curl -fsSL https://mongot-extension.s3.amazonaws.com/latest/mongot-extension-latest-amazon2023-x86_64.tgz | sha256sum
curl -fsSL https://mongot-extension.s3.amazonaws.com/latest/mongot-extension-latest-amazon2023-aarch64.tgz | sha256sum
curl -fsSL https://mongot-extension.s3.amazonaws.com/latest/mongot-extension-latest-amazon2-x86_64.tgz | sha256sum
curl -fsSL https://mongot-extension.s3.amazonaws.com/latest/mongot-extension-latest-amazon2-aarch64.tgz | sha256sum
```

2. Update the corresponding entries in `buildscripts/s3_binary/hashes.py` with the latest hash values:

```python
S3_SHA256_HASHES = {
    # ... other entries ...
    "https://mongot-extension.s3.amazonaws.com/latest/mongot-extension-latest-amazon2023-x86_64.tgz": "<new-hash>",
    "https://mongot-extension.s3.amazonaws.com/latest/mongot-extension-latest-amazon2023-aarch64.tgz": "<new-hash>",
    "https://mongot-extension.s3.amazonaws.com/latest/mongot-extension-latest-amazon2-x86_64.tgz": "<new-hash>",
    "https://mongot-extension.s3.amazonaws.com/latest/mongot-extension-latest-amazon2-aarch64.tgz": "<new-hash>",
}
```

3. Commit the updated `hashes.py` as part of your PR so that Evergreen and others can use the new checksums.

### Using a Custom Extension Binary

For local development or testing with a custom-built `mongot-extension`, you can set the `MONGOT_EXTENSION_PATH` environment variable:

```bash
export MONGOT_EXTENSION_PATH=/path/to/your/mongot-extension.so
buildscripts/resmoke.py run --suites=vector_search_extension_* ...
```

This bypasses the S3 download and instead uses your local binary when generating `mongot-extension.conf`.
