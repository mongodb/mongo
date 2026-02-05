import os
import sys
from urllib.request import urlretrieve

sys.path.append(os.path.join(os.path.dirname(__file__), "."))
from download_archive_dist_test_debug import get_task_id

from buildscripts.resmokelib.hang_analyzer.extractor import find_test_task_with_binaries
from buildscripts.resmokelib.utils import evergreen_conn


def main():
    evg_config = os.path.expanduser(os.path.join("~", ".evergreen.yml"))
    evg_api = evergreen_conn.get_evergreen_api(evergreen_config=evg_config)
    task_id = get_task_id(evg_api)

    resmoke_tests_task = find_test_task_with_binaries(evg_api, task_id)

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
