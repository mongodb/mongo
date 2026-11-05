import hashlib
import os
import pathlib
import sys


def write_wrapper_hook_bazelrc(args):
    python = sys.executable
    workspace_status = os.path.join("bazel", "workspace_status.py")
    if sys.platform == "win32":
        # Bazel processes the workspace_status_command, breaking Windows
        # paths. Add escaping so that the bazelrc contents is like:
        # --workspace_status_command="Z:\\tmp\\python.exe bazel\\workspace_status.py"
        python = python.replace("\\", "\\\\")
        workspace_status = workspace_status.replace("\\", "\\\\")

    repo_root = pathlib.Path(os.path.abspath(__file__)).parent.parent.parent
    version_file = os.path.join(repo_root, ".bazelrc.wrapper_hook")
    existing_hash = ""
    if os.path.exists(version_file):
        with open(version_file, encoding="utf-8") as f:
            existing_hash = hashlib.md5(f.read().encode()).hexdigest()

    bazelrc_contents = f'build --workspace_status_command="{python} {workspace_status}"\n'

    current_hash = hashlib.md5(bazelrc_contents.encode()).hexdigest()
    if existing_hash != current_hash:
        with open(version_file, "w", encoding="utf-8") as f:
            f.write(bazelrc_contents)
