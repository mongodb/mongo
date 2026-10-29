# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Manage a local mongod process for calibration."""

import os
import subprocess
import time

import pymongo
import pymongo.errors
from database_instance import DatabaseInstance


class MongodManager:
    """Start, stop, and cold-restart a local mongod."""

    def __init__(self, mongod_bin, db_config, dbpath, extra_args=None):
        self.mongod_bin = mongod_bin
        self.db_config = db_config
        self.dbpath = dbpath
        self.extra_args = extra_args or []
        self._proc = None
        self._database = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.stop()

    @property
    def connection_string(self):
        return self.db_config.connection_string

    @property
    def database(self) -> DatabaseInstance:
        if self._database is None:
            raise RuntimeError("mongod is not running")
        return self._database

    @property
    def is_running(self):
        return self._proc is not None and self._proc.poll() is None

    def start(self, extra_start_args=None):
        """Start the mongod process."""
        if self.is_running:
            raise RuntimeError("mongod is already running")

        os.makedirs(self.dbpath, exist_ok=True)
        with open(os.path.join(self.dbpath, "mongod.log"), "a") as log_fh:
            self._proc = subprocess.Popen(
                [
                    self.mongod_bin,
                    "--dbpath",
                    self.dbpath,
                    *self.extra_args,
                    *(extra_start_args or []),
                ],
                stdout=log_fh,
                stderr=subprocess.STDOUT,
            )
        self._wait_ready()
        self._database = DatabaseInstance(self.db_config)

    def stop(self):
        """Shutdown mongod cleanly and release resources."""
        if not self.is_running:
            return

        try:
            client = pymongo.MongoClient(self.connection_string, serverSelectionTimeoutMS=5000)
            client.admin.command("shutdown")
        except (pymongo.errors.AutoReconnect, pymongo.errors.ServerSelectionTimeoutError):
            pass

        self._proc.wait(timeout=30)
        self._proc = None
        self._database = None

    def restart_cold(self, extra_start_args=None):
        """Stop mongod, drop OS page cache, start fresh."""
        self.stop()
        self.flush_os_cache()
        self.start(extra_start_args=extra_start_args)

    def flush_os_cache(self):
        """Flush the OS page cache."""
        subprocess.run(["sync"], check=True)
        subprocess.run(
            ["sudo", "sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"],
            check=True,
        )

    def _wait_ready(self, timeout=60, poll_interval=0.25):
        client = pymongo.MongoClient(self.connection_string, serverSelectionTimeoutMS=500)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self._proc.poll() is not None:
                raise RuntimeError(f"mongod exited with code {self._proc.returncode}")
            try:
                client.admin.command("ping")
                return
            except pymongo.errors.ConnectionFailure:
                time.sleep(poll_interval)
        raise RuntimeError(f"mongod did not become ready in {timeout}s")
