# Toolchain target sysroots

MongoDB master maintains the base-image definitions for both remote-execution containers and
cross-toolchain sysroots in `repin_dockerfiles.sh`. The `--toolchain-sysroots` profile installs only
runtime-build inputs: `glibc-devel`, `kernel-headers`, `libxcrypt-devel`, and `systemtap-sdt-devel`,
plus their dependencies. It requires a separate output directory and supports RHEL8, RHEL9, and
RHEL10 with amd64, ARM64, PPC64LE, and s390x targets.

Toolchain sysroots have independent Evergreen tasks on the `toolchain-sysroots` variant:

| Task                          | Behavior                                                                                                                                                                          |
| ----------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `publish_toolchain_sysroots`  | Commit-only. Repins recipes, builds temporary local images, validates/exports archives, uploads content-addressed sysroots, and proposes updated pins in a draft PR.              |
| `validate_toolchain_sysroots` | Patch-only. Uses the same recipe/build/validation path, retaining archives, recipes, and a report as private Evergreen patch artifacts. No production publication or pin updates. |

Neither workflow publishes images to ECR or changes remote-execution container pins or
`MODULE.bazel.lock`. Temporary images, containers, and the workflow's builder are removed; other
Docker state is retained. Publication always builds all supported architectures. Both workflows
default to all three distros; no-push validation can restrict architectures for a selected distro.

Archives include target C/kernel/SDT/crypt headers and libraries, with relative symlinks. Debug
build-ID indexes are excluded, and library links through `/etc/alternatives` resolve to their
archived library targets. Validation checks required headers, CRT objects, glibc, libgcc, libcrypt,
the dynamic loader, target ELF machine/endianness, and internal links.

Each archive embeds `sysroot-manifest.json` containing its purpose, distro, architecture, loaded
image's immutable config ID, complete Dockerfile, pullable digest-pinned base image, recipe SHA-256,
installed package versions, and validated paths. The recipe remains available after temporary images
and task workspaces are removed. Archives have normalized timestamps/ownership. Publication uploads
only validated archives under:

```
s3://mdb-build-public/toolchain-sysroots/toolchain-sysroot-<distro>-<architecture>-<sha256>.tar.gz
```

`bazel/toolchains/target_sysroots.json` records URLs, SHA-256 hashes, image IDs, pinned base images,
and recipe hashes. It starts empty; the publication task proposes pins after successful uploads.
Failed runs preserve existing pins. The PR bot uses `validation.json` to identify freshly published
distros, preserving other pending updates in an open PR. Pins updated on master since that PR
branched take precedence over conflicting pending pins, unless their distro was freshly published.
Patch artifacts use signed download links while remaining private.

Toolchain-builder must adopt and verify those pins separately, then use the extracted archive with
GCC's `--with-build-sysroot`. Rebuilding and checking libstdc++ probes remain follow-up work.
Content checks alone do not establish runtime compatibility or explain fortification differences.

## Test without publication

```sh
evergreen patch -p mongodb-mongo-master \
    -v toolchain-sysroots -t validate_toolchain_sysroots \
    --param toolchain_sysroot_distro=rhel8 \
    --param toolchain_sysroot_architectures=ppc64le,s390x -f -y
```

For local validation, run from the repository root:

```sh
python3-venv/bin/python -m bazel.platforms.toolchain_sysroots_generator \
    --no-push --distro rhel8 --architectures ppc64le,s390x \
    --output-dir /tmp/rhel8-toolchain-sysroots --install-binfmt
```

`--install-binfmt` installs QEMU handlers in a privileged disposable container; omit it when
handlers are already installed. `--publish` replaces `--no-push` for publication; it requires S3
credentials and rejects patch builds. The output directory contains archives, generated recipes, and
`validation.json` in both modes.
