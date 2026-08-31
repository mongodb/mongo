import argparse
import contextlib
import json
import os
import signal
import shutil
import stat
import sys
import tempfile
import time
from collections.abc import Callable, Iterator

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


def _remove_readonly(function, path: str, error: BaseException) -> None:
    """Retry a failed tree removal after making a read-only path writable."""
    if not isinstance(error, PermissionError):
        raise error

    # POSIX deletion is controlled by the parent directory. Avoid chmodding a hardlinked file,
    # which would also mutate the Bazel source inode. Windows additionally requires clearing a
    # file's read-only attribute; read-only Windows inputs are copied instead of hardlinked.
    parent = os.path.dirname(path)
    if parent:
        os.chmod(parent, 0o700)
    if os.name == "nt":
        # A sharing violation is also reported as PermissionError. Do not chmod an already
        # writable hardlink (and therefore its source inode) when the real problem is an open
        # executable handle.
        if not (os.stat(path, follow_symlinks=False).st_mode & stat.S_IWRITE):
            os.chmod(path, stat.S_IWRITE)
    elif os.path.isdir(path) and not os.path.islink(path):
        os.chmod(path, 0o700)
    function(path)


def _remove_tree(path: str) -> None:
    # Make directories writable up front so their children can be unlinked without touching
    # hardlinked file modes.
    os.chmod(path, 0o700)
    for root, directories, _ in os.walk(path):
        os.chmod(root, 0o700)
        for directory in directories:
            directory_path = os.path.join(root, directory)
            if not os.path.islink(directory_path):
                os.chmod(directory_path, 0o700)
    shutil.rmtree(path, onexc=_remove_readonly)


def _make_directory_writable(path: str) -> None:
    """Restore owner write/search permissions on a shared directory before publishing."""
    try:
        directory_stat = os.stat(path, follow_symlinks=False)
    except FileNotFoundError:
        return
    if stat.S_ISDIR(directory_stat.st_mode):
        os.chmod(path, stat.S_IMODE(directory_stat.st_mode) | stat.S_IRWXU)


def _replace_staged_destination(staged_destination: str, destination: str) -> None:
    """Publish a staged artifact, reopening directories if the host protects them mid-action."""
    try:
        os.replace(staged_destination, destination)
    except PermissionError:
        # Some local action runners protect temporary trees after an action completes. The
        # staging directory is outside the declared outputs too, so reopen both sides before
        # retrying the rename. This is harmless when the first error was transient and still
        # reports the original permission failure when the filesystem is genuinely inaccessible.
        _make_directory_writable(staged_destination)
        _make_directory_writable(os.path.dirname(staged_destination))
        _make_directory_writable(os.path.dirname(destination))
        os.replace(staged_destination, destination)


@contextlib.contextmanager
def _termination_as_exception() -> Iterator[None]:
    """Let SIGTERM unwind install transactions instead of skipping their cleanup."""

    def terminate(signum, _frame) -> None:
        raise SystemExit(128 + signum)

    previous_handler = signal.signal(signal.SIGTERM, terminate)
    try:
        yield
    finally:
        signal.signal(signal.SIGTERM, previous_handler)


args = parser.parse_args()
if os.path.exists(args.install_dir):
    _remove_tree(args.install_dir)
os.makedirs(args.install_dir, exist_ok=True)


def _output_base(install_dir: str) -> str | None:
    normalized = os.path.abspath(install_dir)
    marker = f"{os.sep}bazel-out{os.sep}"
    marker_index = normalized.rfind(marker)
    if marker_index == -1:
        return None
    return normalized[:marker_index]


def _bazel_output_base(install_dir: str) -> str | None:
    """Return Bazel's output base from an execroot-relative install path."""
    normalized = os.path.abspath(install_dir)
    marker = f"{os.sep}execroot{os.sep}"
    marker_index = normalized.rfind(marker)
    if marker_index == -1:
        return None
    return normalized[:marker_index]


