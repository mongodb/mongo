"""Probe the tools selected by a Bazel C++ toolchain on their execution platform."""

import argparse
import json
import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path


def run_tool(arguments: list[str], environment: dict[str, str]) -> str:
    try:
        result = subprocess.run(
            arguments,
            env=os.environ | environment,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"Could not probe tool {arguments!r}: {exc.stdout or ''}{exc.stderr or ''}"
        ) from exc
    except (OSError, subprocess.SubprocessError) as exc:
        raise RuntimeError(f"Could not probe tool {arguments!r}: {exc}") from exc
    return result.stdout + result.stderr


def compiler_version(compiler: str, driver: str, environment: dict[str, str]) -> str:
    option = "-dumpfullversion" if compiler == "gcc" else "-dumpversion"
    version = run_tool([driver, option], environment).strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:git\S*)?", version):
        raise RuntimeError(f"Invalid compiler version from {driver}: {version!r}")
    return version


def linker_from_trace(trace: str) -> str:
    for line in reversed(trace.splitlines()):
        if not line.startswith(" "):
            continue
        command = shlex.split(line)
        if command and re.fullmatch(
            r"(?:.*-)?(?:ld(?:\.(?:lld|mold|bfd|gold))?|ld64\.lld|gold|mold|lld)",
            Path(command[0]).name,
        ):
            return command[0]
    raise RuntimeError(f"Could not find linker command in compiler driver trace:\n{trace}")


def parse_linker_version(banner: str) -> tuple[str, str]:
    for name, pattern in (
        ("lld", r"\bLLD (\d+\.\d+\.\d+(?:git\S*)?)"),
        ("mold", r"\bmold (\d+\.\d+\.\d+)"),
        ("apple", r"PROJECT:ld-(\d+(?:\.\d+)+)"),
        # gold reports both the containing Binutils release and its own version.
        ("gold", r"GNU gold[^\n]*?\) (\d+\.\d+(?:\.\d+)*)\s*(?:\n|$)"),
        ("bfd", r"GNU ld[^\n]*? (\d+\.\d+(?:\.\d+)*)\s*(?:\n|$)"),
    ):
        match = re.search(pattern, banner)
        if match:
            return name, match[1]
    raise RuntimeError(f"Could not parse linker version: {banner!r}")


def linker_banner(driver: str, flags: list[str], environment: dict[str, str]) -> str:
    # Query through the driver so cross-build wrappers execute the linker in
    # the same container as real links. --version exits before reading the
    # empty object. Keep temporary inputs in the execroot, which is mounted
    # into the wrapper's container on macOS hosts.
    # Keep driver options (including linker selection, target and search paths),
    # but omit linker arguments that read build inputs such as order files.
    # Those inputs need not exist to identify the selected tool's version.
    driver_flags = []
    skip_next = False
    for flag in flags:
        if skip_next:
            skip_next = False
        elif flag == "-Xlinker":
            skip_next = True
        elif not flag.startswith("-Wl,"):
            driver_flags.append(flag)
    with tempfile.TemporaryDirectory(dir=".") as temporary:
        obj = Path(temporary).resolve() / "version_probe.o"
        obj.touch()
        return run_tool([driver, *driver_flags, "-Wl,--version", str(obj)], environment)


def probe_linker(
    driver: str, flags: list[str], environment: dict[str, str], compiler: str = "clang"
) -> dict[str, str]:
    if compiler == "gcc":
        # GCC invokes collect2, which does not expose its selected linker with
        # -###. Forward --version to it using an empty object: the linker exits
        # after printing its banner, without reading the object or linking.
        output = linker_banner(driver, flags, environment)
        name, version = parse_linker_version(output)
        banner = next(
            line for line in output.splitlines() if version in line and "collect2" not in line
        )
        return {"linker": name, "linker_version": version, "ld": f"{driver}: {banner}"}
    # -### prints the actual commands without compiling or linking. In particular,
    # -print-prog-name=ld ignores -fuse-ld and can report the wrong linker.
    with tempfile.TemporaryDirectory(dir=".") as temporary:
        source = Path(temporary).resolve() / "version_probe.cpp"
        source.write_text("int main() { return 0; }\n")
        trace = run_tool([driver, *flags, "-###", str(source)], environment)
    linker_path = linker_from_trace(trace)
    # Apple's system ld has no --version. Its -v query does not need the driver;
    # all other supported linkers can be queried through cross-build wrappers.
    if Path(linker_path).name == "ld" and os.sys.platform == "darwin":
        banner = run_tool([linker_path, "-v"], environment)
    else:
        banner = linker_banner(driver, flags, environment)
    name, version = parse_linker_version(banner)
    return {"linker": name, "linker_version": version, "ld": f"{linker_path}: {banner.strip()}"}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", choices=("compiler", "linker"), required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--flags", required=True)
    parser.add_argument("--environment", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    environment = json.loads(args.environment)
    if args.kind == "compiler":
        data = {
            "compiler": args.compiler,
            "compiler_version": compiler_version(args.compiler, args.driver, environment),
        }
    else:
        data = probe_linker(args.driver, json.loads(args.flags), environment, args.compiler)
    Path(args.output).write_text(json.dumps(data))


if __name__ == "__main__":
    main()
