"""
Exceptions raised by resmoke.py.
"""


class ResmokeError(Exception):
    """
    Base class for all resmoke.py exceptions.
    """
    pass


class StopExecution(ResmokeError):
    """
    Exception that is raised when resmoke.py should stop executing tests
    if failing fast is enabled.
    """
    pass


class UserInterrupt(StopExecution):
    """
    Exception that is raised when a user signals resmoke.py to
    unconditionally stop executing tests.
    """
    pass


class TestFailure(ResmokeError):
    """
    Exception that is raised by a hook in the after_test method if it
    determines the the previous test should be marked as a failure.
    """
    pass


class ServerFailure(TestFailure):
    """
    Exception that is raised by a hook in the after_test method if it
    detects that the fixture did not exit cleanly and should be marked
    as a failure.
    """
    pass


class PortAllocationError(ResmokeError):
    """
    Exception that is raised by the PortAllocator if a port is requested
    outside of the range of valid ports, or if a fixture requests more
    ports than were reserved for that job.
    """
    pass
