import hashlib
import os
import pathlib
import subprocess


def write_mongo_version_bazelrc(args):

    # quick return don't waste time if set on command line
    if any(["MONGO_VERSION" in arg for arg in args]):
        return
    

    repo_root = pathlib.Path(os.path.abspath(__file__)).parent.parent.parent
    version_file = os.path.join(repo_root, ".bazelrc.mongo_version")
    existing_hash = ""
    if os.path.exists(version_file):
        with open(version_file, encoding="utf-8") as f:
            existing_hash = hashlib.md5(f.read().encode()).hexdigest()


    proc = subprocess.run(["git", "describe", "--abbrev=0"], capture_output=True, text=True)
    bazelrc_contents = f"""
common --define=MONGO_VERSION={proc.stdout.strip()[1:]}
"""
    current_hash = hashlib.md5(bazelrc_contents.encode()).hexdigest()
    if existing_hash != current_hash:
        with open(version_file, "w", encoding="utf-8") as f:
            f.write(bazelrc_contents)
