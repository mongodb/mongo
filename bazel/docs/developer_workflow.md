# Bazel Developer Workflow

This document describes the Server Developer workflow for modifying Bazel build definitions.

# Creating a new BUILD.bazel file

Similar to SCons, a build target is defined in the directory where its source code exists. To create a target that compiles **src/mongo/hello_world.cpp**, you would create **src/mongo/BUILD.bazel**.

The Bazel equivalent of SConscript files are BUILD.bazel files.

src/mongo/BUILD.bazel would contain:

    mongo_cc_binary(
        name = "hello_world",
        srcs = [
    	    "hello_world.cpp"
    	],
    }

Once you've obtained bazelisk by running **evergreen/get_bazelisk.sh**, you can then build this target via "bazelisk build":

    ./bazelisk build //src/mongo:hello_world

Or run this target via "bazelisk run":

    ./bazelisk run //src/mongo:hello_world

The full target name is a combination between the directory of the BUILD.bazel file and the target name:

    //{BUILD.bazel dir}:{targetname}

# Adding a New Header / Source File

Bazel makes use of static analysis wherever possible to improve execution and querying speed. As part of this, source and header files must not be declared dynamically (ex. glob, wildcard, etc). Instead, you'll need to manually add a reference to each header or source file you add into your build target.

The divergence from SCons is that now source files have to be declared in addition to header files.

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

-   mongo_cc_binary
-   mongo_cc_library
-   idl_generator

Creating a new library is similar to the steps above for creating a new binary. A new **mongo_cc_library** definition would be created in the BUILD.bazel file.

    mongo_cc_library(
        name = "new_library",
        srcs = [
    	    "new_library_source_file.cpp"
    	]
    }

## Declaring Dependencies

If a library or binary depends on another library, this must be declared in the **deps** section of the target. The syntax for referring to the library is the same syntax used in the bazelisk build/run command.

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

## Depending on a Bazel Library in a SCons Build Target

During migration from SCons to Bazel, the Build Team has created an integration layer between the two while working towards converting all SCons targets to Bazel targets.

This allows SCons build targets to depend on Bazel build targets directly. The Bazel targets depended on by the SCons build target will be built with the normal scons.py invocation automatically.

    env.BazelLibrary(
        target='fsync_locked',
        source=[
            'fsync_locked.cpp',
        ],
        LIBDEPS=[
            'new_library', # depend on the bazel "new_library" target defined above
    	],
    )
