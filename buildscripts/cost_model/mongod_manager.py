# Copyright (C) 2026-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
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
