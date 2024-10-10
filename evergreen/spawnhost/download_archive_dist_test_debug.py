import collections
import os
import time
from urllib.request import urlretrieve

import requests

from buildscripts.resmokelib.utils import evergreen_conn


def main():
    data_dir = "/data/mci"
    dirs = os.listdir(data_dir)
    contains_symbols = False
    contains_binaries = False
    for dir in dirs:
        if "archive_dist_test_debug" in dir:
            contains_symbols = True
        elif "archive_dist_test" in dir:
            contains_binaries = True

    # early return if symbols already exist on disk
    # early return if there are not binaries (which means this is not a resmoke task)
    if contains_symbols or not contains_binaries:
        return

    # some ec2 instances use a newer version of the metadata service.
    # we try to first use the newer version that required a token
    # if that doesn't work we then fallback to the unauthenticated version of the api
    instance_token_endpoint = "http://169.254.169.254/latest/api/token"
    instance_id_endpoint = "http://169.254.169.254/latest/meta-data/instance-id"
    response = requests.put(
        instance_token_endpoint, headers={"X-aws-ec2-metadata-token-ttl-seconds": "300"}
    )
    if response.ok:
        auth_token = response.content.decode("utf-8")
        response = requests.get(
            instance_id_endpoint, headers={"X-aws-ec2-metadata-token": auth_token}
        )
    else:
        response = requests.get(instance_id_endpoint)
    if not response.ok:
        raise RuntimeError(f"Could not query the instance endpoint: {response.status_code}")
    instance_id = response.content.decode("utf-8")
    evg_config = os.path.expanduser(os.path.join("~", ".evergreen.yml"))
    evg_api = evergreen_conn.get_evergreen_api(evergreen_config=evg_config)
    host = evg_api.host_by_id(instance_id)
    task_id = host.json["provision_options"]["task_id"]

    compile_tasks = evergreen_conn._filter_successful_tasks(evg_api, collections.deque([task_id]))
    debugsymbols_task = compile_tasks.symbols_task
    if debugsymbols_task is None:
        raise RuntimeError("Could not find debugsymbols task")

    debugsymbols_task_id = debugsymbols_task.task_id

    start_time = time.time()
    timeout = 60 * 45  # 45 mins
    output_dir = "/data/mci/artifacts-archive_dist_test_debug"
    os.mkdir(output_dir)

    while time.time() - start_time < timeout:
        debugsymbols_task = evg_api.task_by_id(debugsymbols_task_id)
        if debugsymbols_task.status != "success" and debugsymbols_task != "failed":
            print("Waiting for debugsymbols task to finish")
            time.sleep(10)
            continue

        for artifact in debugsymbols_task.artifacts:
            if not artifact.name.startswith("mongo-debugsymbols"):
                continue

            ext = artifact.name.split(".")[-1]
            urlretrieve(artifact.url, f"{output_dir}/debugsymbols-manually-downloaded.{ext}")
            return

    raise RuntimeError("Error occured while trying to download debugsymbols.")


if __name__ == "__main__":
    main()
