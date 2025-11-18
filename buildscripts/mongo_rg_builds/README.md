# mongo ripgrep builds

This directory contains scripts to produce **portable, high-performance `ripgrep` binaries** for all major platforms:

- **Linux** (`manylinux2014` glibc 2.17 baseline): `x86_64`, `aarch64`, `s390x`, `ppc64le`
- **macOS** universal2 (`x86_64` + `arm64`)
- **Windows** x86_64 (MSVC)

Each build uses **bundled static PCRE2**, **LTO**, and conservative CPU baselines to maximize portability.  
All artifacts are placed in the `dist/` directory.

---

## 📁 Contents

| Script                        | Platform                                | Output                         |
| :---------------------------- | :-------------------------------------- | :----------------------------- |
| `build_rg_manylinux2014.sh`   | Linux (x86_64, aarch64, s390x, ppc64le) | `dist/rg-manylinux2014-<arch>` |
| `build_rg_macos_universal.sh` | macOS (universal2)                      | `dist/rg-macos-universal2`     |
| `build_rg_windows_x64.ps1`    | Windows (x86_64)                        | `dist/rg-windows-x86_64.exe`   |

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
./build_rg_manylinux2014.sh
```

#### Cross-build via QEMU

```bash
ARCH=x86_64 PLATFORM=linux/amd64   ./build_rg_manylinux2014.sh
ARCH=aarch64 PLATFORM=linux/arm64   ./build_rg_manylinux2014.sh
ARCH=s390x   PLATFORM=linux/s390x   ./build_rg_manylinux2014.sh
ARCH=ppc64le PLATFORM=linux/ppc64le ./build_rg_manylinux2014.sh
```

#### Tune CPU baseline

```bash
CPU_BASELINE=x86-64-v2 ./build_rg_manylinux2014.sh
```

---

### 🍎 macOS (universal2)

```bash
./build_rg_macos_universal.sh
```

- Targets macOS **10.13+** (x86_64) and **11.0+** (arm64).
- Uses `lipo` to merge slices.

---

### 🪟 Windows (x86_64)

Run in **Developer PowerShell for VS** (so `cl.exe` is available):

```powershell
.\build_rg_windows_x64.ps1
```

---

## ⚙️ Build Behavior (All Platforms)

- **Release mode** with `LTO=fat`, `codegen-units=1`, `panic=abort`
- **Bundled static PCRE2** (`PCRE2_SYS_BUNDLED=1`, `PCRE2_SYS_STATIC=1`)
- **CPU baseline:**
  - Linux x86_64 – `x86-64` (override with `CPU_BASELINE`)
  - Other Linux – `generic`
  - macOS – `x86-64` / `generic`
  - Windows – `x86-64`
- **No** `-C lto` in `RUSTFLAGS`; LTO handled via Cargo profile

---

## 🧩 Environment Variables

| Variable                        | Purpose                          | Default                                     |
| :------------------------------ | :------------------------------- | :------------------------------------------ |
| `RG_REPO`                       | Git repo to clone                | `https://github.com/BurntSushi/ripgrep.git` |
| `RG_REF`                        | Branch / tag / commit            | `master`                                    |
| `OUT_DIR`                       | Output directory                 | `./dist`                                    |
| `ARCH`                          | Linux target arch                | `uname -m`                                  |
| `PLATFORM`                      | Docker platform                  | auto                                        |
| `CPU_BASELINE`                  | CPU baseline (Linux/Windows)     | `x86-64`                                    |
| `DEPLOY_X86`                    | macOS min version (x86_64 slice) | `10.13`                                     |
| `DEPLOY_ARM`                    | macOS min version (arm64 slice)  | `11.0`                                      |
| `CPU_BASE_X86` / `CPU_BASE_ARM` | macOS CPU baselines              | `x86-64` / `generic`                        |

## 📜 License & Attribution

These scripts build **ripgrep** from the official upstream repository  
👉 <https://github.com/BurntSushi/ripgrep>

Ripgrep is distributed under the terms of the MIT license.  
PCRE2 is statically linked under its respective license via `pcre2-sys`.
