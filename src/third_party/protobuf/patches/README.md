# protobuf patches

These patches are applied by `scripts/import.sh` after cloning the upstream tag.

## Note for future protobuf upgrades

Everything here is **purely build logic** needed to build protobuf 6.31.1 with Bazel 9. None of it
touches protobuf's C++/runtime behavior. All of it has been fixed upstream in newer protobuf
releases, so when protobuf is next re-imported at a version that officially supports Bazel 9,
**re-check whether these patches are still needed and delete the ones that are not** rather than
forward-porting them.

- `0001-bazel-9-rules_cc-loads.patch` — Bazel 9 removed the native `cc_library`/`cc_binary` symbols
  and the built-in `CcInfo`/`cc_common` globals from Starlark. This adds the explicit
  `@rules_cc//cc:...` / `@rules_cc//cc/common:...` loads and swaps `native.cc_library` /
  `native.cc_binary` for the loaded rules.
- `0002-bazel-9-cc-debug-context.patch` — Bazel 9 removed the privileged `CcInfo.debug_context()`
  accessor that protobuf's allowlisted path in `cc_proto_compile_and_link` used, while
  `bazel_features` still reports protobuf as allowlisted. Skips the debug context entirely; the only
  loss is `.dwo`/temps aggregation for generated proto C++ code.
