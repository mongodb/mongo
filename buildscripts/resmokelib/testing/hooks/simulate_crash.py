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

from buildscripts.resmokelib import config
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
                elif ("code" in res and "errmsg" in res and res["code"] == 26 and
                      "Timeseries buckets collection does not exist" in res["errmsg"]):
                    # TODO(SERVER-109819): Remove this workaround once v9.0 is last LTS
                    # Validating a timeseries view without a matching buckets collection fails with
                    # NamespaceNotFound. This can happen with this create+drop interleaving:
                    # Thread 1 (create mycoll as timeseries): Creates system.buckets.mycoll.
                    # Thread 2 (drop mycoll): Drops system.buckets.mycoll.
                    # Thread 1 (create mycoll as timeseries): Creates mycoll, the timeseries view.
                    # Ignore this error, which can not happen with viewless timeseries collections.
                    logger.info("Ignoring NamespaceNotFound due to orphan legacy timeseries view")
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
                self.capture_db(node.get_dbpath_prefix())
            finally:
                node.mongod.resume()

    def capture_db(self, dbpath):
        """Makes a lightweight copy of the given database at '<dbpath>/simulateCrashes/<backup_num>'.

        "Lightweight" here means that it excludes unnecessary files e.g. temporaries and diagnostics.

        Note: the directory structure wiredtiger creates can vary, due to configurations like
        --directoryPerDb or --wiredTigerDirectoryForIndexes, so it is necessary to recursively copy
        directories and files.
        """
        for current_path, _, filenames in os.walk(dbpath, followlinks=True):
            if (
                current_path.endswith(os.path.sep + "diagnostic.data")
                or current_path.endswith(os.path.sep + "_tmp")
                or os.path.sep + "simulateCrashes" in current_path
            ):
                continue
            dest_root = os.path.join(dbpath, "simulateCrashes", "{}".format(self.backup_num))
            rel_path = os.path.relpath(current_path, start=dbpath)
            os.makedirs(os.path.join(dest_root, rel_path), exist_ok=True)
            for filename in filenames:
                if "Preplog" in filename:
                    continue
                absolute_filepath = os.path.join(current_path, filename)
                self.copy_file(dbpath, absolute_filepath, dest_root)

    @classmethod
    def copy_file(cls, root, absolute_filepath, new_root):
        """Copy a file in |root| at |absolute_filepath| into |new_root|, maintaining its relative position.

        For example: '/a/b/c' if copied from '/a/b' to '/x' would yield '/x/c'.
        """
        in_fd = os.open(absolute_filepath, os.O_RDONLY)
        in_bytes = os.stat(in_fd).st_size

        rel = absolute_filepath[len(root) :]
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

            # When restarting the node for validation purposes, we need to mirror some
            # configuration options applied to the original standalone invocation.
            extra_configs = [
                "--" + cfg_k for (cfg_k, cfg_v) in config.MONGOD_EXTRA_CONFIG.items() if cfg_v
            ]

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
                ]
                + extra_configs,
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
