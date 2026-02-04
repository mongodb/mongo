# Extensions

This module provides utilities for setting up and configuring MongoDB extensions in resmoke test suites.

## Overview

Extensions are dynamically loaded shared objects (`.so` files) that provide additional functionality to MongoDB. The utilities in this folder can handle:

1. Discovering extension `.so` files in build directories
2. Generating `.conf` configuration files for extensions
3. Downloading external extensions (e.g. `mongot-extension`) from S3.
4. Cleaning up configuration files after tests

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
2. For each `.so`, creates a `.conf` file in the temp directory (`/tmp/mongo/extensions/`)
3. Looks up corresponding extension options from `src/mongo/db/extension/test_examples/configurations.yml`, if any are specified
4. Writes the config file with `sharedLibraryPath` and any `extensionOptions`

### Automatic Discovery and Generation

The `find_and_generate_extension_configs.py` module combines discovery and generation:

1. Searches for `*_mongo_extension.so` files in build directories:
   - **Evergreen:** `dist-test/lib/`
   - **Local:** `bazel-bin/install-dist-test/lib/` or `bazel-bin/install-extensions/lib/`
2. Generates `.conf` files with a unique UUID suffix to avoid collisions
3. Adds the `loadExtensions` parameter to mongod/mongos options

## `mongot-extension` Setup

The `setup_mongot_extension.py` script downloads and configures the Rust `mongot-extension` binary for extension $vectorSearch testing.
All `vector_search_extension_*` test suites use this framework to run server Extensions code with the Rust `mongot-extension.so`.

### How It Works

1. Builds the download URL from the **pinned release version** (see `MONGOT_EXTENSION_VERSION` in `setup_mongot_extension.py`).
   We use the **release/** path (e.g. `release/mongot-extension-0.0.0-{platform}-{arch}.tgz`), not **latest/**, so normal mongot-extension pushes do not overwrite the artifact.
   Only **sign-and-publish-release** will change the content when run.
2. Downloads the tarball from S3 and verifies it using the hardcoded SHA256 checksums in `buildscripts/s3_binary/hashes.py`.
3. Extracts the `.so` file and creates a configuration file

### Updating Checksums

The `mongot-extension` binaries are stored in S3 and verified using hardcoded SHA256 checksums in `buildscripts/s3_binary/hashes.py`.
When the mongot-extension team overwrites **release/0.0.0** (by running `sign-and-publish-release`), the content at the same URL changes and the hashes in `hashes.py` must be updated.
This ensures every change is explicitly documented in the commit history and no component changes without an auditable commit.

This cross-repo test infrastructure is temporary for the initial rollout of extension `$vectorSearch`. Long-term, all extension testing will live outside the server repository.

**To update the checksums:**

1. Run the following to download each tarball and print its SHA256. Use **all four** so Evergreen and other developers have the correct hashes.

```bash
for s in amazon2023-x86_64 amazon2023-aarch64 amazon2-x86_64 amazon2-aarch64; do
  url="https://mongot-extension.s3.amazonaws.com/release/mongot-extension-0.0.0-${s}.tgz"
  curl -sL -o "/tmp/mongot-${s}.tgz" "$url"
  echo "$url"
  sha256sum "/tmp/mongot-${s}.tgz" | awk '{print $1}'
done
```

2. In `buildscripts/s3_binary/hashes.py`, replace the four mongot-extension **hash values** with the four printed hashes in the same order:

- `amazon2023-x86_64`
- `amazon2023-aarch64`
- `amazon2-x86_64`
- `amazon2-aarch64`

3. Commit the updated `hashes.py` so Evergreen and others use the new checksums.

### Using a Custom Extension Binary

For local development or testing with a custom-built `mongot-extension`, you can set the `MONGOT_EXTENSION_PATH` environment variable:

```bash
export MONGOT_EXTENSION_PATH=/path/to/your/mongot-extension.so
buildscripts/resmoke.py run --suites=vector_search_extension_* ...
```

This bypasses the S3 download and instead uses your local binary when generating `mongot-extension.conf`.
