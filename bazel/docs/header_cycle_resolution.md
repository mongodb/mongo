# Header relocation and cycle resolution.

1. Locate all the targets that reference the header file in BUILD.bazel files.
2. Find an ideal target to declare the header under. The is usually under the target that features the .cpp file of the same name. Otherwise, the header can be placed it its own library.
3. Ensure that all the targets that need this header can depend on the target the header was moved to.
4. Run `bazel build //src/...` to see if it builds (check for failures related to dependency cycles).
5. If the build fails because of a dependency cycle, you may need to split up the dependent library or relocate the header.
6. Once the build succeeds, please create a PR and include `devprod-build` for review.
