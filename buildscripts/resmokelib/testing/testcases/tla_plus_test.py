"""The unittest.TestCase for model-checking TLA+ specifications."""

import os

from . import interface
from ... import core


class TLAPlusTestCase(interface.ProcessTestCase):
    """A TLA+ specification to model-check."""

    REGISTERED_NAME = "tla_plus_test"

    def __init__(self, logger, model_config_file):
        """Initialize the TLAPlusTestCase with a TLA+ model config file.

        model_config_file is the full path to a file like
        src/mongo/db/repl/tla_plus/MongoReplReconfig/MCMongoReplReconfig.cfg.
        """
        message = f"Path '{model_config_file}' doesn't" \
                  f" match **/<SpecName>/MC<SpecName>.cfg"

        # spec_dir should be like src/mongo/db/repl/tla_plus/MongoReplReconfig.
        spec_dir, filename = os.path.split(model_config_file)
        if not (spec_dir and filename):
            raise ValueError(message)

        # working_dir is like src/mongo/db/repl/tla_plus.
        self.working_dir, specname = os.path.split(spec_dir)
        if not specname or filename != f'MC{specname}.cfg':
            raise ValueError(message)

        interface.ProcessTestCase.__init__(self, logger, "TLA+ test", specname)

    def _make_process(self):
        return core.programs.generic_program(self.logger, ["sh", "model-check.sh", self.test_name],
                                             process_kwargs={"cwd": self.working_dir})
