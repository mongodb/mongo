import os
import sys
from urllib.request import urlretrieve

sys.path.append(os.path.join(os.path.dirname(__file__), "."))
from download_archive_dist_test_debug import get_task_id

from buildscripts.resmokelib.utils import evergreen_conn


def main():
    evg_config = os.path.expanduser(os.path.join("~", ".evergreen.yml"))
    evg_api = evergreen_conn.get_evergreen_api(evergreen_config=evg_config)
    task_id = get_task_id(evg_api)

    task = evg_api.task_by_id(task_id)
    tasks_in_variant = evg_api.tasks_by_build(task.build_id)

    resmoke_tests_task = list(filter(lambda t: t.display_name == "resmoke_tests", tasks_in_variant))
    assert len(resmoke_tests_task) == 1, "Could not find a unique resmoke_tests task"
    resmoke_tests_task = resmoke_tests_task[0]

    output_dir = "/data/mci/artifacts-resmoke_tests"
    os.mkdir(output_dir)

    resmoke_tests_task_id = resmoke_tests_task.task_id
    resmoke_tests_task = evg_api.task_by_id(resmoke_tests_task_id)
    for artifact in resmoke_tests_task.artifacts:
        if not artifact.name.startswith("Test binaries and libraries"):
            continue

        urlretrieve(artifact.url, f"{output_dir}/mongo-tests-resmoke_tests.tgz")
        return


if __name__ == "__main__":
    main()
