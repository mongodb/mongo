# Bazel Developer Workflow

This document describes the Server Developer workflow for modifying Bazel build definitions.

# Creating a new BUILD.bazel file

A build target is defined in the directory where its source code exists. To create a target that compiles **src/mongo/hello_world.cpp**, you would create **src/mongo/BUILD.bazel**.

src/mongo/BUILD.bazel would contain:

    mongo_cc_binary(
        name = "hello_world",
        srcs = [
            "hello_world.cpp"
        ],
    }

Once you've obtained bazel by running **python buildscripts/install_bazel.py**, you can then build this target via "bazel build":

    bazel build //src/mongo:hello_world

Or run this target via "bazel run":

    bazel run //src/mongo:hello_world

The full target name is a combination between the directory of the BUILD.bazel file and the target name:

    //{BUILD.bazel dir}:{targetname}

# Adding a New Header / Source File

Bazel makes use of static analysis wherever possible to improve execution and querying speed. As part of this, source and header files must not be declared dynamically (ex. glob, wildcard, etc). Instead, you'll need to manually add a reference to each header or source file you add into your build target.

    mongo_cc_binary(
        name = "hello_world",
        srcs = [
            "hello_world.cpp",
            "new_source.cpp" # If adding a source file
        ],
        hdrs = [
            "new_header.h" # If adding a header file
        ],
    }

## Adding a New Library

The DevProd Build Team created MongoDB-specific macros for the different types of build targets you may want to specify. These include:

- mongo_cc_binary
- mongo_cc_library
- idl_generator

Creating a new library is similar to the steps above for creating a new binary. A new **mongo_cc_library** definition would be created in the BUILD.bazel file.

    mongo_cc_library(
        name = "new_library",
        srcs = [
            "new_library_source_file.cpp"
        ]
    }

## Declaring Dependencies

If a library or binary depends on another library, this must be declared in the **deps** section of the target. The syntax for referring to the library is the same syntax used in the bazel build/run command.

    mongo_cc_library(
        name = "new_library",
        # ...
    }

    mongo_cc_binary(
        name = "hello_world",
        srcs = [
            "hello_world.cpp",
        ],
        deps = [
            ":new_library", # if referring to the library declared in the same directory as this build file
            # "//src/mongo:new_library" # absolute path
            # "sub_directory:new_library" # relative path of a subdirectory
        ],
    }

## Running clang-tidy via Bazel

Note: This feature is still in development; see https://jira.mongodb.org/browse/SERVER-80396 for details)

To run clang-tidy via Bazel, do the following:

1. To analyze all code, run `bazel build --config=clang-tidy src/...`
2. To analyze a single target (e.g.: `fsync_locked`), run the following command (note that `_with_debug` suffix on the target): `bazel build --config=clang-tidy src/mongo/db/commands:fsync_locked_with_debug`

Testing notes:

- If you want to test whether clang-tidy is in fact finding bugs, you can inject the following code into a `cpp` file to generate a `bugprone-incorrect-roundings` warning:

```
const double f = 1.0;
const int foo = (int)(f + 0.5);
```

# Frequently Asked Questions

### The header I want to add is referenced in several places, how do I figure out where to add references to it in the BUILD.bazel files?

Follow this loop to figure out where the header needs to be added

1. Build directly with bazel to speed up the loop: `bazel build //src/...`
2. This will fail on the first missing header dependency, search the bazel build files for the library the header is defined on. Currently there are cases where headers are incorrectly located so you'll need to use your best judgement. If the header exists on some library, add that library as a dep, for example `scoped_timer.h` is part of `scope_timer` library so add `//src/mongo/db/exec:scoped_timer` to deps field (this will take care of `scoped_timer.h` transitive dependencies). If not add the header directly to the hdrs field of the library that's failing to compile.
3. Build directly with bazel `bazel build //src/...`
4. If there is a cycle remove the dependency from Step #2, add the header as direct dependency to the hdrs field, and then start back at Step #1

### The header I want to add is referenced in dozens or more locations, and adding it to the proper location requires a large refactor that is blocking critical work, what should I do?

If you've put in a significant amount of work to try to get a header added and have found to get it added to the right place (usually alongside the associated .cpp file, having all dependents add that library as a dep) will take a significant refactor, create a SERVER ticket explaining the problem, solution, and complexity required to resolve it. Then, open up src/mongo/BUILD.bazel and add the header to "core_headers" file group referencing your ticket in a TODO comment.

This is very much a last resort and should only be done if the refactor will take a very significant amount of time and is blocking other work.
