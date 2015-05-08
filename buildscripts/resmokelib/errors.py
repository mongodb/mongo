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
