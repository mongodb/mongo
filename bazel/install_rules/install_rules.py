import argparse
import contextlib
import json
import os
import shutil
import tempfile
import time
from collections.abc import Iterator

try:
    import fcntl
except ImportError:
    fcntl = None

try:
    import msvcrt
except ImportError:
    msvcrt = None

parser = argparse.ArgumentParser()

parser.add_argument("--depfile", action="append")
parser.add_argument("--install-dir")
parser.add_argument("--install-mode", choices=["copy", "symlink", "hardlink"], default="hardlink")

args = parser.parse_args()
if os.path.exists(args.install_dir):
    os.chmod(args.install_dir, 0o755)
    for root, dirs, files in os.walk(args.install_dir):
        for name in files:
            try:
                os.chmod(os.path.join(root, name), 0o755)
                os.unlink(os.path.join(root, name))
            # Sometimes we find files that don't exist
            # from os.walk - not sure why
            except FileNotFoundError:
                continue
        for name in dirs:
            try:
                os.chmod(os.path.join(root, name), 0o755)
            except FileNotFoundError:
                continue
    shutil.rmtree(args.install_dir)
os.makedirs(args.install_dir, exist_ok=True)


def _configuration_name(install_dir):
    """Return Bazel's output configuration from a bazel-out path."""
    components = os.path.normpath(install_dir).split(os.sep)
    for index in range(len(components) - 2):
        if components[index] == "bazel-out" and components[index + 2] == "bin":
            return components[index + 1]
    raise RuntimeError(
        f"install directory does not contain a Bazel output configuration: {install_dir}"
    )


shared_install_root = os.environ.get("MONGO_BAZEL_SHARED_INSTALL_DIR")
if shared_install_root:
    # The output base is shared by all Bazel configurations. Keep the writable install trees
    # separate just as the default install_link (args.install_dir/../install) is.
    install_link = os.path.join(shared_install_root, _configuration_name(args.install_dir))
else:
    install_link = args.install_dir + "/../install"
os.makedirs(install_link, exist_ok=True)
shared_install_lock = install_link + ".lock"


@contextlib.contextmanager
def _exclusive_file_lock(lock_path: str) -> Iterator[None]:
    """Serialize mutations to the shared install tree on Unix and Windows."""
    with open(lock_path, "a+b") as lock_file:
        if fcntl is not None:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
            return

        if msvcrt is not None:
            lock_file.seek(0, os.SEEK_END)
            if lock_file.tell() == 0:
                lock_file.write(b"\0")
                lock_file.flush()
            lock_file.seek(0)
            msvcrt.locking(lock_file.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                lock_file.seek(0)
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
            return

        # All supported host platforms provide one of the locking APIs above.
        yield


def _copy_file_atomically(src: str, dst: str) -> None:
    """Copy a file to dst without exposing a partially copied shared install file."""
    destination_dir = os.path.dirname(dst)
    temporary_fd, temporary_dst = tempfile.mkstemp(
        dir=destination_dir,
        prefix=f".{os.path.basename(dst)}.",
    )
    os.close(temporary_fd)
    try:
        shutil.copyfile(src, temporary_dst)
        shutil.copymode(src, temporary_dst)
        os.replace(temporary_dst, dst)
    finally:
        try:
            os.unlink(temporary_dst)
        except FileNotFoundError:
            pass


def _install_destination(src: str, dst: str, is_shared: bool) -> None:
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if args.install_mode == "hardlink":
        destination_is_different = False
        if os.path.exists(dst):
            try:
                destination_is_different = not os.path.samefile(src, dst)
            except FileNotFoundError:
                # Another install action may have removed the shared destination
                # between exists() and samefile().
                pass
        if destination_is_different:
            try:
                if os.path.isdir(dst):
                    shutil.rmtree(dst)
                else:
                    os.chmod(dst, 0o755)
                    os.unlink(dst)
            except FileNotFoundError as exc:
                if is_shared:
                    # if multiple installs are requested at once and happen
                    # to install the same file, it is ambiguous
                    # when one should be linked into the general install dir
                    # so we pass on exceptions
                    pass
                else:
                    raise exc
            except OSError as exc:
                if exc.strerror == "Directory not empty":
                    print("Encountered OSError: Directory not empty. Retrying...")
                    time.sleep(1)
                    shutil.rmtree(dst)
                else:
                    raise exc
        if not os.path.exists(dst):
            try:
                if os.path.isdir(src):
                    for root, _, files in os.walk(src):
                        for name in files:
                            source_file = os.path.join(root, name)
                            dest_dir = os.path.dirname(os.path.join(root, name)).replace(src, dst)
                            if not os.path.exists(dest_dir):
                                os.makedirs(dest_dir)
                            _copy_file_atomically(source_file, os.path.join(dest_dir, name))
                else:
                    try:
                        os.link(src, dst)
                    # If you try hardlinking across drives link will fail
                    except OSError:
                        _copy_file_atomically(src, dst)
            except FileExistsError as exc:
                if is_shared:
                    # if multiple installs are requested at once and happen
                    # to install the same file, it is ambiguous
                    # when one should be linked into the general install dir
                    # so we pass on exceptions
                    pass
                else:
                    raise exc
    else:
        raise Exception("Only hardlink mode is currently implemented.")


def install(src: str, install_type: str, is_rename: bool = False) -> str:
    if is_rename:
        install_dst = os.path.join(args.install_dir, install_type)
        link_dst = os.path.join(install_link, install_type)
    else:
        install_dst = os.path.join(args.install_dir, install_type, os.path.basename(src))
        link_dst = os.path.join(install_link, install_type, os.path.basename(src))

    if src.endswith(".dwp"):
        # Due to us creating our binaries using the _with_debug name
        # the dwp files also contain it. Strip the _with_debug from the name
        install_dst = install_dst.replace("_with_debug.dwp", ".dwp")
        link_dst = link_dst.replace("_with_debug.dwp", ".dwp")

    # The action-specific output can be installed in parallel. Only the shared
    # convenience tree needs serialization, including directory replacement and
    # the complete copy of a directory such as a dSYM bundle.
    _install_destination(src, install_dst, is_shared=False)
    with _exclusive_file_lock(shared_install_lock):
        _install_destination(src, link_dst, is_shared=True)

    return install_dst


for depfile in args.depfile:
    with open(depfile) as f:
        content = json.load(f)
        for binary in content["bins"]:
            install(binary, "bin")
        for lib in content["libs"]:
            install(lib, "lib")
        for file, folder in content["roots"].items():
            install(file, folder)
        for file, folder in content["includes"].items():
            install(file, folder, is_rename=True)
