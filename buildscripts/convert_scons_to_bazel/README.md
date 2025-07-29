# The basics

The convert.py script will attempt convert the existing scons build to a bazel build based of masters bazel build infrastructure.

1. first it will checkout the latest build files from master.
2. Several third parties are required to come from master for infrastructure purposes (bzlmod)
3. The script will then glob through the SConscript files and look for easy to convert targets, targets that don't have strange invocation formats. An example of an easy format would be:
    ```
    env.Library(
        target="str_name",
        source=["list", "of", "strings"],
        LIBDEPS=["list", "of", "strings"],
        LIBDEPS_PRIVATE=["list", "of", "strings"],
    )
    ```
4. The script will skip generation for SConscript which already have a BUILD.bazel file.
5. The script will look for special files in the same directory as the SConcript named `converter_callbacks.py`. this file must contain a dictionary name `converter_callbacks` which lists the "scons_name" as the key and what the converted bazel invocation should look like, for example
   `    converter_callbacks = { 
    "some_library": """mongo_cc_library(
    name = "some_library",
    srcs = ["some_source", "some_other_source"],
    deps = ["some_dep"]
)"""
}`
   Note that all callbacks will be written to the generate BUILD.bazel file. The key string is important only for the script to know which automated targets to not generate missing stubs for.

# Using the script

The script is going to modifying files with git so you need to be careful about this fact when using it. The script has two modes, the default **generate** mode and the `--clean` mode. When generating it assumes a clean state, so if iterating make sure to pass clean between generations. The `clean` mode will revert all untracked changes. This means that you can change the generator script freely. If adding a new `converter_callbacks.py` make sure to commit it before cleaning. This also means you can not change the generated BUILD.bazel files as they are untracked.

# Building after the script

After you run the generator, a valid bazel build should exists. Note that most targets will not work, but you can build certain targets which have all the dependencies satisified, for example at the time of writing you can generate and build all the abseil-cpp libraries with the following example:

```
python buildscripts/convert_scons_to_bazel/convert.py
bazel build --skip_archive=False //src/third_party/abseil-cpp:all
```

# Find deps to convert for a unittest

The utilty script `unittest_deps.py` will generate a full list of the unittests ordered by number of deps by passing `--full-list`. It can also print the deps that have not been converted yet for a given unittest by passing `--check-test-deps` <unittest name>, for example:

```
python buildscripts/convert_scons_to_bazel/unittest_deps.py --check-test-deps base_test
```

will output something like:

```
Gathering unittests...
done.
Checking deps for build/opt/mongo/base/base_test
Deps left to convert:
build/opt/mongo/util/libprocessinfo.so
build/opt/mongo/util/libprocparser.so
build/opt/mongo/util/libsecure_zero_memory.so
build/opt/third_party/fmt/libfmt.so
build/opt/third_party/libshim_abseil.so
build/opt/third_party/libshim_allocator.so
build/opt/third_party/libshim_boost.so
build/opt/third_party/libshim_fmt.so
build/opt/third_party/murmurhash3/libmurmurhash3.so
```
