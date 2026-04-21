# Extensions

This module provides utilities for setting up and configuring MongoDB extensions in resmoke test
suites.

## Overview

Extensions are dynamically loaded shared objects (`.so` files) that provide additional functionality
to MongoDB. The utilities in this folder can handle:

1. Discovering extension `.so` files in build directories
2. Generating `.conf` configuration files for extensions
3. Cleaning up configuration files after tests

## Configuration File Generation in Tests

Extension `.conf` files are YAML configuration files that tell the server how to load an extension.
They contain:

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

1. Receives a list of `.so` file paths (either from automatic discovery via
   `find_and_generate_extension_configs.py`, or manually via `--so-files` command-line argument)
2. For each `.so`, creates a `.conf` file in the temp directory (`/tmp/mongo/extensions/`)
3. Looks up corresponding extension options from
   `src/mongo/db/extension/test_examples/configurations.yml`, if any are specified
4. Writes the config file with `sharedLibraryPath` and any `extensionOptions`

### Automatic Discovery and Generation

The `find_and_generate_extension_configs.py` module combines discovery and generation:

1. Searches for `*_mongo_extension.so` files in build directories:
   - **Evergreen:** `dist-test/lib/`
   - **Local:** `bazel-bin/install-dist-test/lib/` or `bazel-bin/install-extensions/lib/`
2. Generates `.conf` files with a unique UUID suffix to avoid collisions
3. Adds the `loadExtensions` parameter to mongod/mongos options
