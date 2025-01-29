import base64
import hashlib
import json
import os
import socket

import git


def write_workstation_bazelrc(args):
    workstation_file = ".bazelrc.common_bes"
    existing_hash = ""
    if os.path.exists(workstation_file):
        with open(workstation_file) as f:
            existing_hash = hashlib.md5(f.read().encode()).hexdigest()

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
            status = "Unknown"

        try:
            remote = repo.branches.master.repo.remote().url
        except Exception:
            try:
                remote = repo.remotes[0].url
            except Exception:
                remote = "Unknown"

        try:
            branch = repo.active_branch.name
        except Exception:
            branch = "Unknown"

        try:
            commit = repo.commit("HEAD")
        except Exception:
            commit = "Unknown"

        try:
            reader = repo.config_reader()
            user = reader.get_value("user", "email")
        except Exception:
            user = "Unknown"

    if os.environ.get("CI") is not None:
        user = os.environ.get("author_email", "Unknown")

    try:
        hostname = socket.gethostname()
    except Exception:
        hostname = "Unknown"

    developer_build = os.environ.get("CI") is None
    b64_cmd_line = base64.b64encode(json.dumps(args[1:]).encode()).decode()
    bazelrc_contents = f"""\
# Generated file, do not modify
common --bes_keywords=developerBuild={developer_build}
common --bes_keywords=user_email={user}
common --bes_keywords=engflow:BuildScmRemote={remote}
common --bes_keywords=engflow:BuildScmBranch={branch}
common --bes_keywords=engflow:BuildScmRevision={commit}
common --bes_keywords=engflow:BuildScmStatus={status}
common --bes_keywords=rawCommandLineBase64={b64_cmd_line}
"""

    if developer_build:
        bazelrc_contents += f"common --bes_keywords=workstation={hostname}{os.linesep}"

    current_hash = hashlib.md5(bazelrc_contents.encode()).hexdigest()
    if existing_hash != current_hash:
        print(f"Generating new {workstation_file} file...")
        with open(workstation_file, "w") as f:
            f.write(bazelrc_contents)
