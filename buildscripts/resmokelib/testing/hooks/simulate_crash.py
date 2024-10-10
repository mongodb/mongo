"""Test hook for simulating a node crash.

This hook will periodically send a SIGSTOP signal to the replica set nodes and copy their data
files. After copying the data files, the SIGCONT signal will be sent to the replica set nodes to
continue normal operation.

A standalone node will be started on the copied data files. These data files will be treated as an
unclean shutdown. Once started, validate will be run on all collections. A validate failure
indicates a problem.
"""

import os
import random
import shutil
import time

import pymongo

from buildscripts.resmokelib.core import process
from buildscripts.resmokelib.testing.hooks import bghook


def validate(mdb, logger, acceptable_err_codes):
    """Return true if all collections are valid."""
    for db in mdb.list_database_names():
        for coll in mdb.get_database(db).list_collection_names():
            res = mdb.get_database(db).command({"validate": coll}, check=False)

            if res["ok"] != 1.0 or res["valid"] is not True:
                if "code" in res and res["code"] in acceptable_err_codes:
                    # Command not supported on view.
                    pass
                else:
                    logger.info("FAILURE!\nValidate Response: ")
                    logger.info(res)
                    return False
    return True


class SimulateCrash(bghook.BGHook):
    """A hook to simulate crashes."""

    def __init__(self, hook_logger, fixture):
        """Initialize SimulateCrash."""
        bghook.BGHook.__init__(self, hook_logger, fixture, "Simulate crashes hook")
        self.acceptable_err_codes = [166, 11600]
        self.backup_num = 0
        self.validate_port = self.fixture.fixturelib.get_next_port(self.fixture.job_num)

    def run_action(self):
        """Copy data files and run validation on all nodes."""
        self.pause_and_copy()

        if not self.validate_all():
            raise ValueError("Validation failed")

        time.sleep(random.randint(1, 5))
        self.backup_num += 1

    def pause_and_copy(self):
        """For all replica set nodes, this will send a SIGSTOP signal, copy the data files and send a SIGCONT signal."""
        self.logger.info("Taking snapshot #{}".format(self.backup_num))
        nodes_to_copy = [x for x in self.fixture.nodes]
        random.shuffle(nodes_to_copy)

        for node in nodes_to_copy:
            node.mongod.pause()

            self.logger.info(
                "Starting to copy data files. DBPath: {}".format(node.get_dbpath_prefix())
            )

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
                            node.get_dbpath_prefix(),
                            fqfn,
                            node.get_dbpath_prefix()
                            + "/simulateCrashes/{}".format(self.backup_num),
                        )
            finally:
                node.mongod.resume()

    @classmethod
    def copy_file(cls, root, fqfn, new_root):
        """Copy a file."""
        in_fd = os.open(fqfn, os.O_RDONLY)
        in_bytes = os.stat(in_fd).st_size

        rel = fqfn[len(root) :]
        os.makedirs(new_root + "/journal", exist_ok=True)
        out_fd = os.open(new_root + rel, os.O_WRONLY | os.O_CREAT)

        total_bytes_sent = 0
        while total_bytes_sent < in_bytes:
            bytes_sent = os.sendfile(out_fd, in_fd, total_bytes_sent, in_bytes - total_bytes_sent)
            if bytes_sent == 0:
                raise ValueError("Unexpectedly reached EOF copying file")
            total_bytes_sent += bytes_sent

        os.close(out_fd)
        os.close(in_fd)

    def validate_all(self):
        """Start a standalone node to validate all collections on the copied data files."""
        for node in self.fixture.nodes:
            path = node.get_dbpath_prefix() + "/simulateCrashes/{}".format(self.backup_num)
            self.logger.info(
                "Starting to validate. DBPath: {} Port: {}".format(path, self.validate_port)
            )

            mdb = process.Process(
                self.logger,
                [
                    node.mongod_executable,
                    "--dbpath",
                    path,
                    "--port",
                    str(self.validate_port),
                    "--setParameter",
                    "enableTestCommands=1",
                    "--setParameter",
                    "testingDiagnosticsEnabled=1",
                ],
            )
            mdb.start()

            client = pymongo.MongoClient(
                host="localhost",
                port=self.validate_port,
                connect=True,
                connectTimeoutMS=300000,
                serverSelectionTimeoutMS=300000,
                directConnection=True,
            )
            is_valid = validate(client, self.logger, self.acceptable_err_codes)

            mdb.stop()
            mdb.wait()

            if not is_valid:
                return False

            shutil.rmtree(path, ignore_errors=True)
        return True

    def after_test(self, test, test_report):
        """Each test will call this after it executes. Check if the hook found an error."""
        self._background_job.kill()
        self._background_job.join()

        if self._background_job.err is not None and test_report.wasSuccessful():
            self.logger.error(
                "Encountered an error inside the hook after all tests passed: %s.",
                self._background_job.err,
            )
            raise self._background_job.err
        else:
            self.logger.info("Reached end of cycle in the hook, killing background thread.")