def _darwin_shared_install_root(output_base: str) -> str:
    """Return a writable host path that does not overlap Bazel's output-root hierarchy."""
    output_base_name = os.path.basename(os.path.normpath(output_base))
    return os.path.join(tempfile.gettempdir(), f"{output_base_name}-mongo-shared-install")


def _linux_native_shared_install_root(output_base: str) -> str:
    """Return the host-temp shared install root used by native Linux actions."""
    output_base_name = os.path.basename(os.path.normpath(output_base))
    return os.path.join(tempfile.gettempdir(), f"{output_base_name}-mongo-shared-install")


action_output_base = _output_base(args.install_dir)
bazel_output_base = _bazel_output_base(args.install_dir)
legacy_install_link = os.path.abspath(os.path.join(args.install_dir, os.pardir, "install"))
external_shared_install = False


def _stable_output_symlink_target(src: str) -> str | None:
    """Return a persistent Bazel output target for a safe symlink input."""
    if action_output_base is None or not os.path.islink(src):
        return None

    output_marker = f"{os.sep}bazel-out{os.sep}"
    source_path = os.path.abspath(src)
    symlink_target = os.path.realpath(src)
    if output_marker not in source_path or output_marker not in symlink_target:
        return None

    try:
        # Inputs in a sandbox may resolve into the sandbox's own output tree. Those links would
        # dangle after the action exits, while a target outside this action output base persists.
        target_is_in_action_output = (
            os.path.commonpath((action_output_base, symlink_target)) == action_output_base
        )
    except ValueError:
        return None

    if target_is_in_action_output or not os.path.isfile(symlink_target):
        return None
    return symlink_target


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
    install_link = os.path.abspath(
        os.path.join(shared_install_root, _configuration_name(args.install_dir))
    )
elif sys.platform == "darwin" and action_output_base is not None and bazel_output_base is not None:
    # Darwin may make the entire Bazel output-root hierarchy read-only after a local action
    # completes. Keep the shared publication tree in the host temp directory when the wrapper did
    # not provide an explicit shared-install root.
    shared_install_root = _darwin_shared_install_root(bazel_output_base)
    install_link = os.path.abspath(
        os.path.join(shared_install_root, _configuration_name(args.install_dir))
    )
    external_shared_install = True
elif sys.platform == "linux" and action_output_base is not None and bazel_output_base is not None:
    # Linux install actions run locally even when the rest of the build uses remote execution.
    # The output root can be protected after another local action completes. Keep the implicit
    # native fallback in the host temp directory; Linux container actions receive an explicit
    # output-base-sibling root through MONGO_BAZEL_SHARED_INSTALL_DIR instead.
    shared_install_root = _linux_native_shared_install_root(bazel_output_base)
    install_link = os.path.abspath(
        os.path.join(shared_install_root, _configuration_name(args.install_dir))
    )
    external_shared_install = True
else:
    # Keep the legacy path for non-Bazel invocations, where there is no output base from which to
    # derive the shared tree.
    install_link = os.path.abspath(os.path.join(args.install_dir, os.pardir, "install"))
os.makedirs(install_link, exist_ok=True)
shared_install_lock = install_link + ".lock"
shared_install_staging = install_link + ".staging"
os.makedirs(shared_install_staging, exist_ok=True)


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


def _publish_external_convenience_symlink() -> None:
    """Keep bazel-bin/install pointing at an externally published shared tree."""
    if not external_shared_install or legacy_install_link == install_link:
        return

    try:
        with _exclusive_file_lock(shared_install_lock):
            if os.path.islink(legacy_install_link):
                if os.path.realpath(legacy_install_link) == install_link:
                    return
                os.unlink(legacy_install_link)
            elif os.path.lexists(legacy_install_link):
                _make_directory_writable(os.path.dirname(legacy_install_link))
                if os.path.isdir(legacy_install_link):
                    _remove_tree(legacy_install_link)
                else:
                    os.unlink(legacy_install_link)
            _make_directory_writable(os.path.dirname(legacy_install_link))
            os.symlink(install_link, legacy_install_link, target_is_directory=True)
    except PermissionError:
        # The action's declared outputs remain valid even if a stale convenience directory is
        # protected by the host. The wrapper or a later action can publish the link once writable.
        return


