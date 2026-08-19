# Bazel Rules Mongo

This directory is a bazel rule we use to ship common code between bazel repos

## Python dependencies

This package has no lockfile of its own. Its python deps (`pyyaml`, `retry`, `gitpython`,
`requests`, `structlog`) live in the mongo repo's top-level `pyproject.toml` / `uv.lock` as the
`bazel_rules_mongo` dependency group.

The BUILD files here reference python deps through the `dependency()` shim in `bazel/uv/defs.bzl`,
which emits labels of the form `@pypi//:<pkg>`, where `<pkg>` is the PEP 503 normalized distribution
name (`@pypi//:pyyaml`, `@pypi//:gitpython`, ...). The hub name is the `PYPI_HUB` constant in that
file — that constant is the single seam if your workspace names its hub something else.

This package is a Bzlmod module (see its `MODULE.bazel`) and reaches that hub with:

```python
lock_repos = use_extension("@rules_pycross//pycross/extensions:lock_repos.bzl", "lock_repos")
use_repo(lock_repos, "pypi")
```

`lock_repos` is a single global extension instance, so the hub is populated by whichever module in
the graph imports a lock into a repo named `pypi`. The consuming **root** module must do that.

- Inside the mongo repo this happens automatically: `//MODULE.bazel` declares
  `bazel_dep(name = "bazel_rules_mongo")` + `local_path_override`, and its
  `lock_import.import_uv(repo = "pypi", lock_file = "//:uv.lock", ...)` builds the hub from the
  top-level `uv.lock`.
- Standalone consumers must import an equivalent lock into a repo named `pypi` (see below).

# Using in your repo

1. Look at the latest version in
   [this](https://github.com/mongodb/mongo/blob/master/buildscripts/bazel_rules_mongo/version.txt)
   file

2. Get the sha of the latest release at
   https://mdb-build-public.s3.amazonaws.com/bazel_rules_mongo/{version}/bazel_rules_mongo.tar.gz.sha256

3. Get the link to the latest version at
   https://mdb-build-public.s3.amazonaws.com/bazel_rules_mongo/{version}/bazel_rules_mongo.tar.gz

4. Wire it into your `MODULE.bazel`. This package is a Bzlmod module and is not published to the
   Bazel Central Registry, so use `archive_override`. You must also import a python lock into a hub
   repo named `pypi` containing this package's deps:

```python
bazel_dep(name = "bazel_rules_mongo", version = "0.0.0")
archive_override(
    module_name = "bazel_rules_mongo",
    integrity = "sha256-<...>",  # from the .sha256 URL in step 2, base64-encoded
    strip_prefix = "bazel_rules_mongo",
    urls = ["https://mdb-build-public.s3.amazonaws.com/bazel_rules_mongo/{version}/bazel_rules_mongo.tar.gz"],
)

# The hub this package's BUILD files resolve `@pypi//:<pkg>` against. Your lock
# must contain: pyyaml, retry, gitpython, requests, structlog (see the
# `bazel_rules_mongo` dependency group in the mongo repo's top-level
# pyproject.toml for known-good ranges).
bazel_dep(name = "rules_pycross", version = "0.8.3")

lock_import = use_extension("@rules_pycross//pycross/extensions:lock_import.bzl", "lock_import")
lock_import.import_uv(
    lock_file = "//:uv.lock",
    repo = "pypi",
    target_environments = ["//:environments"],
)

lock_repos = use_extension("@rules_pycross//pycross/extensions:lock_repos.bzl", "lock_repos")
use_repo(lock_repos, "pypi")

codeowners_validator_extension = use_extension("@bazel_rules_mongo//codeowners:codeowners_validator.bzl", "codeowners_validator_extension")
use_repo(codeowners_validator_extension, "codeowners_validator")

codeowners_binary_extension = use_extension("@bazel_rules_mongo//codeowners:codeowners_binary.bzl", "codeowners_binary_extension")
use_repo(codeowners_binary_extension, "codeowners_binary")
```

If you source python deps some other way (rules_python's `pip.parse`, a vendored hub, ...), the hub
must expose one `py_library`-compatible target per package at the repo root, named by PEP 503
normalized distribution name. If yours has a different shape (e.g. `//<pkg>:pkg`), add a small repo
of `alias()` targets, name it `pypi`, or vendor this package and change `PYPI_HUB` in
`bazel/uv/defs.bzl`.

5. Use the rule however you see fit! For example to add `bazel run codeowners` to your repo you can
   add the following to your root `BUILD.bazel` file

```
alias(
    name = "codeowners",
    actual = "@bazel_rules_mongo//codeowners:codeowners",
)
```

# Deploying

When you are ready for a new version to be released, bump the version in the
[version.txt](https://github.com/mongodb/mongo/blob/master/buildscripts/bazel_rules_mongo/version.txt)
file. This will be deployed the next time the `package_bazel_rules_mongo` task runs (nightly). You
can schedule this earlier in the waterfall when your pr is merged if you want it quicker.
