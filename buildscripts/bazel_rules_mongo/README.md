# Bazel Rules Mongo

This directory is a bazel rule we use to ship common code between bazel repos

# Using in your repo

1. Look at the latest version in [this](https://github.com/mongodb/mongo/blob/master/buildscripts/bazel_rules_mongo/pyproject.toml) file

2. Get the sha of the latest release at https://mdb-build-public.s3.amazonaws.com/bazel_rules_mongo/{version}/bazel_rules_mongo.tar.gz.sha256

3. Get the link to the latest version at https://mdb-build-public.s3.amazonaws.com/bazel_rules_mongo/{version}/bazel_rules_mongo.tar.gz

4. Add this as a http archive to your repo and implement the dependencies listed in the [WORKSPACE](https://github.com/mongodb/mongo/blob/master/buildscripts/bazel_rules_mongo/WORKSPACE.bazel) file. It will look something like this

```
# Poetry rules for managing Python dependencies
http_archive(
    name = "rules_poetry",
    sha256 = "533a0178767be4d79a67ae43890970485217f031adf090ef28c5c18e8fd337d8",
    strip_prefix = "rules_poetry-092d43107d13e711ac4ac92050d8b570bcc8ef43",
    urls = [
        "https://github.com/mongodb-forks/rules_poetry/archive/092d43107d13e711ac4ac92050d8b570bcc8ef43.tar.gz",
    ],
)

load("@rules_poetry//rules_poetry:poetry.bzl", "poetry")

http_archive(
    name = "bazel_rules_mongo",
    repo_mapping = {"@poetry": "@poetry_bazel_rules_mongo"},
    sha256 = "bb2c2dafc82d905422a12ebef41637b0a1160adffc8a5009dcd1c3d1f81b4056",
    strip_prefix = "bazel_rules_mongo",
    urls = [
        "https://mdb-build-public.s3.amazonaws.com/bazel_rules_mongo/0.1.1/bazel_rules_mongo.tar.gz",
    ],
)

load("@bazel_rules_mongo//codeowners:codeowners_validator.bzl", "codeowners_validator")

codeowners_validator()

load("@bazel_rules_mongo//codeowners:codeowners_binary.bzl", "codeowners_binary")

codeowners_binary()

poetry(
    name = "poetry_bazel_rules_mongo",
    lockfile = "@bazel_rules_mongo//:poetry.lock",
    pyproject = "@bazel_rules_mongo//:pyproject.toml",
)
```

5. Use the rule however you see fit! For example to add `bazel run codeowners` to your repo you can add the following to your root `BUILD.bazel` file

```
alias(
    name = "codeowners",
    actual = "@bazel_rules_mongo//codeowners:codeowners",
)
```

# Deploying

When you are ready for a new version to be released, bump the version in the [pyproject.toml](https://github.com/mongodb/mongo/blob/master/buildscripts/bazel_rules_mongo/pyproject.toml) file.
This will be deployed the next time the `package_bazel_rules_mongo` task runs (nightly). You can schedule this earlier in the waterfall when your pr is merged if you want it quicker.
