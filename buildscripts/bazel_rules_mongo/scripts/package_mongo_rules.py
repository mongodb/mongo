import hashlib
import os
import pathlib
import shutil
import tarfile

import toml
import typer
import yaml

from buildscripts.util.expansions import get_expansion

RULE_ROOT = str(pathlib.Path(__file__).parents[1])

file_name = "bazel_rules_mongo.tar.gz"
sha256_file_name = f"{file_name}.sha256"
release_file_name = f"release_{file_name}"
release_sha256_file_name = f"release_{sha256_file_name}"
expansions_file_name = "bazel_rules_mongo_expansions.yml"


def main():
    with tarfile.open(file_name, "w:gz") as tar:
        tar.add(RULE_ROOT, os.path.basename(RULE_ROOT))

    sha256_hash = get_sha256(file_name)

    with open(sha256_file_name, "w") as file:
        file.write(sha256_hash)

    # We only make release files if it is not a patch build.
    if not get_expansion("is_patch", False):
        shutil.copy(file_name, release_file_name)
        shutil.copy(sha256_file_name, release_sha256_file_name)

    version = get_current_version()

    expansions = {
        "bazel_rules_mongo_version": version,
        "bazel_rules_mongo_sha256": sha256_hash,
        "bazel_rules_mongo_file_name": file_name,
        "bazel_rules_mongo_file_name_sha256": sha256_file_name,
        "bazel_rules_mongo_release_file_name": release_file_name,
        "bazel_rules_mongo_release_sha256_file_name": release_sha256_file_name,
    }

    with open(expansions_file_name, "w") as file:
        yaml.dump(expansions, file)


def get_sha256(file_path: str) -> str:
    sha256_hash = hashlib.sha256()
    with open(file_path, "rb") as f:
        for byte in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte)
    return sha256_hash.hexdigest()


def get_current_version():
    toml_path = os.path.join(RULE_ROOT, "pyproject.toml")
    data = toml.load(toml_path)
    return data["tool"]["poetry"]["version"]


if __name__ == "__main__":
    typer.run(main)
