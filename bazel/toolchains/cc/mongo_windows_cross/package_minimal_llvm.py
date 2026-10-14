"""Packages the subset of an official LLVM release that the Windows cross toolchain uses.

The official LLVM Linux release archive is 1.6GB compressed and 8.2GB unpacked, but the Windows
cross toolchain only runs a handful of its tools. This script repackages those tools, the clang
builtin headers, and the LLVM license into a small archive that downloads and unpacks quickly.

The output contains only regular files. LLVM's multicall tools (clang, lld, llvm-ar) are stored
once under their real binary names; the toolchain repository rule links the driver names
(clang-cl, lld-link, llvm-lib) to them, and the tools pick their mode from that name. Keeping
symlinks out of the archive matters because Windows clients unpack it too.

Usage:
    python3 package_minimal_llvm.py \\
        --archive LLVM-19.1.7-Linux-X64.tar.xz \\
        --sha256 4a5ec53951a584ed36f80240f6fbf8fdd46b4cf6c7ee87cc2d5018dc37caf679 \\
        --llvm-version 19.1.7 \\
        --output llvm-19.1.7-windows-cross-x86_64-linux.tar.xz
"""

import argparse
import hashlib
import io
import lzma
import os
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

# The tools the toolchain runs, and the diagnostics tools used to symbolize Windows binaries
# offline (llvm-symbolizer and llvm-pdbutil read PDBs natively, without dbghelp).
TOOLS = [
    "clang",
    "lld",
    "llvm-ar",
    "llvm-rc",
    "llvm-symbolizer",
    "llvm-pdbutil",
]

LICENSE_URL = (
    "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-{version}/llvm/LICENSE.TXT"
)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def find_llvm_root(extract_dir: Path) -> Path:
    """Returns the directory containing bin/ in the unpacked release."""
    if (extract_dir / "bin").is_dir():
        return extract_dir
    candidates = [entry for entry in extract_dir.iterdir() if (entry / "bin").is_dir()]
    if len(candidates) != 1:
        sys.exit(f"Expected one LLVM root under {extract_dir}, found {candidates}")
    return candidates[0]


def resolve_tool(llvm_root: Path, name: str) -> Path:
    """Returns the regular file a tool name refers to, following release symlinks."""
    path = llvm_root / "bin" / name
    if not path.exists():
        sys.exit(f"Required LLVM tool not found: {path}")
    real = path.resolve()
    if not real.is_file() or not real.is_relative_to(llvm_root.resolve()):
        sys.exit(f"LLVM tool {path} does not resolve to a file inside the release: {real}")
    return real


def collect_files(llvm_root: Path, clang_major: str) -> dict[str, Path]:
    """Maps archive-relative paths to the source files to package."""
    files: dict[str, Path] = {}
    for name in TOOLS:
        real = resolve_tool(llvm_root, name)
        files[f"bin/{real.name}"] = real

    include_dir = llvm_root / "lib" / "clang" / clang_major / "include"
    if not include_dir.is_dir():
        sys.exit(f"Clang builtin headers not found: {include_dir}")
    for path in sorted(include_dir.rglob("*")):
        if path.is_symlink():
            sys.exit(f"Unexpected symlink in the clang builtin headers: {path}")
        if path.is_file():
            files[path.relative_to(llvm_root).as_posix()] = path
    return files


def add_file(
    archive: tarfile.TarFile, arcname: str, data: bytes | None, source: Path | None
) -> None:
    info = tarfile.TarInfo(arcname)
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    if source is not None:
        info.size = source.stat().st_size
        info.mode = 0o755 if os.access(source, os.X_OK) else 0o644
        with source.open("rb") as f:
            archive.addfile(info, f)
    else:
        assert data is not None
        info.size = len(data)
        info.mode = 0o644
        archive.addfile(info, io.BytesIO(data))


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--archive", type=Path, required=True, help="Official LLVM release archive."
    )
    parser.add_argument("--sha256", required=True, help="Expected SHA-256 of --archive.")
    parser.add_argument("--llvm-version", required=True, help="LLVM release version, e.g. 19.1.7.")
    parser.add_argument("--output", type=Path, required=True, help="Output .tar.xz path.")
    args = parser.parse_args()

    actual = sha256_of(args.archive)
    if actual != args.sha256:
        sys.exit(f"SHA-256 mismatch for {args.archive}: expected {args.sha256}, got {actual}")

    clang_major = args.llvm_version.split(".", 1)[0]
    prefix = args.output.name.removesuffix(".tar.xz")

    with urllib.request.urlopen(LICENSE_URL.format(version=args.llvm_version)) as response:
        license_text = response.read()

    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(args.archive) as release:
            release.extractall(tmp, filter="tar")
        llvm_root = find_llvm_root(Path(tmp))
        files = collect_files(llvm_root, clang_major)

        with lzma.open(args.output, "wb", preset=9 | lzma.PRESET_EXTREME) as xz:
            with tarfile.open(fileobj=xz, mode="w", format=tarfile.PAX_FORMAT) as archive:
                add_file(archive, f"{prefix}/LICENSE.TXT", license_text, None)
                for arcname in sorted(files):
                    add_file(archive, f"{prefix}/{arcname}", None, files[arcname])

    print(f"Wrote {args.output} ({args.output.stat().st_size} bytes)")
    print(f"sha256: {sha256_of(args.output)}")
    print(f"strip_prefix: {prefix}")


if __name__ == "__main__":
    main()
