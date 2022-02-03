"""Test hook for simulating a node crash.

This hook will periodically send a SIGSTOP signal to the replica set nodes and copy their data
files. After copying the data files, the SIGCONT signal will be sent to the replica set nodes to
continue normal operation.

A standalone node will be started on the copied data files. These data files will be treated as an
unclean shutdown. Once started, validate will be run on all collections. A validate failure
indicates a problem.
"""

import os
import pprint
import random
import shutil
import threading
import time
import pymongo

from buildscripts.resmokelib.core import process
from buildscripts.resmokelib.testing.hooks import interface


def validate(mdb, logger):
    """Return true if all collections are valid."""
    for db in mdb.database_names():
        for coll in mdb.get_database(db).list_collection_names():
            res = mdb.get_database(db).command({"validate": coll}, check=False)

            if res["ok"] != 1.0 or res["valid"] is not True:
                if "code" in res and res["code"] == 166:
                    # Command not supported on view.
                    pass
                else:
                    logger.info("FAILURE!\nValidate Response: {}", pprint.pformat(res))
                    return False
    return True


class BGJob(threading.Thread):
    """Background job to pause nodes, copy data files, resume nodes, and validate data files."""

    def __init__(self, hook):
        """Initialize the background job."""
        threading.Thread.__init__(self, name="SimulateCrashes")
        self.daemon = True
        self._hook = hook
        self._lock = threading.Lock()
        self._is_alive = True
        self.backup_num = 0
        self.found_error = False

    def run(self):
        """Run the background job."""
        while True:
            with self._lock:
                if self.is_alive is False:
                    break

            self._hook.pause_and_copy(self.backup_num)
            if not self._hook.validate_all(self.backup_num):
                self.found_error = True
                self._hook.running_test.fixture.teardown()
                self.is_alive = False
                return

            time.sleep(random.randint(1, 5))
            self.backup_num += 1

    def kill(self):
        """Kill the background job."""
        with self._lock:
            self.is_alive = False


class SimulateCrash(interface.Hook):
    """A hook to simulate crashes."""

    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture):
        """Initialize SimulateCrash."""
        interface.Hook.__init__(self, hook_logger, fixture, "Simulate crashes hook")
        self.found_error = False
        self.last_validate_port = 19000
        self.logger = hook_logger
        self.running_test = None
        self._background_job = None

    def pause_and_copy(self, backup_num):
        """For all replica set nodes, this will send a SIGSTOP signal, copy the data files and send a SIGCONT signal."""
        self.logger.info("Taking snapshot #{}".format(backup_num))
        nodes_to_copy = [x for x in self.fixture.nodes]
        random.shuffle(nodes_to_copy)

        for node in nodes_to_copy:
            node.mongod.pause()

            self.logger.info("Starting to copy data files. DBPath: {}".format(
                node.get_dbpath_prefix()))

            try:
                for tup in os.walk(node.get_dbpath_prefix(), followlinks=True):
                    if tup[0].endswith("/diagnostic.data") or tup[0].endswith("/_tmp"):
                        continue
                    if "/simulateCrashes" in tup[0]:
                        continue
                    for filename in tup[-1]:
                        if "Preplog" in filename:
                            continue
                        fqfn = "/".join([tup[0], filename])
                        self.copy_file(
                            node.get_dbpath_prefix(), fqfn,
                            node.get_dbpath_prefix() + "/simulateCrashes/{}".format(backup_num))
            finally:
                node.mongod.resume()

    @classmethod
    def copy_file(cls, root, fqfn, new_root):
        """Copy a file."""
        in_fd = os.open(fqfn, os.O_RDONLY)
        in_bytes = os.stat(in_fd).st_size

        rel = fqfn[len(root):]
        os.makedirs(new_root + "/journal", exist_ok=True)
        out_fd = os.open(new_root + rel, os.O_WRONLY | os.O_CREAT)
        os.sendfile(out_fd, in_fd, 0, in_bytes)
        os.close(out_fd)
        os.close(in_fd)

    def validate_all(self, backup_num):
        """Start a standalone node to validate all collections on the copied data files."""
        for node in self.fixture.nodes:
            if self.last_validate_port >= 20000:
                self.last_validate_port = 19000
            validate_port = self.last_validate_port
            self.last_validate_port += 1

            path = node.get_dbpath_prefix() + "/simulateCrashes/{}".format(backup_num)
            self.logger.info("Starting to validate. DBPath: {} Port: {}".format(
                path, validate_port))

            mdb = process.Process(self.logger, [
                node.mongod_executable, "--dbpath", path, "--port",
                str(validate_port), "--logpath",
                node.get_dbpath_prefix() + "/simulateCrashes/validate.log"
            ])
            mdb.start()

            client = pymongo.MongoClient("localhost:{}".format(validate_port))
            is_valid = validate(client, self.logger)

            mdb.stop()
            mdb.wait()

            if not is_valid:
                return False

            shutil.rmtree(path, ignore_errors=True)
        return True

    def before_suite(self, test_report):
        """Start the background thread."""
        self.logger.info("Starting the SimulateCrashes thread.")
        self._background_job = BGJob(self)
        self._background_job.start()

    def after_suite(self, test_report, teardown_flag=None):
        """Signal the background thread to exit, and wait until it does."""
        if self._background_job is None:
            return

        self.logger.info("Stopping the SimulateCrashes thread.")
        self._background_job.kill()
        self._background_job.join()

        if self._background_job.found_error:
            self.logger.error("Encountered an error inside the simulate crashes hook.",
                              exc_info=self._background_job.exc_info)

    def before_test(self, test, test_report):
        """Each test will call this before it executes."""
        self.running_test = test

    def after_test(self, test, test_report):
        """Each test will call this after it executes. Check if the hook found an error."""
        if self._background_job is None:
            return

        if not self._background_job.found_error:
            return

        self._background_job.kill()
        self._background_job.join()

        self.logger.error("Encountered an error inside the simulate crashes hook.",
                          exc_info=self._background_job.exc_info)
