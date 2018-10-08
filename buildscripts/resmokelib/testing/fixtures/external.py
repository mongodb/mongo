"""External fixture for executing JSTests against."""

from __future__ import absolute_import

from . import interface


class ExternalFixture(interface.Fixture):
    """Fixture which provides JSTests capability to connect to external (non-resmoke) cluster."""

    def __init__(self, logger, job_num, shell_conn_string=None):
        """Initialize ExternalFixture."""
        interface.Fixture.__init__(self, logger, job_num)

        if shell_conn_string is None:
            raise ValueError("The ExternalFixture must be specified with the resmoke option"
                             " --shellConnString or --shellPort")

        self.shell_conn_string = shell_conn_string

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        return self.shell_conn_string

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        return "mongodb://" + self.get_internal_connection_string()
