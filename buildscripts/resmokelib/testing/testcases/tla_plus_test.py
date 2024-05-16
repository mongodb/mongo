"""The unittest.TestCase for model-checking TLA+ specifications."""

import os
from typing import Optional

from buildscripts.resmokelib import core, logging
from buildscripts.resmokelib.testing.testcases import interface


class TLAPlusTestCase(interface.ProcessTestCase):
    """A TLA+ specification to model-check."""

    REGISTERED_NAME = "tla_plus_test"

    def __init__(
        self,
        logger: logging.Logger,
        model_config_files: list[str],
        java_binary: Optional[str] = None,
    ):
        """Initialize the TLAPlusTestCase with a TLA+ model config file.

        model_config_file is the full path to a file like
        src/mongo/tla_plus/MongoReplReconfig/MCMongoReplReconfig.cfg.

        java_binary is the full path to the "java" program, or None.
        """

        assert len(model_config_files) == 1
        message = f"Path '{model_config_files[0]}' doesn't" f" match **/<SpecName>/MC<SpecName>.cfg"

        # spec_dir should be like src/mongo/tla_plus/MongoReplReconfig.
        spec_dir, filename = os.path.split(model_config_files[0])
        if not (spec_dir and filename):
            raise ValueError(message)

        # working_dir is like src/mongo/tla_plus.
        self.working_dir, specname = os.path.split(spec_dir)
        if not specname or filename != f"MC{specname}.cfg":
            raise ValueError(message)

        self.java_binary = java_binary

        interface.ProcessTestCase.__init__(self, logger, "TLA+ test", specname)

    def _make_process(self):
        process_kwargs = {"cwd": self.working_dir}
        if self.java_binary is not None:
            process_kwargs["env_vars"] = {"JAVA_BINARY": self.java_binary}

        return core.programs.generic_program(
            self.logger, ["sh", "model-check.sh", self.test_name], process_kwargs=process_kwargs
        )
