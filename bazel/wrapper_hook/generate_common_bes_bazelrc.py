import base64
import hashlib
import json
import os
import pathlib
import socket
import sys

import git


def write_workstation_bazelrc(args):
    repo_root = pathlib.Path(os.path.abspath(__file__)).parent.parent.parent
    workstation_file = os.path.join(repo_root, ".bazelrc.common_bes")
    existing_hash = ""
    if os.path.exists(workstation_file):
        with open(workstation_file, encoding="utf-8") as f:
            existing_hash = hashlib.md5(f.read().encode()).hexdigest()

    status = "Unknown"
    remote = "Unknown"
    branch = "Unknown"
    commit = "Unknown"
    user = "Unknown"
    hostname = "Unknown"
    base_branch = "Unknown"
    try:
        repo = git.Repo()
    except Exception:
        print(
            "Unable to setup git repo, skipping workstation file generation. This will result in incomplete telemetry data being uploaded."
        )
    else:
        try:
            status = "clean" if repo.head.commit.diff(None) is None else "modified"
        except Exception:
            pass

        try:
            remote = repo.branches.master.repo.remote().url
            if "@" in remote:
                # If the remote is an SSH URL, we want to use the URL without the user part.
                remote = remote.split("@")[-1]
        except Exception:
            try:
                remote = repo.remotes[0].url
            except Exception:
                pass

        try:
            branch = repo.active_branch.name
        except Exception:
            pass

        try:
            commit = repo.commit("HEAD")
        except Exception:
            pass

        try:
            reader = repo.config_reader()
            user = reader.get_value("user", "email")
        except Exception:
            pass

    if os.environ.get("CI") is not None:
        user = os.environ.get("author_email", "Unknown")
        # This is the branch that the PR is merging into
        base_branch = os.environ.get("github_pr_base_branch", "Unknown")
        # Replace the branch with the head branch if this is a PR check / merge queue run
        branch = os.environ.get("github_pr_head_branch", branch)

    try:
        hostname = socket.gethostname()
    except Exception:
        pass

    filtered_args = args[1:]
    if "--" in filtered_args:
        filtered_args = filtered_args[:filtered_args.index("--")] + ["--", "(REDACTED)"]

    developer_build = os.environ.get("CI") is None
    b64_cmd_line = base64.b64encode(json.dumps(filtered_args).encode()).decode()
    normalized_os = sys.platform.replace("win32", "windows").replace("darwin", "macos")
    bazelrc_contents = f"""\
# Generated file, do not modify
common --bes_keywords=developerBuild={developer_build}
common --bes_keywords=user_email={user}
common --bes_keywords=operating_system={normalized_os}
common --bes_keywords=engflow:BuildScmRemote={remote}
common --bes_keywords=engflow:BuildScmBranch={branch}
common --bes_keywords=engflow:BuildScmRevision={commit}
common --bes_keywords=engflow:BuildScmStatus={status}
common --bes_keywords=rawCommandLineBase64={b64_cmd_line}
common --bes_keywords=base_branch={base_branch}
"""

    if developer_build:
        bazelrc_contents += f"common --bes_keywords=workstation={hostname}{os.linesep}"

        # Boost the remote execution priority on developer workstation builds for lower
        # queue times.
        bazelrc_contents += "common --remote_execution_priority=5"

    current_hash = hashlib.md5(bazelrc_contents.encode()).hexdigest()
    if existing_hash != current_hash:
        with open(workstation_file, "w", encoding="utf-8") as f:
            f.write(bazelrc_contents)
