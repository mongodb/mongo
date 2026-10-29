import os
import sys
import tempfile
import time

# A fixed, host-wide path (not under any per-task working directory) so that concurrent
# burn-in copies of the same test file, each running from their own task workspace, still
# contend for the same lock file.
LOCK_PATH = os.path.join(tempfile.gettempdir(), "mongodb_ssl_castore_test.lock")

# If a lock is older than this, assume its holder crashed without releasing it and steal it,
# rather than deadlocking every future run on the host.
STALE_SECONDS = 300
TIMEOUT_SECONDS = 300
POLL_SECONDS = 0.5


def acquire(token):
    deadline = time.time() + TIMEOUT_SECONDS
    while True:
        try:
            fd = os.open(LOCK_PATH, os.O_CREAT | os.O_EXCL | os.O_RDWR)
            os.write(fd, token.encode("utf-8"))
            os.close(fd)
            return 0
        except FileExistsError:
            try:
                if time.time() - os.path.getmtime(LOCK_PATH) > STALE_SECONDS:
                    os.remove(LOCK_PATH)
                    continue
            except OSError:
                continue  # Lock file vanished between the exists check and the remove; retry.
            if time.time() > deadline:
                print(f"Timed out waiting for lock '{LOCK_PATH}'", file=sys.stderr)
                return 1
            time.sleep(POLL_SECONDS)


def release(token):
    # Only remove the lock if we are the current holder, identified by the token written at
    # acquire time. This makes release() safe to call even if this process never successfully
    # acquired the lock (e.g. acquire() itself timed out).
    try:
        with open(LOCK_PATH, "r") as f:
            owner = f.read()
        if owner == token:
            os.remove(LOCK_PATH)
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    action, token = sys.argv[1], sys.argv[2]
    sys.exit(acquire(token) if action == "acquire" else release(token))
