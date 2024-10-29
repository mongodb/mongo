import glob
import os
import sys
import tarfile


def main():
    data_dir = "/data/mci"
    artifact_files = glob.glob("artifacts*archive_dist_test/artifacts*.tgz", root_dir=data_dir)
    if len(artifact_files) > 1:
        raise RuntimeError("More than one artifacts file found")

    if len(artifact_files) == 0:
        print("No artifacts file found, this was probably not generated from a resmoke task.")
        return 0

    home_dir = os.environ.get("HOME", None)
    if not home_dir:
        raise RuntimeError("HOME env var could not be found")

    artifact_path = os.path.join(data_dir, artifact_files[0])
    with tarfile.open(artifact_path, "r") as tar:
        tar.extractall(home_dir)

    return 0


if __name__ == "__main__":
    sys.exit(main())
