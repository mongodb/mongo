"""
unittest.TestCase for sleeping a given amount of time.
"""

from __future__ import absolute_import

import time

from . import interface


class SleepTestCase(interface.TestCase):

    REGISTERED_NAME = "sleep_test"

    def __init__(self, logger, sleep_duration_secs):
        """
        Initializes the SleepTestCase with the duration to sleep for.
        """

        sleep_duration_secs = int(sleep_duration_secs)

        interface.TestCase.__init__(
            self, logger, "Sleep", "{:d} seconds".format(sleep_duration_secs))

        self.__sleep_duration_secs = sleep_duration_secs

    def run_test(self):
        time.sleep(self.__sleep_duration_secs)
        self.return_code = 0

    def as_command(self):
        """
        Returns the command invocation used to run the test.
        """
        return "sleep {:d}".format(self.__sleep_duration_secs)
