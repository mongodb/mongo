import argparse
import concurrent.futures
import datetime
import os
import re
import sys

from dateutil import tz

import evergreen
from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.util.read_config import read_config_file

timeouts_without_dumps = set()
passed_with_dumps = set()
evg_api = evergreen_conn.get_evergreen_api()


def process_version(version: evergreen.Version) -> None:
    for build_variant_status in version.build_variants_status:
        build_variant = build_variant_status.get_build()
        if not build_variant.activated:
            continue

        build_variant_name = build_variant.build_variant
        unsupported_variants = ["tsan", "asan", "aubsan", "macos"]
        if any(
            unsupported_variant in build_variant_name
            for unsupported_variant in unsupported_variants
        ):
            continue

        for task_id in build_variant.tasks:
            task = evg_api.task_by_id(task_id)
            if not task.activated:
                continue
            has_core_dump = False
            is_resmoke_task = False
            for artifact in task.artifacts:
                if "Core Dump" in artifact.name:
                    has_core_dump = True
                elif artifact.name == "Resmoke.py Invocation for Local Usage":
                    is_resmoke_task = True

            if not has_core_dump and task.is_timeout() and is_resmoke_task:
                timeouts_without_dumps.add(task_id)

            if task.is_success() and task.display_status == "success" and has_core_dump:
                passed_with_dumps.add(task_id)


def main(expansions_file: str, output_file: str) -> int:
    expansions = read_config_file(expansions_file)
    current_task_id = expansions.get("task_id", None)
    is_patch = expansions.get("is_patch", False)

    today = datetime.datetime.utcnow().date()
    start_of_today = datetime.datetime(today.year, today.month, today.day, tzinfo=tz.UTC)

    # STM daily cron runs everyday at 4 AM
    # We scan the day before yesterday so we do not have to worry about in progress tasks assuming
    # that a version will finish within 28 hours
    start_of_window = start_of_today - datetime.timedelta(days=2)
    end_of_window = start_of_today - datetime.timedelta(days=1)

    # We only care maintaining alerts as of v7.2
    mongo_projects = ["mongodb-mongo-master", "mongodb-mongo-master-nightly"]
    for project in evg_api.all_projects():
        if not project.enabled:
            continue
        regex = re.match(r"mongodb-mongo-v([0-9]{1,3})\.([0-9])", project.identifier)
        if not regex:
            continue

        major_version = int(regex.group(1))
        minor_version = int(regex.group(2))
        if major_version > 7 or (major_version == 7 and minor_version >= 2):
            mongo_projects.append(project.id)

    cores = os.cpu_count()
    with concurrent.futures.ThreadPoolExecutor(max_workers=cores) as executor:
        futures = []
        for project in mongo_projects:
            patches = evg_api.patches_by_project_time_window(
                project, end_of_window, start_of_window
            )
            # This covers all user patches generated with `evergreen patch`
            for patch in patches:
                if not patch.activated:
                    continue
                version = evg_api.version_by_id(patch.patch_id)
                futures.append(executor.submit(process_version, version=version))

            # This covers all automated versions that are run by crons and other stuff
            for requester in ["adhoc", "gitter_request", "github_pull_request"]:
                for version in evg_api.versions_by_project_time_window(
                    project, end_of_window, start_of_window, requester
                ):
                    futures.append(executor.submit(process_version, version=version))

        concurrent.futures.wait(futures)
    errors = []
    if timeouts_without_dumps:
        errors.append("ERROR: The following tasks timed out without core dumps uploaded:")
        errors.extend(
            [f"https://spruce.mongodb.com/task/{task_id}" for task_id in timeouts_without_dumps]
        )

    if passed_with_dumps:
        errors.append("ERROR: The following tasks had core dumps uploaded while being successful:")
        errors.extend(
            [f"https://spruce.mongodb.com/task/{task_id}" for task_id in passed_with_dumps]
        )

    if not errors:
        return 0

    error_str = "\n".join(errors)
    print(error_str, file=sys.stderr)

    with open(output_file, "w") as file:
        file.write(error_str)

    month = start_of_window.month
    day = start_of_window.day
    year = start_of_window.year
    msg = [
        f"The daily task scanner has come across some issues for versions on {month}/{day}/{year}"
    ]

    if timeouts_without_dumps:
        msg.append(
            f"- {len(timeouts_without_dumps)} task(s) timed out without core dumps being uploaded"
        )

    if passed_with_dumps:
        msg.append(
            f"- {len(passed_with_dumps)} task(s) had core dumps uploaded while being successful"
        )

    msg.append(
        f"For more details view the `Task Errors` file <https://spruce.mongodb.com/task/{current_task_id}/files|here>."
    )

    if not is_patch:
        evg_api.send_slack_message(
            target="#sdp-test-alerts",  # TODO SERVER-83205: change to #sdp-triager
            msg="\n".join(msg),
        )

    # TODO SERVER-83205: change to return 1
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        prog="DailyTaskScanner",
        description="Iterates over all of the tasks in mongodb-mongo-master and "
        "mongodb-mongo-master-nightly over a day and looks for certain errors to send "
        "alerts of.",
    )

    parser.add_argument(
        "--expansions-file",
        "-e",
        help="Expansions file to read task info from.",
        default="../expansions.yml",
    )
    parser.add_argument(
        "--output-file", "-f", help="File to output errors to.", default="task_errors.txt"
    )
    args = parser.parse_args()
    sys.exit(main(args.expansions_file, args.output_file))
