"""External fixture for executing JSTests against."""

from . import interface


class ExternalFixture(interface.Fixture):
    """Fixture which provides JSTests capability to connect to external (non-resmoke) cluster."""

    def pids(self):
        """:return: no pids are owned by this fixture."""
        return []

    def __init__(self, logger, job_num, shell_conn_string=None):
        """Initialize ExternalFixture."""
        interface.Fixture.__init__(self, logger, job_num)

        if shell_conn_string is None:
            raise ValueError("The ExternalFixture must be specified with the resmoke option"
                             " --shellConnString or --shellPort")

        self.shell_conn_string = shell_conn_string

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        # Reconfiguring the external fixture isn't supported so there's no reason to attempt to
        # parse the mongodb:// connection string the user specified via the command line into the
        # internal format used by the server.
        raise NotImplementedError("ExternalFixture can only be used with a MongoDB connection URI")

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        return self.shell_conn_string
