# Custom Builds

Custom builds are CI jobs that produce non-standard release artifacts for a specific consumer (for
example, a community build that links jemalloc instead of tcmalloc). They differ from normal
release variants in that they:

- set `MONGO_VERSION_OVERRIDE` so the produced binaries carry a version string with a
  consumer-specific suffix (for example `6.0.29-mcas-jemalloc`),
- apply consumer-specific modifications at build time from patch files (see below),
- upload the resulting dist tarball to a consumer-specific location in addition to the standard
  `mciuploads` task artifacts.

This is a port of the custom build framework from newer branches (see SERVER-133644) to this
SCons-based branch. The OpenSSL 3 mechanisms from the newer branches
(`mongo_openssl_root`/`custom_openssl3_*`, `build_openssl3`) are not ported here because no custom
build on this branch needs them; `custom_ld_library_path` in `evergreen/prelude.sh` is available
for future builds that do need a runtime library path.

## How it works

Custom builds do not define their own compile task. Instead, the shared compile flow contains
custom-build hooks that are no-ops unless the variant sets the `custom_build_*` expansions:

1. The `archive_dist_test` and `package` tasks run `evergreen/custom_builds/apply_patches.sh`,
   which applies the variant's patch files before compiling (gated on `custom_build_patch_files`).
2. The shared `"scons compile"` function runs
   `evergreen/custom_builds/set_version_override.sh`, which computes `MONGO_VERSION_OVERRIDE` from
   `custom_build_base_version` plus `custom_build_version_suffix` (for example
   `r6.0.29-mcas-jemalloc`) and regenerates the version expansions locally in the same step,
   instead of using only the shared per-version expansions (which do not know about the override).
   The hook lives in the shared function rather than in `archive_dist_test` because every compile
   task (`package`, `archive_dist_test_debug`, the dbtest compiles, ...) must apply the override
   itself, or its binaries would silently carry the standard patch/commit version. When
   `custom_build_version_suffix` is unset the script writes a harmless empty override file so the
   unconditional `expansions.update` that follows is a no-op.
3. The `package` task builds and uploads the dist tarball to the standard `mciuploads` dist
   location as usual.

Because every test task depends on `archive_dist_test`, the binaries under test are the exact
binaries being delivered. The `custom_build_deliver` task in this directory (defined in
`tasks.yml`) then fetches the dist tarball from `mciuploads`, smoke-tests it with
`evergreen/custom_builds/verify_dist.sh` (the binary runs, reports the suffixed version, and
reports the allocator in `custom_build_verify_allocator` if set), and uploads it to the consumer's
S3 bucket. Commit builds deliver to
`<custom_build_upload_bucket>/<custom_build_upload_path_prefix>/<src_suffix>/mongodb-<push_arch>.tgz`;
patch builds are redirected to a patch-specific path in the same bucket so verification patches
never overwrite the consumer-facing artifact.

## Copybara rule

Everything under `etc/evergreen_yml_components/` is excluded from the copybara sync to the public
repository, so task/variant definitions and patch files for custom builds live here. A custom build
must **not** land consumer-specific modifications in any file that copybara syncs publicly
(`buildscripts/**` outside of `buildscripts/private/**`, `evergreen/**`, `src/**`, ...). If the
build requires such modifications, put them in a patch file under `patches/` and have the build
apply it before compiling; the `evergreen/custom_builds/apply_patches.sh` helper does this via
`git apply`.

The helper scripts in `evergreen/custom_builds/` are synced publicly and must therefore stay fully
generic: all consumer-specific values are passed in through variant expansions.

### Generic mechanisms available to custom builds

- `custom_build_patch_files`: space-separated list of patch files (paths relative to the repo
  root) applied by `git apply` before compiling. The MCAS build splits its patch into
  `*-third-party.patch` (the vendored jemalloc tree and `src/third_party` wiring), `*-build.patch`
  (the `SConstruct`/`SConscript`/buildscripts wiring), and `*-server.patch` (the server code and
  test changes) so the vendored third-party drop can be reviewed separately from the hand-written
  changes.
- `custom_build_version_suffix` + `custom_build_base_version`: drive `MONGO_VERSION_OVERRIDE`.
- `custom_build_upload_bucket` / `custom_build_upload_path_prefix`: delivery destination.
- `custom_build_verify_allocator` / `custom_build_verify_ssl_lib`: assertions run by
  `verify_dist.sh` before delivery.
- `custom_ld_library_path`: `evergreen/prelude.sh` prepends it to `LD_LIBRARY_PATH` in every
  Evergreen script, so the produced binaries find their libraries at run time.

## Credentials

The delivery upload uses the `aws_key_build`/`aws_secret_build` project variables, matching how
other uploads to `mdb-build-private` authenticate. If the project does not define those variables,
the `s3.put` in `custom_build_deliver` fails; add them to the project before enabling a custom
build variant.
