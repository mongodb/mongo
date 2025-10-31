import datetime
import os
import pathlib
import platform
import subprocess
import sys
import time

REPO_ROOT = str(pathlib.Path(__file__).parent.parent.parent)
sys.path.append(REPO_ROOT)

from bazel.wrapper_hook.wrapper_debug import wrapper_debug


def check():
    """Check if the git branch is older than 7 weeks."""

    wrapper_debug(f"Checking git branch age {REPO_ROOT}")

    if os.environ.get("CI") is None and platform.machine().lower() not in {"ppc64le", "s390x"}:
        try:
            git_log_output = subprocess.check_output(
                ["git", "log", '--pretty=format:"%ad"', "master..HEAD"], cwd=REPO_ROOT
            )
            wrapper_debug(f"Git log output: {git_log_output}")
            git_dates = git_log_output.decode("utf-8").strip().split("\n")
            if not git_dates or git_dates == [""]:
                wrapper_debug("No new commits found on the current branch compared to master.")
                return
            latest_commit_date_str = git_dates[-1]
            latest_commit_date_str = latest_commit_date_str.strip('"')
            wrapper_debug(f"Latest commit date string: {latest_commit_date_str}")
            latest_commit_date = datetime.datetime.strptime(
                latest_commit_date_str, "%a %b %d %H:%M:%S %Y %z"
            )
            current_date = datetime.datetime.now(datetime.timezone.utc)
            wrapper_debug(f"Current date: {current_date}")
            age_in_weeks = (current_date - latest_commit_date).days / 7
            wrapper_debug(f"Age in weeks: {age_in_weeks}")
            wrapper_debug(
                f"Latest commit date: {latest_commit_date_str}, Age in weeks: {age_in_weeks:.2f}"
            )
            if age_in_weeks > 2:
                print(
                    f"WARNING: The current git branch is {age_in_weeks:.2f} weeks old. "
                    "Please rebase onto the latest master branch to ensure up-to-date code and fast builds.",
                )
                time.sleep(2)
        except subprocess.CalledProcessError as e:
            wrapper_debug(f"Failed to get git log: {e}")
            return
