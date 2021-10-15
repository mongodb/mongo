"""An interface to control a replicator in our test infrastructure."""


class ReplicatorInterface(object):
    """A replicator interface.

    Provides a way to start, pause, resume and stop the replicator.
    """

    def __init__(self, logger, fixture):
        """Initialize the replicator."""
        self.logger = logger
        self._fixture = fixture

    def start(self, start_options):
        """Start the replicator.

        This method will return after the replicator has been started.
        """

    def stop(self, stop_options):
        """Stop the replicator.

        This method will return after the replicator has been stopped, without requiring the
        replicator to have finished synchronizing across clusters.
        """

    def pause(self, pause_options):
        """Pause the replicator.

        This method will return after the replicator has been paused. This method requires that the
        replicator has completed synchronizing across clusters.
        """

    def resume(self, resume_options):
        """Resume the replicator.

        This method will return after the replicator has been resumed
        """
