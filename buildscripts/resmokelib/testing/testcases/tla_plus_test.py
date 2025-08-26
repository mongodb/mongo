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
        model_check_command: Optional[str] = "sh model-check.sh",
    ):
        """Initialize the TLAPlusTestCase with a TLA+ model config file.

        model_config_file is the full path to a file like
        src/mongo/tla_plus/**/MCMongoReplReconfig.cfg.

        java_binary is the full path to the "java" program, or None.
        """

        assert len(model_config_files) == 1
        message = f"Path '{model_config_files[0]}' doesn't" f" match **/<SpecName>/MC<SpecName>.cfg"

        # working_dir is always src/mongo/tla_plus, which contains 'model-check.sh' and subfolders
        # organizing specifications per component.
        self.working_dir = "src/mongo/tla_plus/"
        cfg_relpath = os.path.relpath(model_config_files[0], self.working_dir)

        # cfg_relpath should be like **/MongoReplReconfig/MCMongoReplReconfig.cfg
        spec_dir, filename = os.path.split(cfg_relpath)
        specname = os.path.split(spec_dir)[1]
        if not (spec_dir and filename) or filename != f"MC{specname}.cfg":
            raise ValueError(message)

        self.java_binary = java_binary

        self.model_check_command = model_check_command

        interface.ProcessTestCase.__init__(self, logger, "TLA+ test", spec_dir)

    def _make_process(self):
        process_kwargs = {"cwd": self.working_dir}
        if self.java_binary is not None:
            process_kwargs["env_vars"] = {"JAVA_BINARY": self.java_binary}

        interface.append_process_tracking_options(process_kwargs, self._id)

        return core.programs.generic_program(
            self.logger,
            [self.model_check_command, self.test_name],
            process_kwargs=process_kwargs,
        )
