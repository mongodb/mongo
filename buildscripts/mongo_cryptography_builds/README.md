# mongo cryptography wheel builds

This directory contains tooling to produce `cryptography` wheels for the Linux architectures that
the upstream `cryptography` project does **not** publish wheels for on PyPI — currently **`s390x`**
and **`ppc64le`**.

The repo's build/test path needs `cryptography` on every architecture because
`//x509:generate_main_certificates` (the Bazel target that generates the SSL test certificates under
`dist-test/bin/x509/`) calls `x509/mkcert.py`, which imports the `cryptography` package. Without
`cryptography` available, that rule cannot run, and any test that consumes the generated certs (e.g.
`jstests/core/testing/certs_are_generated.js`, the entire `jstests/client_encrypt/` directory) fails
on s390x / ppc64le.

The wheels produced here are uploaded to `s3://mdb-build-public/cryptography_wheels/simple/` and
consumed by `pip.parse` via the `[[tool.uv.index]]` and `[tool.uv.sources]` entries in the top-level
`pyproject.toml` — the same shape used for `rapidyaml` (see `buildscripts/mongo_rapidyaml_builds/`).

## Why pre-built wheels and not sdist + Rust-in-Bazel

`cryptography` 35+ ships a Rust extension. PyPI carries an `sdist` (`cryptography-44.0.2.tar.gz`)
that builds via `maturin` / `setuptools-rust`, requiring Rust ≥ 1.65 to be available to the build
backend. We can't easily make Rust available inside `pip.parse`'s hermetic build sandbox
(`whl_library` repository rule) — it would require either an in-tree Rust toolchain wired through
`rules_python`, or env-var passthroughs that defeat the sandbox. Pre-building wheels in a
`manylinux_2_28_<arch>` container (with rustup installed) sidesteps all of that: `pip.parse` just
downloads the wheel like any other.

## Why one wheel per arch (and not per Python version)

`cryptography` builds **abi3** wheels (`cp37-abi3-…`), which means a single wheel built against the
Python 3.7 stable ABI works for every Python 3.7+ consumer. So we need exactly two wheels in total:

- `cryptography-<ver>-cp37-abi3-manylinux_2_28_s390x.whl`
- `cryptography-<ver>-cp37-abi3-manylinux_2_28_ppc64le.whl`

## Why `manylinux_2_28` and not `manylinux2014`

We used to target `manylinux2014` (CentOS 7 base, glibc 2.17). It didn't work on s390x / ppc64le for
two reasons:

1. `pypa/manylinux2014_s390x` and `pypa/manylinux2014_ppc64le` don't ship a pre-built OpenSSL — pypa
   CI infra can't cleanly cross-build it for those archs — so `pkg-config openssl` returns nothing
   and `openssl-sys` fails the build with _"Could not find OpenSSL installation"_.
2. The s390x image's `clefos-rh` yum repo (`mirrors.sinenomine.net`) is the only RHEL 7 extras
   rebuild that works for that arch and is frequently slow or unreachable, so we can't reliably
   install OpenSSL on the fly inside the container either.

`manylinux_2_28_<arch>` is based on AlmaLinux 8 (current RHEL 8 rebuild), uses dnf with
mirror.almalinux.org (stable), and lets us `dnf install openssl-devel libffi-devel` to get headers
in seconds. This is also the image the upstream cryptography project uses for their PyPI
`manylinux_2_28_{x86_64,aarch64,armv7l}` wheels, so we're mirroring their own build environment.

The trade-off: `manylinux_2_28` requires glibc ≥ 2.28 on the consuming host. The relevant CI distros
all satisfy that:

| Distro                                      | glibc |
| :------------------------------------------ | :---- |
| rhel83-zseries-small / rhel83-zseries-large | 2.28  |
| rhel81-power8-small / rhel81-power8-large   | 2.28  |
| rhel9-zseries-_ / rhel9-power-_             | 2.34  |

## Contents

| Script                            | Platform                                                                              | Output                    |
| :-------------------------------- | :------------------------------------------------------------------------------------ | :------------------------ |
| `build_cryptography_manylinux.sh` | Linux (`s390x`, `ppc64le`; also works on `x86_64` / `aarch64` if you want to re-host) | `dist/cryptography-*.whl` |

## Prerequisites

- Docker.
- For cross-building non-native architectures via QEMU, enable `binfmt` once on the host:

  ```bash
  docker run --privileged --rm tonistiigi/binfmt --install all
  ```

## Building

### s390x

```bash
CRYPTOGRAPHY_VERSION=44.0.2 ARCH=s390x PLATFORM=linux/s390x \
    ./build_cryptography_manylinux.sh
```

### ppc64le

```bash
CRYPTOGRAPHY_VERSION=44.0.2 ARCH=ppc64le PLATFORM=linux/ppc64le \
    ./build_cryptography_manylinux.sh
```

The wheels land in `dist/`. The script runs an `import cryptography; print(__version__)` smoke test
inside the container before declaring success.

## Uploading to S3

The wheels are consumed from `s3://mdb-build-public/cryptography_wheels/simple/`. Upload with
appropriate AWS credentials:

