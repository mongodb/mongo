"""The unittest.TestCase for genny."""

import os
import os.path

from . import interface
from ... import config
from ... import core
from ... import utils


class GennyTestCase(interface.ProcessTestCase):
    """A genny workload to execute."""

    REGISTERED_NAME = "genny_test"

    def __init__(self, logger, genny_workload, genny_executable=None, genny_options=None):
        """Init the GennyTestCase with the genny workload to run."""
        interface.ProcessTestCase.__init__(self, logger, "Genny workload", genny_workload)

        self.genny_executable = utils.default_if_none(config.GENNY_EXECUTABLE, genny_executable)
        self.genny_options = utils.default_if_none(genny_options, {}).copy()
        self.genny_options["workload-file"] = genny_workload

    def configure(self, fixture, *args, **kwargs):
        """Configure GennyTestCase."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)
        self.genny_options["mongo-uri"] = self.fixture.get_driver_connection_url()

        output_directory = "./genny_results"
        output_file = os.path.join(output_directory, self.short_name() + ".csv")

        try:
            os.makedirs(output_directory)
        except os.error:
            # Directory already exists
            pass

        self.genny_options.setdefault("metrics-output-file", output_file)

    def _make_process(self):
        return core.programs.genny_program(self.logger, self.genny_executable, **self.genny_options)
