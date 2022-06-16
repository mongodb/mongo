"""Util functions to assist in setting up a sharded cluster topology in Antithesis."""
import subprocess
import time


def mongo_process_running(host, port):
    """Check to see if the process at the given host & port is running."""
    return subprocess.run(['mongo', '--host', host, '--port',
                           str(port), '--eval', '"db.stats()"'], check=True)


def retry_until_success(func, kwargs=None, wait_time=1, timeout_period=30):
    """Retry the function periodically until timeout."""
    kwargs = {} if kwargs is None else kwargs
    timeout = time.time() + timeout_period
    while True:
        if time.time() > timeout:
            raise TimeoutError(
                f"{func.__name__} called with {kwargs} timed out after {timeout_period} second(s).")
        try:
            func(**kwargs)
            break
        except:  # pylint: disable=bare-except
            print(f"Retrying {func.__name__} called with {kwargs} after {wait_time} second(s).")
            time.sleep(wait_time)
