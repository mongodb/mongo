import hashlib
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

from bazel.wrapper_hook.wrapper_debug import wrapper_debug


def get_deps_dirs(deps):
    tmp_dir = pathlib.Path(os.environ["Temp"] if platform.system() == "Windows" else "/tmp")
    bazel_bin = REPO_ROOT / "bazel-bin"
    for dep in deps:
        try:
            for out_dir in [
                REPO_ROOT / "bazel-out",
                tmp_dir / "compiledb-out",
            ]:
                for child in os.listdir(out_dir):
                    yield f"{out_dir}/{child}/bin/external/poetry/{dep}", dep
        except OSError:
            pass
        yield f"{bazel_bin}/external/poetry/{dep}", dep


def add_module_to_path(poetry_dir, modules_added):
    for module in poetry_dir.iterdir():
        for dist_info in module.iterdir():
            if str(dist_info).endswith(".dist-info"):
                dirname = dist_info.parent
                module = dirname.name
                if module not in modules_added:
                    modules_added.add(module)
                    sys.path.append(str(dirname))


def setup_python_path():
    tmp_dir = pathlib.Path(os.environ["Temp"] if platform.system() == "Windows" else "/tmp")
    modules_added = set()

    for out_dir in [
        REPO_ROOT / "bazel-out",
        tmp_dir / "compiledb-out",
    ]:
        if out_dir.exists():
            for child in out_dir.iterdir():
                poetry_dir = child / "bin" / "external" / "poetry"
                if poetry_dir.exists():
                    add_module_to_path(poetry_dir, modules_added)

    poetry_dir = REPO_ROOT / "bazel-bin" / "external" / "poetry"
    if poetry_dir.exists():
        add_module_to_path(poetry_dir, modules_added)


def search_for_modules(deps, deps_installed, lockfile_changed=False):
    deps_not_found = deps.copy()
    wrapper_debug(f"deps_installed: {deps_installed}")
    for target_dir, dep in get_deps_dirs(deps):
        wrapper_debug(f"checking for {dep} in target_dir: {target_dir}")
        if dep in deps_installed:
            continue

        if not pathlib.Path(target_dir).exists():
            continue

        if not lockfile_changed:
            for entry in os.listdir(target_dir):
                if entry.endswith(".dist-info"):
                    wrapper_debug(f"found: {target_dir}")
                    deps_installed.append(dep)
                    deps_not_found.remove(dep)
                    break
        else:
            os.chmod(target_dir, 0o777)
            for root, dirs, files in os.walk(target_dir):
                for somedir in dirs:
                    os.chmod(pathlib.Path(root) / somedir, 0o777)
                for file in files:
                    os.chmod(pathlib.Path(root) / file, 0o777)
            shutil.rmtree(target_dir)
    wrapper_debug(f"deps_not_found: {deps_not_found}")
    return deps_not_found

def skip_cplusplus_toolchain(args):
    if any("no_c++_toolchain" in arg for arg in args):
        return True
    return False

def install_modules(bazel, args):
    need_to_install = False
    pwd_hash = hashlib.md5(str(REPO_ROOT).encode()).hexdigest()
    lockfile_hash_file = pathlib.Path(tempfile.gettempdir()) / f"{pwd_hash}_lockfile_hash"
    with open(REPO_ROOT / "poetry.lock", "rb") as f:
        current_hash = hashlib.md5(f.read()).hexdigest()

    old_hash = None
    if lockfile_hash_file.exists():
        with open(lockfile_hash_file) as f:
            old_hash = f.read()

    if old_hash != current_hash:
        with open(lockfile_hash_file, "w") as f:
            f.write(current_hash)

    deps = ["retry", "gitpython", "requests", "timeout-decorator", "boto3"]
    deps_installed = []
    deps_needed = search_for_modules(
        deps, deps_installed, lockfile_changed=old_hash != current_hash
    )

    if deps_needed:
        need_to_install = True

    if old_hash != current_hash:
        need_to_install = True
        deps_needed = deps

    if need_to_install:
        cmd = [
            bazel,
            "build",
        ] + ["@poetry//:library_" + dep.replace("-", "_") for dep in deps_needed]

        if skip_cplusplus_toolchain(args):
            cmd += ["--repo_env=no_c++_toolchain=1"]

        proc = subprocess.run(
            cmd
            + [
                "--remote_download_all",
                "--bes_backend=",
                "--bes_results_url=",
            ]
        )
        if proc.returncode != 0:
            print("Failed to install modules using remote exec/cache, falling back to local...")
            proc = subprocess.run(
                cmd
                + [
                    "--config=local",
                ]
            )
        deps_missing = search_for_modules(deps_needed, deps_installed)
        if deps_missing:
            raise Exception(f"Failed to install python deps {deps_missing}")
    setup_python_path()
