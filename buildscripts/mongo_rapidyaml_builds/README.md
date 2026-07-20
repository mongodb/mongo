# mongo rapidyaml wheel builds

This directory contains scripts to produce versioned `rapidyaml` wheels that can be uploaded to S3
and consumed directly instead of building from the git dependency in `pyproject.toml`.

The scripts default to the `rapidyaml` commit currently pinned in `pyproject.toml`:

- `a5d485fd44719e1c03e059177fc1f695fc462b66`

They also require `RAPIDYAML_VERSION` to be set explicitly. The MongoDB fork does not currently
publish git tags, so `setuptools-scm` cannot infer a stable release version on its own.

All artifacts are written to `dist/`.

## Contents

| Script                             | Platform                                        | Output                 |
| :--------------------------------- | :---------------------------------------------- | :--------------------- |
| `build_rapidyaml_manylinux2014.sh` | Linux (`x86_64`, `aarch64`, `s390x`, `ppc64le`) | `dist/rapidyaml-*.whl` |
| `build_rapidyaml_macos.sh`         | macOS host arch (`x86_64` or `arm64`)           | `dist/rapidyaml-*.whl` |
| `build_rapidyaml_windows_x64.ps1`  | Windows x86_64                                  | `dist/rapidyaml-*.whl` |

## Quick Start

### Linux (manylinux2014)

Requirements: Docker.

To cross-build non-native Linux architectures via QEMU, enable `binfmt` once:

```bash
docker run --privileged --rm tonistiigi/binfmt --install all
```

Build for the host Linux arch:

```bash
RAPIDYAML_VERSION=0.9.0.post0 ./build_rapidyaml_manylinux2014.sh
```

Cross-build specific Linux wheels:

```bash
RAPIDYAML_VERSION=0.9.0.post0 ARCH=x86_64 PLATFORM=linux/amd64 ./build_rapidyaml_manylinux2014.sh
RAPIDYAML_VERSION=0.9.0.post0 ARCH=aarch64 PLATFORM=linux/arm64 ./build_rapidyaml_manylinux2014.sh
RAPIDYAML_VERSION=0.9.0.post0 ARCH=s390x PLATFORM=linux/s390x ./build_rapidyaml_manylinux2014.sh
RAPIDYAML_VERSION=0.9.0.post0 ARCH=ppc64le PLATFORM=linux/ppc64le ./build_rapidyaml_manylinux2014.sh
```

### macOS

Run the script on each target macOS architecture you want to publish. The script intentionally
builds for the host arch only, which keeps wheel tags and interpreter usage straightforward.

The script creates and uses a temporary virtualenv, so it works with Homebrew-managed Python
installations that reject direct `pip install` into the system environment.

It also leaves `Python.framework` external during delocation, so the wheel should be built with the
same Python distribution family you expect consumers to use.

```bash
RAPIDYAML_VERSION=0.9.0.post0 PYTHON_BIN=python3.13 ./build_rapidyaml_macos.sh
```

### Windows x86_64

Run in Developer PowerShell for Visual Studio so `cl.exe` is available:

```powershell
$env:RAPIDYAML_VERSION = "0.9.0.post0"
$env:PYTHON_BIN = "C:\Python313\python.exe"
.\build_rapidyaml_windows_x64.ps1
```

Note: `pyproject.toml` currently excludes `rapidyaml` on Windows, so a Windows wheel is only needed
if that marker changes later.

## Build Behavior

- The Linux script builds inside the appropriate `manylinux2014` image and runs `auditwheel repair`.
- The macOS script creates a temporary virtualenv, installs its build tooling there, and runs
  `delocate-wheel` while excluding `Python.framework` from bundling.
- The Windows script runs `delvewheel repair` after building.
- Every script clones the `mongodb-forks/rapidyaml` repo, checks out the requested ref, initializes
  submodules, builds a wheel, and performs a simple `import ryml` smoke test.
