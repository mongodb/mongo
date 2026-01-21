"""Suite hook for verifying extension symbol visibility."""

import os
import subprocess

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.extensions.find_and_generate_extension_configs import (
    find_extension_so_files,
)
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.utils import default_if_none
from buildscripts.util.expansions import get_expansion


class VerifyExtensionsVisibility(interface.Hook):
    """Verify that extension .so files are safe to dlopen (symbol visibility, deps, overrides)."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, **kwargs):
        super().__init__(
            hook_logger,
            fixture,
            description="Validate extension .so visibility and dlopen safety",
        )

    def before_suite(self, test_report):
        mongod_path = default_if_none(
            self.fixture.mongod_executable,
            self.fixture.config.MONGOD_EXECUTABLE,
            self.fixture.config.DEFAULT_MONGOD_EXECUTABLE,
        )

        if not mongod_path:
            raise errors.ServerFailure("could not determine a mongod executable")

        if not os.path.isfile(mongod_path) or not os.access(mongod_path, os.X_OK):
            raise errors.ServerFailure(f"mongod is not executable: {mongod_path}")

        workdir = get_expansion("workdir")
        pathname = "evergreen/verify_extension_visibility_test.sh"
        verifier_script_path = os.path.join(workdir, "src", pathname) if workdir else pathname

        so_files = find_extension_so_files(
            is_evergreen=bool(self.fixture.config.EVERGREEN_TASK_ID),
            logger=self.logger,
        )

        failures = []
        for so in so_files:
            self.logger.info("Verifying extension visibility: %s", so)
            proc = subprocess.run(
                [verifier_script_path, mongod_path, so],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            if proc.returncode != 0:
                self.logger.error("Verifier failed for %s\n%s", so, proc.stdout)
                failures.append(so)

        if failures:
            raise errors.ServerFailure(f"Extension visibility verification failed for: {failures}")