def _copy_file_atomically(src: str, dst: str) -> None:
    """Materialize a file at dst without exposing a partially written file."""
    destination_dir = os.path.dirname(dst)
    temporary_fd, temporary_dst = tempfile.mkstemp(
        dir=destination_dir,
        # Do not repeat the destination basename here. Runfiles paths can be long enough on
        # Windows that the final destination is valid but the temporary path exceeds MAX_PATH.
        prefix=".tmp-",
    )
    os.close(temporary_fd)
    try:
        os.unlink(temporary_dst)
        _link_or_copy_file(src, temporary_dst)
        os.replace(temporary_dst, dst)
    finally:
        try:
            os.unlink(temporary_dst)
        except FileNotFoundError:
            pass


def _link_or_copy_file(src: str, dst: str) -> None:
    """Hardlink stable inputs when possible, copying cross-device or unsafe inputs."""
    source_stat = os.stat(src)
    # Clearing the Windows read-only bit on one hardlink changes it for every link, including
    # the Bazel source. Copy read-only Windows inputs so later cleanup cannot mutate the source.
    windows_readonly = os.name == "nt" and not (source_stat.st_mode & stat.S_IWRITE)
    source_is_symlink = os.path.islink(src)
    stable_symlink_target = _stable_output_symlink_target(src)

    if (
        not source_is_symlink and stat.S_ISREG(source_stat.st_mode) and not windows_readonly
    ) or stable_symlink_target is not None:
        try:
            os.link(src, dst, follow_symlinks=False)
            return
        except (NotImplementedError, OSError, TypeError):
            # The action output and shared install tree can be separate mounts in the Linux
            # container. Preserve a stable output symlink when a cross-device hardlink fails;
            # copying it would materialize the potentially multi-GiB target.
            if stable_symlink_target is not None:
                try:
                    os.symlink(stable_symlink_target, dst)
                    return
                except OSError:
                    pass

    # Source-tree inputs can point into a writable checkout, while a hardlink to a relative or
    # sandbox-local symlink would dangle when the action sandbox disappears.
    shutil.copyfile(src, dst)
    shutil.copymode(src, dst)


def _copy_directory(
    src: str,
    dst: str,
    copy_file: Callable[[str, str], None],
    active_directories: set[tuple[int, int, str]] | None = None,
) -> None:
    """Copy a tree while dereferencing directory symlinks and rejecting cycles."""
    if active_directories is None:
        active_directories = set()

    source_stat = os.stat(src)
    identity = (
        source_stat.st_dev,
        source_stat.st_ino,
        "" if source_stat.st_ino else os.path.normcase(os.path.realpath(src)),
    )
    if identity in active_directories:
        raise RuntimeError(f"Directory symlink cycle while installing {src}")

    active_directories.add(identity)
    try:
        os.makedirs(dst)
        with os.scandir(src) as entries:
            for entry in entries:
                source_entry = os.path.join(src, entry.name)
                destination_entry = os.path.join(dst, entry.name)
                if entry.is_dir(follow_symlinks=True):
                    _copy_directory(
                        source_entry,
                        destination_entry,
                        copy_file,
                        active_directories,
                    )
                else:
                    copy_file(source_entry, destination_entry)
        shutil.copymode(src, dst)
    finally:
        active_directories.remove(identity)


def _populate_destination(src: str, dst: str) -> None:
    """Populate an absent destination from src."""
    if os.path.isdir(src):
        _copy_directory(src, dst, _link_or_copy_file)
    else:
        _link_or_copy_file(src, dst)