```bash
aws s3 cp dist/cryptography-44.0.2-cp37-abi3-manylinux_2_28_s390x.whl \
    s3://mdb-build-public/cryptography_wheels/cryptography-44.0.2-cp37-abi3-manylinux_2_28_s390x.whl
aws s3 cp dist/cryptography-44.0.2-cp37-abi3-manylinux_2_28_ppc64le.whl \
    s3://mdb-build-public/cryptography_wheels/cryptography-44.0.2-cp37-abi3-manylinux_2_28_ppc64le.whl
```

Then refresh the index page at `s3://mdb-build-public/cryptography_wheels/simple/index.html`. The
existing `rapidyaml_wheels/simple/` listing
(<https://mdb-build-public.s3.amazonaws.com/rapidyaml_wheels/simple/>) is a static HTML file with
one `<a>` per wheel — generate the same shape for cryptography. A minimal template:

```html
<!doctype html>
<html>
  <body>
    <a
      href="https://mdb-build-public.s3.amazonaws.com/cryptography_wheels/cryptography-44.0.2-cp37-abi3-manylinux_2_28_s390x.whl"
      >cryptography-44.0.2-cp37-abi3-manylinux_2_28_s390x.whl</a
    ><br />
    <a
      href="https://mdb-build-public.s3.amazonaws.com/cryptography_wheels/cryptography-44.0.2-cp37-abi3-manylinux_2_28_ppc64le.whl"
      >cryptography-44.0.2-cp37-abi3-manylinux_2_28_ppc64le.whl</a
    ><br />
  </body>
</html>
```

## Wiring up consumption (`pyproject.toml`)

Once the wheels exist at the URL above, replace the current platform-marker exclusion with a custom
index. **This is the change that re-enables `//x509:generate_main_certificates` on s390x/ppc64le —
don't make this change before the wheels are uploaded, or `uv lock` / `pip.parse` will fail.**

```toml
# In pyproject.toml's `platform` dependency-group, change:
#
#   "cryptography>=44.0.2,<45 ; platform_machine != 's390x' and platform_machine != 'ppc64le'",
#
# to:

"cryptography>=44.0.2,<45",
```

```toml
# And add (next to the existing rapidyaml-style entries near the bottom
# of the file):

[[tool.uv.index]]
name = "mongodb-cryptography"
url = "https://mdb-build-public.s3.amazonaws.com/cryptography_wheels/simple/"
explicit = true

[tool.uv.sources]
# `marker` restricts the override to the archs that need it. On other
# archs uv falls back to PyPI (where the manylinux wheels exist).
cryptography = [
    { index = "mongodb-cryptography", marker = "platform_machine == 's390x' or platform_machine == 'ppc64le'" },
]
```

Then refresh both lockfiles:

```bash
uv lock
bazel run //bazel/uv:export
```

## Reverts to make in the same follow-up

Once the wheels are uploaded and the `pyproject.toml` changes above are in, the surrounding
workarounds can all come out:

1. **`BUILD.bazel`** — drop the `":linux_s390x": {}` / `":linux_ppc64le": {}` branches in the
   `root_files = select({...})` of the `devcore`, `dist-test`, and `dist-test-debug` install rules
   (search for "omit x509 cert generation on s390x / ppc64le"). The `:linux_s390x` and
   `:linux_ppc64le` `config_setting`s at the top of the file can stay — nothing else uses them now,
   but they're cheap and may be useful later.
2. **`jstests/core/testing/certs_are_generated.js`** — remove `incompatible_s390x` and
   `incompatible_ppc` from the `@tags` block.
3. **`etc/evergreen_yml_components/tasks/resmoke/server_divisions/clusters_and_integrations/tasks.yml`**
   — remove `"incompatible_s390x"` and `"incompatible_ppc"` from the `client_encrypt` task's `tags:`
   list.
4. **`evergreen/functions/venv_setup.sh`** — the rustup-toolchain-install block under the
   `s390x | ppc64le)` case becomes redundant once the venv side of `cryptography` can pull a binary
   wheel too. Leave it for now (it doesn't hurt and protects against future sdist deps); revisit
   during cleanup.

## Bumping the cryptography version

When upgrading the pin in `pyproject.toml`, rebuild and re-upload both wheels first, then bump the
version. The wheel filenames embed the version, so old and new wheels can coexist in the bucket.

## Environment variables

| Variable               | Purpose                                                                              | Default           |
| :--------------------- | :----------------------------------------------------------------------------------- | :---------------- |
| `CRYPTOGRAPHY_VERSION` | Wheel version to build                                                               | required          |
| `RUST_VERSION`         | rustup toolchain pin used inside the container                                       | `1.74.0`          |
| `PYTHON_TAG`           | manylinux Python interpreter tag (only affects who invokes the build; wheel is abi3) | `cp313-cp313`     |
| `ARCH`                 | Target architecture (`s390x`, `ppc64le`, `x86_64`, `aarch64`)                        | host arch         |
| `PLATFORM`             | Docker `--platform` override                                                         | unset (host arch) |
| `OUT_DIR`              | Output directory                                                                     | `./dist`          |
