# Install rules architecture

The install rules build one install action per `mongo_install_rule` target. The action materializes
the target's complete install tree and publishes the corresponding convenience tree used by
`bazel-bin/install`.

```text
inputs and transitive source maps
            │
            ▼
  Bazel analysis: normalize destinations,
  assign owners, reject overlaps
            │
            ▼
  declared outputs + install depfile
            │
            ▼
  one Python install action
       ┌────┴────┐
       ▼         ▼
 action-private  shared staging tree
 output tree     (outside the lock)
                     │
                     ▼
              lock + atomic rename
                     │
                     ▼
              bazel-bin/install
```

## Analysis

`install_rules.bzl` collects every requested destination in an ownership map. Destinations are
normalized and compared case-insensitively. Exact conflicts, file/directory conflicts, and prefix
overlaps such as `lib/tool` versus `lib/tool/config` fail during analysis, before an install action
can race. Repeated references to the same source and destination are deduplicated.

The rule declares the complete output set once and writes a depfile describing binaries, libraries,
root files, and explicitly renamed include files. Dependencies contribute source artifacts and
source maps; child install trees are not republished by aggregate targets.

## Materialization

`install_rules.py` first creates the action-private output tree. Each file is written through a
temporary path and published with `os.replace`:

- Regular files use hardlinks when the filesystem permits them.
- Stable Bazel output symlinks are hardlinked without dereferencing their targets. If a hardlink is
  cross-device, an absolute symlink to the persistent output is used instead.
- Source-tree, relative, or sandbox-local symlinks are copied and dereferenced so the published tree
  cannot dangle when the action sandbox is removed.
- Directories are copied recursively, with symlink-cycle detection.

## Shared convenience tree

The shared tree is configuration-specific and is staged without holding the publication lock. The
lock covers only the final rename(s), so concurrent install actions do not expose partially written
trees and spend minimal time contending. A displaced destination is cleaned after publication; if
publication is interrupted, the previous destination is restored before the error is reported. The
publication path is resolved before use so it does not traverse another action's output directory,
and owner write/search permissions are restored on shared directories before a rename.

Path validation also rejects traversal, absolute paths, invalid Windows separators, trailing dots or
spaces, and Windows DOS device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, and `LPT1`–`LPT9`).

Because the shared convenience tree is outside the declared action outputs, `MongoInstallRule`
disables action caching and remote execution. The wrapper selects the appropriate local or container
strategy and keeps the publication path visible and writable instead of hiding it inside an action
sandbox. Linux native and containerized actions share a host-temp root keyed by the Bazel
output-base name. Container actions receive that root through `MONGO_BAZEL_SHARED_INSTALL_DIR` and
mount it writable, so native fallbacks and container actions publish into the same tree. macOS
cross-host install actions use the same host-temp layout when the action environment does not
contain that variable. After each build, the wrapper points `bazel-bin/install` at the shared root.
Keeping this tree outside the Bazel output-root hierarchy avoids output-directory permissions and
finalization races.
