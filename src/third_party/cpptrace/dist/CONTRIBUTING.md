# Contributing

Welcome, thank you for your interest in the project!

## Getting started

Contributions are always welcome. If you have not already, consider joining the community discord
(linked in the README). There is discussion about library development there as well as a development
roadmap. Github issues are also a good place to start.

I'm happy to merge fixes, improvements, and features as well as help with getting pull requests
(PRs) over the finish line. That being said, I can't merge stylistic changes,
premature-optimizations, or micro-optimizations.

When contributing, please try to match the current code style in the codebase. Style doesn't matter
too much ultimately but consistency within a codebase is important.

Please base changes against the `dev` branch, which is used for development.

## Code organization

The library's public interface is defined in headers in `include/`. Declarations for the public interface have
definitions in .cpp files in the top-level of `src/`. Implementation for various actions such as unwinding, demangling,
symbol resolution, etc. are put in various sub-directories of `src/`.

## Local development

The easiest way to get started with local development is running `make debug` (which automatically configures cmake and
builds). Note: This requires ninja at the moment.

For more control over how the library is built you can manually build with cmake:

`cmake ..` in a `build/` folder along with any cmake configurations you desire. Then run `make -j` or `ninja` or
`msbuild cpptrace.sln`.

Some useful configurations:
- `-DCMAKE_BUILD_TYPE=Debug|Release|RelWithDebInfo`: Build in debug / release / etc.
- `-DBUILD_SHARED_LIBS=On`: Build shared library
- `-DCPPTRACE_SANITIZER_BUILD=On`: Turn on sanitizers
- `-DCPPTRACE_BUILD_TESTING=On`: Build small test and demo program

## Testing

Unfortunately because this library is so platform-dependent the best way to test thoroughly is with
github's CI. This will happen automatically when you open a PR.