def _remove_destination(dst: str) -> None:
    if os.path.isdir(dst) and not os.path.islink(dst):
        _remove_tree(dst)
        return

    try:
        os.unlink(dst)
    except PermissionError as error:
        _remove_readonly(os.unlink, dst, error)


def _install_destination(src: str, dst: str) -> None:
    """Install an action-private output destination."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if args.install_mode == "hardlink":
        destination_is_different = False
        if os.path.exists(dst):
            try:
                destination_is_different = not os.path.samefile(src, dst)
            except FileNotFoundError:
                # The destination disappeared between exists() and samefile().
                pass
        if destination_is_different:
            try:
                _remove_destination(dst)
            except OSError as exc:
                if exc.strerror == "Directory not empty":
                    print("Encountered OSError: Directory not empty. Retrying...")
                    time.sleep(1)
                    _remove_tree(dst)
                else:
                    raise
        if not os.path.exists(dst):
            if os.path.isdir(src):
                _copy_directory(src, dst, _copy_file_atomically)
            else:
                _copy_file_atomically(src, dst)
    else:
        raise Exception("Only hardlink mode is currently implemented.")


def _install_shared_destination(src: str, dst: str) -> None:
    """Stage a shared artifact in parallel and publish it under the install-tree lock."""
    if args.install_mode != "hardlink":
        raise Exception("Only hardlink mode is currently implemented.")

    def same_file(first: str, second: str) -> bool:
        # A shared symlink into an action sandbox must be replaced even when it currently
        # resolves to the source; otherwise it dangles as soon as that sandbox is removed.
        if os.path.islink(first) or os.path.islink(second):
            return False
        try:
            return os.path.samefile(first, second)
        except OSError:
            return False

    if same_file(src, dst):
        return

    # Keep the workspace beside install_link. This guarantees same-filesystem renames without
    # exposing an active or stale stage through bazel-bin/install.
    workspace = tempfile.mkdtemp(prefix=".stage-", dir=shared_install_staging)
    staged_destination = os.path.join(workspace, "new")
    previous_destination = os.path.join(workspace, "old")
    preserve_workspace = False
    operation_failed = False

    try:
        # This is the expensive operation for dSYM and toolchain trees. It deliberately runs
        # without the configuration-wide lock so unrelated install actions can make progress.
        _populate_destination(src, staged_destination)

        with _exclusive_file_lock(shared_install_lock):
            # Bazel may make output-tree directories read-only after another local install action
            # completes. The convenience tree is intentionally outside the declared outputs, so
            # restore owner write/search permissions before the final rename.
            _make_directory_writable(install_link)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            _make_directory_writable(os.path.dirname(dst))
            _make_directory_writable(staged_destination)
            _make_directory_writable(os.path.dirname(staged_destination))
            # Another action may have published the same regular file while this action staged
            # it. Avoid replacing an already-correct destination, especially on Windows where
            # readers can hold handles that prevent replacement.
            if same_file(staged_destination, dst):
                return

            try:
                if os.path.lexists(dst):
                    os.replace(dst, previous_destination)
                _replace_staged_destination(staged_destination, dst)
            except BaseException:
                # An asynchronous interruption can arrive after either rename completed. Inspect
                # the workspace instead of relying on an in-memory transaction flag so the old
                # destination is never deleted by the outer cleanup.
                if os.path.lexists(previous_destination):
                    preserve_workspace = True
                    try:
                        if os.path.lexists(dst):
                            os.replace(dst, staged_destination)
                        os.replace(previous_destination, dst)
                    except OSError as rollback_error:
                        raise RuntimeError(
                            "Failed to publish the shared install destination and restore its "
                            f"previous contents; recovery data remains at {workspace}"
                        ) from rollback_error
                    preserve_workspace = False
                raise

        # Removing a displaced dSYM can also be expensive. It is private after the rename and
        # no longer needs to hold up other shared-tree publications.
        if os.path.lexists(previous_destination):
            _remove_destination(previous_destination)
    except BaseException:
        operation_failed = True
        raise
    finally:
        if not preserve_workspace:
            try:
                _remove_tree(workspace)
            except FileNotFoundError:
                pass
            except OSError:
                # Do not hide the operation or rollback error with a secondary cleanup failure.
                if not operation_failed:
                    raise


def _install_relative_path(src: str, install_type: str, is_rename: bool) -> str:
    # Depfiles use '/' as the logical separator. Reject alternate separators and drive
    # prefixes before joining so a Windows-generated '\\' separator is not mistaken for
    # an input path escape below.
    # An empty folder is valid for a non-rename root file: it places the file at the
    # install-tree root. Rename entries still require an explicit destination.
    if (is_rename and not install_type) or "\\" in install_type or ":" in install_type:
        raise RuntimeError(f"Invalid install destination: {install_type!r}")

    if is_rename:
        relative_path = install_type
    else:
        relative_path = os.path.join(install_type, os.path.basename(src))

    if src.endswith(".dwp"):
        # Due to us creating our binaries using the _with_debug name
        # the dwp files also contain it. Strip the _with_debug from the name
        relative_path = relative_path.replace("_with_debug.dwp", ".dwp")

    normalized = os.path.normpath(relative_path)
    if os.name != "nt" and "\\" in normalized:
        raise RuntimeError(f"Invalid install destination: {relative_path!r}")
    if (
        os.path.isabs(normalized)
        or normalized in (os.curdir, os.pardir)
        or normalized.startswith(os.pardir + os.sep)
    ):
        raise RuntimeError(f"Install destination escapes its install tree: {relative_path!r}")
    return normalized


def _destination_within(root: str, relative_path: str) -> str:
    destination = os.path.abspath(os.path.join(root, relative_path))
    real_root = os.path.realpath(root)
    # Existing leaf symlinks are legacy install artifacts that publication must replace. Resolve
    # only the parent: an escaping parent is unsafe, while an escaping leaf is itself the target.
    real_parent = os.path.realpath(os.path.dirname(destination))
    try:
        contained = os.path.commonpath((real_root, real_parent)) == real_root
    except ValueError:
        contained = False
    if not contained:
        raise RuntimeError(f"Install destination escapes its install tree: {relative_path!r}")
    return destination


def install(src: str, install_type: str, is_rename: bool = False) -> str:
    relative_path = _install_relative_path(src, install_type, is_rename)
    install_dst = _destination_within(args.install_dir, relative_path)
    link_dst = _destination_within(install_link, relative_path)

    # The action-specific output is private to this action. Shared artifacts are fully staged
    # before the lock is acquired, then published with constant-time renames.
    _install_destination(src, install_dst)
    _install_shared_destination(src, link_dst)

    return install_dst


_publish_external_convenience_symlink()


with _termination_as_exception():
    installed_destinations: dict[str, str] = {}

    def install_once(src: str, install_type: str, is_rename: bool = False) -> None:
        relative_path = _install_relative_path(src, install_type, is_rename)
        destination_key = os.path.normcase(os.path.normpath(relative_path)).casefold()
        previous_source = installed_destinations.get(destination_key)
        if previous_source is not None:
            if previous_source != src:
                raise RuntimeError(
                    f"Install destination {relative_path} has multiple sources: "
                    f"{previous_source} and {src}"
                )
            return

        installed_destinations[destination_key] = src
        install(src, install_type, is_rename)

    for depfile in args.depfile:
        with open(depfile) as f:
            content = json.load(f)
            for binary in content["bins"]:
                install_once(binary, "bin")
            for lib in content["libs"]:
                install_once(lib, "lib")
            for file, folder in content["roots"].items():
                install_once(file, folder)
            for file, folder in content["includes"].items():
                install_once(file, folder, is_rename=True)
