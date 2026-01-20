# mongo gpg builds

This directory contains a script to produce **portable `gpg` binaries** for all our supported linux platforms:

- **Linux** (`manylinux2014` glibc 2.17 baseline): `x86_64`, `aarch64`, `s390x`, `ppc64le`

In particular, it builds gnupg-2.5.16 from source.

This script is used to generate the binaries that we use bring into bazel as a dependency to sign test extensions.
All artifacts are placed in the `dist/` directory.

---

## 📁 Contents

| Script                   | Platform                                | Output                      |
| :----------------------- | :-------------------------------------- | :-------------------------- |
| `build_gpg_manylinux.sh` | Linux (x86_64, aarch64, s390x, ppc64le) | `dist/gpg-manylinux-<arch>` |

---

## 🚀 Quick Start

### 🐧 Linux (manylinux2014 glibc 2.17)

**Requirements:** Docker.  
To cross-build using QEMU (for aarch64/s390x/ppc64le), enable binfmt once:

```bash
docker run --privileged --rm tonistiigi/binfmt --install all
```

#### Build native architecture

```bash
./build_gpg_manylinux.sh
```

#### Cross-build via QEMU

```bash
ARCH=x86_64 PLATFORM=linux/amd64   ./build_gpg_manylinux.sh
ARCH=aarch64 PLATFORM=linux/arm64   ./build_gpg_manylinux.sh
ARCH=s390x   PLATFORM=linux/s390x   ./build_gpg_manylinux.sh
ARCH=ppc64le PLATFORM=linux/ppc64le ./build_gpg_manylinux.sh
```

---

## ⚙️ Build Behavior (All Platforms)

---

## 🧩 Environment Variables

| Variable   | Purpose           | Default    |
| :--------- | :---------------- | :--------- |
| `OUT_DIR`  | Output directory  | `./dist`   |
| `ARCH`     | Linux target arch | `uname -m` |
| `PLATFORM` | Docker platform   | auto       |

## 📜 License & Attribution

These scripts build **gpg** and its required dependencies from sources originally obtained from:
👉 <https://www.gnupg.org/ftp/gcrypt/gnupg/> and <https://gnupg.org/download/index.html>

The exact sources can be obtained at the following URLs:

- https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/sources/gnupg-w32-2.5.16_20251230.tar.xz
- https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/sources/libassuan-3.0.2.tar.bz2
- https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/sources/libgcrypt-1.11.2.tar.bz2
- https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/sources/libgpg-error-1.58.tar.bz2
- https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/sources/libksba-1.6.7.tar.bz2
- https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/sources/npth-1.8.tar.bz2
- https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/sources/ntbtls-0.3.2.tar.bz2