- Linux defaults to `cp313-cp313`, which matches the repo's current Python version. Override that
  when you need a wheel for a different interpreter.

## Environment Variables

| Variable                   | Purpose                                           | Default                                          |
| :------------------------- | :------------------------------------------------ | :----------------------------------------------- |
| `RAPIDYAML_REPO`           | Git repo to clone                                 | `https://github.com/mongodb-forks/rapidyaml.git` |
| `RAPIDYAML_REF`            | Branch, tag, or commit to build                   | `a5d485fd44719e1c03e059177fc1f695fc462b66`       |
| `RAPIDYAML_VERSION`        | Explicit wheel version passed to `setuptools-scm` | required                                         |
| `OUT_DIR`                  | Output directory                                  | `./dist`                                         |
| `PYTHON_TAG`               | manylinux Python interpreter tag                  | `cp313-cp313`                                    |
| `PYTHON_BIN`               | Host Python executable for macOS or Windows       | `python3` on macOS, `python` on Windows          |
| `ARCH`                     | Target architecture                               | host arch                                        |
| `PLATFORM`                 | Docker platform override for Linux                | auto                                             |
| `CPU_FLAGS`                | Extra C/C++ CPU portability flags                 | platform-specific defaults                       |
| `MACOSX_DEPLOYMENT_TARGET` | macOS minimum OS version                          | `10.13` on x86_64, `11.0` on arm64               |

## Consuming the Wheels

Once the wheels are uploaded, you can replace the current git dependency in `pyproject.toml` with
URL-based entries scoped by platform markers.

For example:

```toml
rapidyaml = { url = "https://your-bucket/rapidyaml/0.9.0.post0/rapidyaml-0.9.0.post0-cp313-cp313-manylinux2014_x86_64.whl", markers = "platform_system == 'Linux' and platform_machine == 'x86_64'" }
```

## Simple-Index Regeneration (`regenerate_simple_index.py`)

The repo consumes `rapidyaml` from the S3-hosted PEP 503 simple index at
`https://mdb-build-public.s3.amazonaws.com/rapidyaml_wheels/simple/` — declared under
`[[tool.poetry.source]]` in `pyproject.toml`:

```toml
[[tool.poetry.source]]
name = "mdb-build-public"
url = "https://mdb-build-public.s3.amazonaws.com/rapidyaml_wheels/simple/"
priority = "explicit"
```

After uploading a new wheel or sdist to `s3://mdb-build-public/rapidyaml_wheels/`, regenerate the
index so every anchor carries a `#sha256=<hex>` fragment:

```bash
# Optional preview (writes both index pages to stdout, no upload):
python3 buildscripts/mongo_rapidyaml_builds/regenerate_simple_index.py --dry-run

# Real thing — rewrites the index in place:
python3 buildscripts/mongo_rapidyaml_builds/regenerate_simple_index.py
```

Requires AWS credentials with `s3:List/Get/Put` on `mdb-build-public`. The script downloads each
artifact once to compute a whole-object sha256 locally (S3's ETag is not a full sha256 for multipart
uploads, so we can't trust it).

**Why the fragments matter:** PEP 503 lets each anchor carry a `#sha256=<hex>` URL fragment. Poetry
(and any other PEP 503-aware installer) uses that fragment to verify downloads and to populate
hashes in the lockfile without re-hashing locally. Without fragments, every `poetry lock` that
touches `rapidyaml` re-downloads each wheel to hash it. Future installers that require indexes to
carry hashes (e.g. `rules_pycross`'s `uv_translator`) will work against this bucket without extra
plumbing once the fragments are in place.

The script writes four S3 keys per run (two byte-identical pairs) so that plain
`s3.amazonaws.com/...` endpoints — which serve keys literally, without the `/` → `/index.html`
rewrite that static-website endpoints do — serve the right content whether the client requests
`.../simple/rapidyaml/` or `.../simple/rapidyaml/index.html`.
