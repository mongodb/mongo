# Bazel Rules Mongo

This directory is a bazel rule we use to ship common code between bazel repos

## Python dependencies

This package has no lockfile of its own. Its python deps (`pyyaml`, `retry`, `gitpython`,
`requests`, `structlog`) live in the mongo repo's top-level `pyproject.toml` / `uv.lock` as the
`bazel_rules_mongo` dependency group.

The BUILD files here reference python deps through the `dependency()` shim in `bazel/uv/defs.bzl`,
which emits labels of the form `@poetry//:<pkg>`. `@poetry` is deliberately abstract: it is expected
to be repo-mapped by the consuming workspace onto a hub repo that provides one
`py_library`-compatible target per package, named by PEP 503 normalized distribution name
(`@<hub>//:pyyaml`, `@<hub>//:gitpython`, ...).

- Inside the mongo repo this happens automatically: `//WORKSPACE.bazel` loads this directory as a
  `local_repository` with `repo_mapping = {"@poetry": "@pypi"}`, pointing it at the
  rules_pycross-generated hub built from the top-level `uv.lock`.
- Standalone consumers must provide an equivalent hub themselves (see below).

# Using in your repo

1. Look at the latest version in
   [this](https://github.com/mongodb/mongo/blob/master/buildscripts/bazel_rules_mongo/version.txt)
   file

2. Get the sha of the latest release at
   https://mdb-build-public.s3.amazonaws.com/bazel_rules_mongo/{version}/bazel_rules_mongo.tar.gz.sha256

3. Get the link to the latest version at
   https://mdb-build-public.s3.amazonaws.com/bazel_rules_mongo/{version}/bazel_rules_mongo.tar.gz

4. Wire it into your `WORKSPACE.bazel`. You must create a pip hub containing this package's python
   deps and map `@poetry` onto it:

```python
http_archive(
    name = "rules_python",
    sha256 = "<...>",
    strip_prefix = "rules_python-<version>",
    urls = ["https://github.com/bazel-contrib/rules_python/releases/download/<version>/rules_python-<version>.tar.gz"],
)
load("@rules_python//python:repositories.bzl", "py_repositories", "python_register_toolchains")
load("@rules_python//python:pip.bzl", "pip_parse")
py_repositories()
python_register_toolchains(name = "python3_13", python_version = "3.13")

http_archive(
    name = "bazel_rules_mongo",
    # The BUILD files in the archive reference `@poetry//:<pkg>`; point that
    # at the hub declared below.
    repo_mapping = {"@poetry": "@pypi_bazel_rules_mongo"},
    sha256 = "<...>",
    strip_prefix = "bazel_rules_mongo",
    urls = ["https://mdb-build-public.s3.amazonaws.com/bazel_rules_mongo/{version}/bazel_rules_mongo.tar.gz"],
)

# Hub with bazel_rules_mongo's python deps. Write a requirements lock in your
# own repo containing: pyyaml, retry, gitpython, requests, structlog
# (pin versions however you see fit; see the `bazel_rules_mongo` dependency
# group in the mongo repo's top-level pyproject.toml for known-good ranges).
pip_parse(
    name = "pypi_bazel_rules_mongo",
    python_interpreter_target = "@python3_13_host//:python",
    requirements_lock = "//:bazel_rules_mongo_requirements.txt",
)
load("@pypi_bazel_rules_mongo//:requirements.bzl",
     install_pypi_bazel_rules_mongo_deps = "install_deps")
install_pypi_bazel_rules_mongo_deps()

load("@bazel_rules_mongo//codeowners:codeowners_validator.bzl", "codeowners_validator")
codeowners_validator()
load("@bazel_rules_mongo//codeowners:codeowners_binary.bzl", "codeowners_binary")
codeowners_binary()
```

Note: `pip_parse` generates per-package repos (`@pypi_bazel_rules_mongo_<pkg>`) plus a hub whose
target layout depends on your rules_python version. The `@poetry//:<pkg>` labels this package emits
expect root-level targets named by normalized package name; if your hub exposes a different shape
(e.g. `//<pkg>:pkg`), add a small wrapper repo of `alias()` targets and map `@poetry` to that
instead.

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
