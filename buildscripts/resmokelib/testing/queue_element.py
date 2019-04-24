"""Queue entry interface."""


def queue_elem_factory(testcase, test_config, suite_options):
    """
    Create the appropriate queue element based on suite_options given.

    :param testcase: Test case to be run.
    :param test_config: Configuration for test case.
    :param suite_options: Configuration for test suite.
    :return: QueueElem representing the testcase.
    """
    if suite_options.time_repeat_tests_secs:
        return QueueElemRepeatTime(testcase, test_config, suite_options)
    return QueueElem(testcase, test_config, suite_options)


class QueueElem(object):
    """Base class for an element on the queue."""

    def __init__(self, testcase, test_config, _):
        """
        Initialize QueueElemRepeatNum class.

        :param testcase: Test case to be run.
        :param test_config: Configuration for test case.
        :return: QueueElementRepeatNum representing the testcase.
        """
        self.testcase = testcase
        self.test_config = test_config

    def job_completed(self, job_time):
        """
        Call when an execution has completed.

        :param job_time: The amount of time the job ran for.
        """
        pass

    @staticmethod
    def should_requeue():
        """Return True if the queue element should be requeued."""
        return False


class QueueElemRepeatTime(QueueElem):
    """Queue element for repeat time."""

    def __init__(self, testcase, config, suite_options):
        """Initialize QueueElemRepeatTime class."""
        super(QueueElemRepeatTime, self).__init__(testcase, config, suite_options)
        self.repeat_num_min = suite_options.num_repeat_tests_min
        self.repeat_num_max = suite_options.num_repeat_tests_max
        self.repeat_secs = suite_options.time_repeat_tests_secs
        self.repeat_time_elapsed = 0
        self.repeat_num = 0

    def job_completed(self, job_time):
        """
        Call when an execution has completed, update the run statistics.

        :param job_time: The amount of time the job ran for.
        """
        self.repeat_num += 1
        self.repeat_time_elapsed += job_time

    def _still_need_minimum_runs(self):
        """
        Determine if this element has been run the minimum number of times specified.

        :return: True if the element has not hit the minimum and should be requeued.
        """
        return self.repeat_num_min and self.repeat_num < self.repeat_num_min

    def _have_max_runs_been_satisfied(self):
        """
        Determine if this element has been run the maximum number of times.

        :return: True if the element has been run the maximum number of times.
        """
        return self.repeat_num_max and self.repeat_num >= self.repeat_num_max

    def _has_min_runtime_been_satisfied(self):
        """
        Determine if this element has been run the minimum runtime.

        :return: True if the element has not hit the minimum runtime and should be requeued.
        """
        return self.repeat_time_elapsed >= self.repeat_secs

    def should_requeue(self):
        """Determine if this elem should be requeued."""
        if self._still_need_minimum_runs():
            return True

        if self._have_max_runs_been_satisfied():
            return False

        if self._has_min_runtime_been_satisfied():
            return False

        return True
