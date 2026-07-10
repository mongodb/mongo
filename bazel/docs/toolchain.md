# About

This documents some useful tools, concepts, and debugging strategies for bazel toolchains. This
information was gathered while developing the WASI SDK toolchain.

# Concepts

[Toolchain](https://bazel.build/extending/toolchains#debugging-toolchains) and
[Platform](https://bazel.build/extending/platforms) are the core relevant concepts. Toolchains
define the tools used to compile, and the platform defines either the execution platform (for the
compilation/compiler tools) and target platform (for the binary). Bazel tries to search for a
toolchain based on these constraints.

We also made use of [transitions](https://bazel.build/rules/lib/builtins/transition) which allow
bazel to reconfigure itself before building a target to avoid passing irrelevant or incorrect
compiler flags (e.g. WASI SDK doesn't support shared objects). Similarly, we used
[actions](https://bazel.build/docs/cc-toolchain-config-reference#using-action-config) instead of the
tool paths attribute because of,
[possibly historical, lack of support for remote resources in tool paths](https://stackoverflow.com/questions/73504780/bazel-reference-binaries-from-packages-in-custom-toolchain-definition/73505313#73505313).

# Debugging tools

## Toolchain Debugging

```bash
bazel ... --toolchain_resolution_debug=.* ...
```

The above flag can be used to debug toolchain resolution as bazel tries to automatically satisfy
constraints.

## Debugging Remote Resources

Toolchains may be remotely fetched, but the directory structure of the build environment after these
remote resources are fetched may not be clear. `bazel info` can be used to find the bazel directory
and inspect it `bazel info output_base`. Note: this may be different depending on your configuration
and level of sandboxing.

This is particularly useful when used in combination with the `find` command as shown below.

```bash
find $(bazel info output_base) -name THING
```

Note: this command is directory dependent because output_base is per bazel instance.

## Debugging Bazel Compilation Actions

```bash
bazel ... -s ...
```

This will show verbose output such as cd actions and compiler/linker invocations. Note: bazel may
recast paths relative to the exec directory.

## Debugging on Engflow

Engflow has a lot of helpful views showing remote execution stats and the remote file structure. We
don't intent to duplicate their documentation but be careful as some of their data (particularly
remotely executed actions) may not be accurate immediately after execution.

# Native (system-compiler) toolchain

By default the build uses the hermetic mongo toolchain (a pinned clang/gcc shipped as
`@mongo_toolchain`). The **native toolchain** instead builds against a clang or gcc already
installed on the host, using the system standard library and runtime. This is useful for
experimenting with a compiler version we don't ship, or for reproducing a distro/system-compiler
issue.

> **Note: this is a lightly supported feature.** It is not as a primary build configuration, and it
> is not exercised by most of CI.

## Usage

Point `CC`/`CXX` at the compiler you want and select the matching `--compiler_type`:

```bash
# clang
CC=clang-19 CXX=clang++-19 bazel build \
  --config=native_toolchain --compiler_type=clang --linker=lld install-dist-test

# gcc
CC=gcc-14 CXX=g++-14 bazel build \
  --config=native_toolchain --compiler_type=gcc --linker=lld install-dist-test
```

## Not all compiler versions work

The build only compiles cleanly with a sufficiently modern compiler and standard library — this is
the same modernity the hermetic toolchain pins, and older toolchains will fail.
