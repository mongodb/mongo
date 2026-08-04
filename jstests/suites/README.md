## Resmoke suite test targets

Bazel test targets for resmoke suites.

For documentation of the `resmoke_suite_test` rule, see
[bazel/resmoke/README.md](bazel/resmoke/README.md).

## Configuring

In addition to attributes for `resmoke_suite_test`, the following are options for configuring test
targets.

### size

A test's resources are driven by its Bazel `size` attribute, which selects the remote execution pool
it runs on. The same allocation is applied to local execution via `--default_test_resources` in
`.bazelrc`. The mapping is:

| `size`             | Memory | CPUs |
| ------------------ | ------ | ---- |
| `small`            | 3.5 GB | 1    |
| `medium` (default) | 3.5 GB | 1    |
| `large`            | 7 GB   | 2    |
| `enormous`         | 14 GB  | 4    |

```bzl
resmoke_suite_test(
    name = "my_suite",
    size = "large",  # Uses the 7 GB, 2-core pool.
    ...
)
```

### tags

Arbitrary tags may also be added to group test targets for batch execution. For example, a custom
tag lets you run all matching suites at once:

```
bazel test //jstests/suites/... --test_tag_filters=my_tag
```

The following tags have special meaning:

| Tag                                   | Purpose                                                                                                                                                                                                                                                                                                                                                          | Example                                                                                                      |
| ------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ |
| `ci-` tags                            | Configure priority of the task in CI.<br/>Setting one of these enables the test to run in CI. See [task_selection_tags.md](docs/evergreen-testing/yaml_configuration/task_selection_tags.md) for the semantics of each. <br/><br/>One of <br/> `ci-default`<br/>`ci-release-critical`<br/>`ci-development-critical`<br/>`ci-development-critical-single-variant` | `tags = ["ci-default"]`                                                                                      |
| `incompatible_with_bazel_remote_test` | Exclude the test from Bazel remote execution environments (e.g. remote CI executors). Use this when a test relies on resources or environment characteristics that are unavailable on remote executors.                                                                                                                                                          | `tags = ["incompatible_with_bazel_remote_test"],  # Requires openssl, which is missing on remote executors.` |

### target_compatible_with

Configure platforms/build options that the test is compatible with. Use this to exclude the test
suite from platforms in CI.

Example — exclude the test on PPC/S390x, MacOS, and TSAN builds:

```bzl
target_compatible_with = select({
    "@platforms//cpu:ppc64le": ["@platforms//:incompatible"],
    "@platforms//cpu:s390x": ["@platforms//:incompatible"],
    "@platforms//os:macos": ["@platforms//:incompatible"],
    "//bazel/config:tsan_enabled": ["@platforms//:incompatible"],
    "//conditions:default": [], # Otherwise, the test is compatible and can be run.
})
```
