"""Queue entry interface."""


class QueueElemRepeatNum(object):
    """Base class for an element on the queue."""

    def __init__(self, testcase, test_config, suite_options):
        """Initialize QueueElemRepeatNum class."""
        self.testcase = testcase
        self.test_config = test_config
        self.repeat_num_min = suite_options.num_repeat_tests
        self.repeat_time_elapsed = 0
        self.repeat_num = 0

    def job_completed(self, job_time):
        """Increment values when the job completes."""
        self.repeat_num += 1
        self.repeat_time_elapsed += job_time

    def should_requeue(self):
        """Return True if the queue element should be requeued."""
        return self.repeat_num < self.repeat_num_min


class QueueElemRepeatTime(QueueElemRepeatNum):
    """Queue element for repeat time."""

    def __init__(self, testcase, config, suite_options):
        """Initialize QueueElemRepeatTime class."""
        super(QueueElemRepeatTime, self).__init__(testcase, config, suite_options)
        self.repeat_num_min = suite_options.num_repeat_tests_min
        self.repeat_num_max = suite_options.num_repeat_tests_max
        self.repeat_secs = suite_options.time_repeat_tests_secs

    def should_requeue(self):
        """Return True if the queue element should be requeued."""
        avg_time = 0 if self.repeat_num == 0 else self.repeat_time_elapsed / self.repeat_num

        # Minumim number of tests has not been run.
        if self.repeat_num_min and self.repeat_num < self.repeat_num_min:
            return True

        # Maximum number of tests has been run or elapsed time has been exceeded.
        if ((self.repeat_num_max and self.repeat_num >= self.repeat_num_max)
                or self.repeat_time_elapsed + avg_time > self.repeat_secs):
            return False

        return True
