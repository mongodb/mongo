import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

import requests


def main() -> int:
    parser = argparse.ArgumentParser(prog="Build Bazel", description="Build Bazel from source code")

    parser.add_argument("--version", help="version to build", required=True, type=str)

    args = parser.parse_args()

    download_url = f"https://github.com/bazelbuild/bazel/releases/download/{args.version}/bazel-{args.version}-dist.zip"
    print("Downloading Bazel source code from", download_url)

    result = requests.get(
        download_url,
        timeout=60,
    )
    if result.status_code != 200:
        print(f"Error fetching source zip: {result.status_code}")
        return 1

    with tempfile.TemporaryDirectory() as temp_dir:
        print("Unzipping into", str(temp_dir))

        temp_zip_path = os.path.join(temp_dir, "bazel-dist.zip")
        with open(temp_zip_path, "wb") as temp_zip_file:
            temp_zip_file.write(result.content)

        with zipfile.ZipFile(temp_zip_path, "r") as zip_ref:
            zip_ref.extractall(temp_dir)

        compile_script = os.path.join(temp_dir, "compile.sh")
        print("Running", str(compile_script))

        os.system("chmod -R 755 " + temp_dir)

        subprocess.run(
            [compile_script],
            cwd=temp_dir,
            check=True,
            env={
                **os.environ.copy(),
                **{
                    "JAVA_HOME": "/usr/lib/jvm/java-21-openjdk",
                    "EXTRA_BAZEL_ARGS": "--tool_java_runtime_version=local_jdk",
                },
            },
        )

        shutil.copyfile(os.path.join(temp_dir, "output/bazel"), "bazel_binary")

    return 0


if __name__ == "__main__":
    sys.exit(main())
