# Windows Cross-Builds Through Host Cross RBE

This mode is for Windows hosts that want Bazel to produce Windows x86_64 artifacts with the hermetic
Windows cross toolchain. Bazel runs on the Windows host, C++ and generated-source actions run on
Linux RBE through the cross toolchain wrapper entry points, and `bazel run` for test targets
executes the resulting Windows binary from the local workspace.

## Usage

From a Windows shell that can run `tools/bazel`:

```powershell
bazel build install-dist-test
.\bazel-bin\install-dist-test\bin\mongod.exe --version
```

Windows builds use the original native MSVC toolchain by default. To opt into the experimental
host-cross integration, pass `--config=windows-cross-x86_64` explicitly:

```powershell
bazel build --config=windows-cross-x86_64 install-dist-test
```

To make build-oriented commands such as `build`, `fetch`, `cquery`, and `aquery` default to cross
mode for the current shell, set:

```powershell
$env:MONGO_WINDOWS_CROSS_DEFAULT_CONFIG="1"
```

With that opt-in, `bazel run` also defaults to Windows cross mode for test targets, builds them
through the cross wrapper/RBE path, and runs the produced `.exe` on the Windows host:

```powershell
bazel run +stdx_test
```

To bypass the host-cross integration even when the config is explicit:

```powershell
$env:MONGO_BAZEL_USE_HERMETIC_CONTAINER="0"
```

`bazel test` is not auto-routed through Windows cross mode. For non-test `bazel run` targets, build
the target and run the downloaded `.exe` from PowerShell.

## Required Toolchain Inputs

The cross toolchain is hermetic: Bazel actions must see the same declared Linux LLVM, Windows SDK,
and MSVC-compatible headers/import libraries locally and remotely.

LLVM must be executable inside the Linux RBE/action container. Provide it as an archive or an
unpacked directory:

```powershell
$env:MONGO_WINDOWS_CROSS_LLVM_URL="https://.../llvm-windows-cross-linux-x86_64.tar.xz"
$env:MONGO_WINDOWS_CROSS_LLVM_SHA256="<sha256>"
```

```powershell
$env:MONGO_WINDOWS_CROSS_LLVM_PATH="C:\toolchains\llvm-windows-cross-linux-x86_64"
```

On Windows hosts, the wrapper creates the Windows sysroot automatically when Windows cross mode is
used and `MONGO_WINDOWS_CROSS_SYSROOT_PATH` is not already set. It reads the same pins used by
native Windows builds:

```text
BAZEL_VS
BAZEL_VC
BAZEL_VC_FULL_VERSION
BAZEL_WINSDK_FULL_VERSION
```

The generated sysroot is cached under:

```text
.tmp/hermetic_container/windows-sysroots/msvc-<version>_winsdk-<version>_x64_<hash>
```

The wrapper fails before Docker starts if the exact pinned MSVC toolset, ATL/MFC component, or
Windows SDK is missing. For non-standard SDK installs, set `MONGO_WINDOWS_CROSS_WINSDK_ROOT` to the
Windows Kits `10` directory.

If you need to provide a sysroot manually, set:

```powershell
$env:MONGO_WINDOWS_CROSS_SYSROOT_PATH="C:\toolchains\windows-cross-sysroot"
```

If a packaged sysroot archive is available, provide it instead. When both URL and SHA are set, the
wrapper leaves sysroot setup to the Bazel repository rule and does not generate a host-local
sysroot:

```powershell
$env:MONGO_WINDOWS_CROSS_SYSROOT_URL="https://.../windows-cross-sysroot.tar.xz"
$env:MONGO_WINDOWS_CROSS_SYSROOT_SHA256="<sha256>"
```

The sysroot must be normalized to this layout:

```text
msvc/include/**
msvc/lib/x64/**
msvc/atlmfc/include/**
msvc/atlmfc/lib/x64/**
winsdk/include/ucrt/**
winsdk/include/shared/**
winsdk/include/um/**
winsdk/include/winrt/**
winsdk/include/cppwinrt/**
winsdk/lib/ucrt/x64/**
winsdk/lib/um/x64/**
```

If the Windows host cannot execute the downloaded Linux Bazel binary path inside Docker for an
explicit full-container diagnostic run, set `MONGO_HERMETIC_CONTAINER_CONTAINER_BAZEL` to a Bazel
binary already present in the container, or set `MONGO_HERMETIC_CONTAINER_LINUX_BAZEL` to a Linux
Bazel binary on the host that Docker can mount.

Automatic Linux Bazel downloads are stored atomically and verified against the artifact's `.sha256`
sidecar. If a custom `MONGO_HERMETIC_CONTAINER_LINUX_BAZEL_URL` mirror does not publish that
sidecar, set `MONGO_HERMETIC_CONTAINER_LINUX_BAZEL_SHA256` to the trusted 64-character SHA-256
digest.

## WSL2 Docker Engine

Windows cross builds need Linux containers. Docker Desktop may work on desktop Windows, but Windows
Server native Docker/Mirantis runtimes only run Windows containers and cannot run the Linux RBE
image used by the cross action wrappers.

The supported Windows Server path is Docker Engine inside a WSL2 Linux distribution. This requires
the host or VM to expose virtualization/nested virtualization. If `wsl --install` fails with
`HCS_E_HYPERV_NOT_INSTALLED`, enable Hyper-V or nested virtualization outside the repo.

Once WSL2 can start, install a distro and Docker Engine:

```powershell
wsl --install -d Ubuntu-24.04
```

Inside the distro:

```bash
sudo apt-get update
sudo apt-get install -y docker.io
sudo mkdir -p /mnt/z
sudo mount -t drvfs Z: /mnt/z
sudo dockerd -H unix:///var/run/docker.sock -H tcp://127.0.0.1:2375
```

Keep `dockerd` running, then build from PowerShell:

```powershell
bazel build +stdx_test
```

Windows cross mode defaults to WSL Docker path mapping. It sets Docker host-side hermetic_container
state as if `MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE=wsl` were provided, using
`tcp://127.0.0.1:2375` by default. If your WSL distro uses a different drive mount root, set
`MONGO_HERMETIC_CONTAINER_WSL_DRIVE_MOUNT_PREFIX`. If your Docker Engine listens somewhere else, set
`DOCKER_HOST` or `MONGO_HERMETIC_CONTAINER_WSL_DOCKER_HOST`. If the Windows Docker client is newer
than the WSL Docker daemon, override the default Docker API with
`MONGO_HERMETIC_CONTAINER_WSL_DOCKER_API_VERSION`.
