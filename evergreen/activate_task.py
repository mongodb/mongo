"""Script that activates the input task on the same variant as the running task"""

import typer

from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.util.read_config import read_config_file


def main(task_name: str):
    expansions_file = "../expansions.yml"
    expansions = read_config_file(expansions_file)
    evg_api = evergreen_conn.get_evergreen_api()

    variant_id = expansions.get("build_id")
    variant = evg_api.build_by_id(variant_id)
    found_task = None
    for task in variant.get_tasks():
        if task.display_name == task_name:
            found_task = task
            break

    is_patch = expansions.get("is_patch", False)
    if found_task:
        # In non-patch evergreen versions the task will live as not activated
        # We can just find the task and activate it if it is not activated yet
        if found_task.activated:
            return

        evg_api.configure_task(found_task.task_id, activated=True)
    elif is_patch:
        # Evergreen patches work differently than other evergreen versions
        # When a task is not scheduled initially it does not exist as an unscheduled task
        # So we need to use a different path in the api to schedule the task
        patch_id = expansions.get("version_id")
        build_variant = expansions.get("build_variant")
        evg_api.configure_patch(patch_id, [{"id": build_variant, "tasks": [task_name]}])
    else:
        raise RuntimeError(
            f"The {task_name} task could not be found in the {build_variant} variant"
        )


if __name__ == "__main__":
    typer.run(main)
