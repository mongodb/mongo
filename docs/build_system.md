# The MongoDB Build System

## Introduction

### System requirements and supported platforms

## How to get Help

### Where to go

### What to bring when you go there (SCons version, server version, SCons command line, versions of relevant tools, `config.log`, etc.)

## Known Issues

### Commonly-encountered issues

#### `--disable-warnings-as-errors`

### Reference to known issues in the ticket system

### How to report a problem

#### For employees

#### For non-employees

## Set up the build environment

### Set up the virtualenv and poetry

See [Building Python Prerequisites](building.md#python-prerequisites)

### The Enterprise Module

#### Getting the module source

#### Enabling the module

## Building the software

### Commonly-used build targets

### Building a standard “debug” build

#### `--dbg`

### What goes where?

#### `$BUILD_ROOT/scons` and its contents

#### `$BUILD_ROOT/$VARIANT_DIR` and its contents

#### `$BUILD_ROOT/install` and its contents

#### `DESTDIR` and `PREFIX`

#### `--build-dir`

### Running core tests to verify the build

### Building a standard “release” build

#### `--separate-debug`

### Installing from the build directory

#### `--install-action`

### Creating a release archive

## Advanced Builds

### Compiler and linker options

#### `CC, CXX, CCFLAGS, CFLAGS, CXXFLAGS`

#### `CPPDEFINES and CPPPATH`

#### `LINKFLAGS`

#### `MSVC_VERSION`

#### `VERBOSE`

### Advanced build options

#### `-j`

#### `--separate-debug`

#### `--link-model`

#### `--allocator`

#### `--cxx-std`

#### `--linker`

#### `--variables-files`

### Cross compiling

#### `HOST_ARCH` and `TARGET_ARCH`

### Using Ninja

#### `--ninja`

### Cached builds

#### Using the SCons build cache

##### `--cache`

##### `--cache-dir`

#### Using `ccache`

##### `CCACHE`

### Using Icecream

#### `ICECC`, `ICECRUN`, `ICECC_CREATE_ENV`

#### `ICECC_VERSION` and `ICECC_VERSION_ARCH`

#### `ICECC_DEBUG`

## Developer builds

### Developer build options

#### `MONGO_{VERSION,GIT_HASH}`

By default, the server build system consults the local git repository
(assuming one exists) to automatically derive the current version of
MongoDB and current git revision that is being built. These values are
recorded in the SCons `MONGO_VERSION` and `MONGO_GIT_HASH`
`Environment` variables, respectively. The value of `MONGO_GIT_HASH`
is just that: the value of the currently checked out git hash. The
value computed for `MONGO_VERSION` is based on the result of `git
describe`, which looks for tags matching the release numbering
scheme. Since `git describe` relies on tags, it is important to ensure
that you periodically synchronize new tags to your local repository
with `git fetch` against the upstream server repository.

While this automated scheme works well for release and CI builds, it
has unfortunate consequences for developer builds. Since the git hash
changes on every commit (whether locally authored or pulled from an
upstream repo), and since by default an abbreviated git hash forms
part of the result of `git describe`, a build after a commit or a pull
will see any target that has a direct or indirect dependency on the
parts of the codebase that care about `MONGO_VERSION` and
`MONGO_GIT_HASH` as out of date. Notably, you will at minimum need to
relink `mongod` and other core server binaries.

It is possible to work around this by manually setting values for
`MONGO_VERSION` and `MONGO_GIT_HASH` on the SCons command
line. However, doing so in a way that results in an accurate value for
`MONGO_VERSION` in particular requires writing shell command
substitutions into your SCons invocation, which isn't very
friendly. The longstanding historical practice of setting
`MONGO_VERSION=0.0.0` was never well-advised, but because of recent
feature compatibility version related work it is no longer safe to do
that at all.

To make it easier for developers to manage these variables in a way
which avoids useless rebuilds, has better ergonomics, and does not run
afoul of FCV management, the server build system provides a variables
file to manage these settings automatically:
`etc/scons/developer_versions.vars` . By using this file, you will get
an unchanging `MONGO_GIT_HASH` value of `unknown`, and a
`MONGO_VERSION` value that is still based on `git describe`, but with
`--abbrev=0` affixed, which will eliminate the dependency on the SHA
of the current commit. Note that you will still observe rebuilds if
you pull a new tag which changes the results of `git describe`, but
this should be a much less frequent event.

You can opt into this variable by adding
`--variables-files=etc/scons/developer_versions.vars` to your SCons
command line, either for direct SCons builds, or when generating
Ninja.

Support for `etc/scons/developer_versioning.vars` has been backported
as far back as MongoDB `v4.0`, so you can safely add this to your
SCons invocations on almost any branch you are likely to find yourself
using.

#### Using sanitizers

##### `--sanitize`

##### `*SAN_OPTIONS`

#### `--dbg` `--opt`

#### `--build-tools=[stable|next]`

### Setting up your development environment

#### `mongo_custom_variables.py`

##### Guidance on what to put in your custom variables

##### How to suppress use of your custom variables

##### Useful variables files (e.g. `mongodbtoolchain`)

#### Using the Mongo toolchain

##### Why do we have our own toolchain?

##### When is it appropriate to use the MongoDB toolchain?

##### How do I obtain the toolchain?

##### How do I upgrade the toolchain?

##### How do I tell the build system to use it?

### Creating and using build variants

#### Using `--build-dir` to separate variant build artifacts

#### `BUILD_ROOT` and `BUILD_DIR`

#### `VARIANT_DIR`

#### `NINJA_PREFIX` and `NINJA_SUFFIX`

### Building older versions

#### Using` git-worktree`

### Speeding up incremental builds

#### Selecting minimal build targets

#### Compiler arguments

##### `-gsplit-dwarf` and `/DEBUG:FASTLINK`

#### Don’t reinstall what you don’t have to (\*NIX only)

##### `--install-action=hardlink`

#### Speeding up SCons dependency evaluation

##### `--implicit-cache`

##### `--build-fast-and-loose`

#### Using Ninja responsibly

#### What about `ccache`?

## Making source changes

### Adding a new dependency

### Linting and Lint Targets

#### What lint targets are available?

#### Using `clang-format`

### Testing your changes

#### How are test test suites defined?

#### Running test suites

#### Adding tests to a suite

#### Running individual tests

## Modifying the buid system

### What is SCons?

#### `SConstruct` and `SConscripts`

#### `Environments `and their `Clone`s

##### Overriding and altering variables

#### `Targets` and `Sources`

#### `Nodes`

##### `File` Nodes

##### `Program` and `Library` Nodes

#### `Aliases`, `Depends` and `Requires`

#### `Builders`

#### `Emitters`

#### `Scanners`

#### `Actions`

#### `Configure` objects

#### DAG walk

#### Reference to SCons documentation

### Modules

#### How modules work

#### The Enterprise module

##### The `build.py` file

#### Adding a new module

### Poetry

#### What is Poetry

[Poetry](https://python-poetry.org/) is a python dependency management system. Poetry tries to find dependencies in [pypi](https://pypi.org/) (similar to pip). For more details visit the poetry website.

#### Why use Poetry

Poetry creates a dependency lock file similar to that of a [Ruby Gemfile](https://bundler.io/guides/gemfile.html#gemfiles) or a [Rust Cargo File](https://doc.rust-lang.org/cargo/guide/cargo-toml-vs-cargo-lock.html). This lock file has exact dependencies that will be the same no matter when they are installed. Even if dependencyA has an update available the older pinned dependency will still be installed. The means that there will be less errors that are based on two users having different versions of python dependencies.

#### Poetry Lock File

In a Poetry project there are two files that determine and resolve the dependencies. The first is [pyproject.toml](../pyproject.toml). This file loosely tells poetry what dependencies and needed and the constraints of those dependencies. For example the following are all valid selections.

1. `dependencyA = "1.0.0" # dependencyA can only ever be 1.0.0`
2. `dependencyA = "^1.0.0" # dependencyA can be any version greater than or equal to 1.0.0 and less than 2.0.0`
3. `dependencyA = "*" # dependencyA can be any version`

The [poetry.lock](../poetry.lock) file has the exact package versions. This file is generated by poetry by running `poetry lock`. This file contains a pinned list of all transitive dependencies that satisfy the requirements in [pyproject.toml](../pyproject.toml).

### `LIBDEPS` and the `LIBDEPS` Linter

#### Why `LIBDEPS`?

Libdeps is a subsystem within the build, which is centered around the LIBrary DEPendency graph. It tracks and maintains the dependency graph as well as lints, analyzes and provides useful metrics about the graph.

#### Different `LIBDEPS` variable types

The `LIBDEPS` variables are how the library relationships are defined within the build scripts. The primary variables are as follows:

-   `LIBDEPS`:
    The 'public' type which propagates lower level dependencies onward automatically.
-   `LIBDEPS_PRIVATE`:
    Creates a dependency only between the target and the dependency.
-   `LIBDEPS_INTERFACE`:
    Same as `LIBDEPS` but excludes itself from the propagation onward.
-   `LIBDEPS_DEPENDENTS`:
    Creates a reverse `LIBDEPS_PRIVATE` dependency where the dependency is the one declaring the relationship.
-   `PROGDEPS_DEPENDENTS`:
    Same as `LIBDEPS_DEPENDENTS` but for use with Program builders.

Libraries are added to these variables as lists per each SCons builder instance in the SConscripts depending on what type of relationship is needed. For more detailed information on theses types, refer to [`The LIBDEPS variables`](build_system_reference.md#the-libdeps-variables)

#### The `LIBDEPS` lint rules and tags

The libdeps subsystem is capable of linting and automatically detecting issues. Some of these linting rules are automatically checked during build-time (while the SConscripts are read and the build is performed) while others need to be manually run post-build (after the the generated graph file has been built). Some rules will include exemption tags which can be added to a libraries `LIBDEPS_TAGS` to override a rule for that library.

The build-time linter also has a print option `--libdeps-linting=print` which will print all issues without failing the build and ignoring exemption tags. This is useful for getting an idea of what issues are currently outstanding.

For a complete list of build-time lint rules, please refer to [`Build-time Libdeps Linter`](build_system_reference.md#build-time-libdeps-linter)

#### `LIBDEPS_TAGS`

`LIBDEPS_TAGS` can also be used to supply flags to the libdeps subsystem to do special handling for certain libraries such as exemptions or inclusions for linting rules and also SCons command line expansion functions.

For a full list of tags refer to [`LIBDEPS_TAGS`](build_system_reference.md#libdeps_tags)

#### Using the post-build LIBDEPS Linter

To use the post-build tools, you must first build the libdeps dependency graph by building the `generate-libdeps-graph` target.

You must also install the requirements file:

```
python3 -m poetry install --no-root --sync -E libdeps
```

After the graph file is created, it can be used as input into the `gacli` tool to perform linting and analysis on the complete dependency graph. The `gacli` tool has options for what types of analysis to perform. A complete list can be found using the `--help` option. Minimally, you can run the `gacli` tool by just passing the graph file you wish to analyze:

```
python3 buildscripts/libdeps/gacli.py --graph-file build/cached/libdeps/libdeps.graphml
```

Another tool which provides a graphical interface as well as visual representation of the graph is the graph visualizer. Minimally, it requires passing in a directory in which any files with the `.graphml` extension will be available for analysis. By default it will launch the web interface which is reachable in a web browser at http://localhost:3000.

```
python3 buildscripts/libdeps/graph_visualizer.py --graphml-dir build/opt/libdeps
```

For more information about the details of using the post-build linting tools refer to [`post-build linting and analysis`](build_system_reference.md#post-build-linting-and-analysis)

### Debugging build system failures

#### Using` -k` and `-n`

#### `--debug=[explain, time, stacktrace]`

#### `--libdeps-debug`
