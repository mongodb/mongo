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
### Set up the virtualenv
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
#### Don’t reinstall what you don’t have to (*NIX only)
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
### `LIBDEPS` and the `LIBDEPS` Linter
#### Why `LIBDEPS`?
Libdeps is a subsystem within the build, which is centered around the LIBrary DEPendency graph. It tracks and maintains the dependency graph as well as lints, analyzes and provides useful metrics about the graph.
#### Different `LIBDEPS` variable types
The `LIBDEPS` variables are how the library relationships are defined within the build scripts. The primary variables are as follows:
* `LIBDEPS`:
  The 'public' type which propagates lower level dependencies onward automatically.
* `LIBDEPS_PRIVATE`:
  Creates a dependency only between the target and the dependency.
* `LIBDEPS_INTERFACE`:
  Same as `LIBDEPS` but excludes itself from the propagation onward.
* `LIBDEPS_DEPENDENTS`:
  Creates a reverse `LIBDEPS_PRIVATE` dependency where the dependency is the one declaring the relationship.
* `PROGDEPS_DEPENDENTS`:
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
python3 -m pip install -r etc/pip/libdeps-requirements.txt
```

After the graph file is created, it can be used as input into the `gacli` tool to perform linting and analysis on the complete dependency graph. The `gacli` tool has options for what types of analysis to perform. A complete list can be found using the `--help` option. Minimally, you can run the `gacli` tool by just passing the graph file you wish to analyze:

```
python3 buildscripts/libdeps/gacli.py --graph-file build/cached/libdeps/libdeps.graphml
```

Another tool which provides a graphical interface as well as visual representation of the graph is the graph visualizer. Minimally, it requires passing in a directory in which any files with the  `.graphml` extension will be available for analysis. By default it will launch the web interface which is reachable in a web browser at http://localhost:3000.

```
python3 buildscripts/libdeps/graph_visualizer.py --graphml-dir build/opt/libdeps
```

For more information about the details of using the post-build linting tools refer to [`post-build linting and analysis`](build_system_reference.md#post-build-linting-and-analysis)
### Debugging build system failures
#### Using` -k` and `-n`
#### `--debug=[explain, time, stacktrace]`
#### `--libdeps-debug`
