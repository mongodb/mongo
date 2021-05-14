"""Validation checks for generating tasks."""
import inject
from evergreen import EvergreenApi


class GenTaskValidationService:
    """A service for validation around generating tasks."""

    @inject.autoparams()
    def __init__(self, evg_api: EvergreenApi) -> None:
        """
        Initialize the service.

        :param evg_api: Evergreen API client.
        """
        self.evg_api = evg_api

    def should_task_be_generated(self, task_id: str) -> bool:
        """
        Determine if we should attempt to generate tasks.

        If an evergreen task that calls 'generate.tasks' is restarted, the 'generate.tasks' command
        will no-op. So, if we are in that state, we should avoid generating new configuration files
        that will just be confusing to the user (since that would not be used).

        :param task_id: Id of the task being run.
        :return: Boolean of whether to generate tasks.
        """
        task = self.evg_api.task_by_id(task_id, fetch_all_executions=True)
        # If any previous execution was successful, do not generate more tasks.
        for i in range(task.execution):
            task_execution = task.get_execution(i)
            if task_execution.is_success():
                return False

        return True
