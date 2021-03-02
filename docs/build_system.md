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
#### `LIBDEPS` vs `LIBDEPS_PRIVATE vs LIBDEPS_INTERFACE`
#### Reverse edges with `DEPS_DEPENDENTS`
#### The `LIBDEPS` lint rules and tags
#### `LIBDEPS_TAGS`
##### `init-no-global-side-effects`
#### Using the LIBDEPS Linter
### Debugging build system failures
#### Using` -k` and `-n`
#### `--debug=[explain, time, stacktrace]`
#### `--libdeps-debug`
