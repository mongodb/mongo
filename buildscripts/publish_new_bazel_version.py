import argparse
import os
import shutil
import subprocess
import urllib.request

BAZEL_VERSION = "7.5.0"
BASE_URL = "https://github.com/bazelbuild/bazel/releases/download"
S3_BUCKEt_URL = "s3://mdb-build-public/bazel-binaries"


def main():
    parser = argparse.ArgumentParser(description="Publish new Bazel version to S3.")
    parser.add_argument(
        "--s390x-url", type=str, help="The URL of the s390x binary to download and re-upload"
    )
    parser.add_argument(
        "--ppc-url", type=str, help="The URL of the ppc64le binary to download and re-upload"
    )
    args = parser.parse_args()

    combinations = [
        ("darwin", "arm64"),
        ("darwin", "x86_64"),
        ("linux", "arm64"),
        ("linux", "x86_64"),
        ("windows", "x86_64"),
    ]

    if args.s390x_url:
        combinations += [("linux", "s390x")]

    if args.ppc_url:
        combinations += [("linux", "ppc64le")]

    tmpdir = "/tmp/bazel_binaries"
    os.makedirs(tmpdir, exist_ok=True)

    for os_name, arch in combinations:
        binary_name = f"bazel-{BAZEL_VERSION}-{os_name}-{arch}"
        if os_name == "windows":
            binary_name += ".exe"
        if arch == "s390x":
            url = args.s390x_url
        elif arch == "ppc64le":
            url = args.ppc_url
        else:
            url = f"{BASE_URL}/{BAZEL_VERSION}/{binary_name}"
        local_path = os.path.join(tmpdir, binary_name)

        print(f"Downloading {url}...")
        urllib.request.urlretrieve(url, local_path)

        print(f"Uploading {local_path} to {S3_BUCKEt_URL}/{BAZEL_VERSION}/{binary_name}...")
        subprocess.run(
            ["aws", "s3", "cp", local_path, f"{S3_BUCKEt_URL}/{BAZEL_VERSION}/{binary_name}"],
            check=True,
        )

        sha256_hash = subprocess.run(
            ["sha256sum", local_path], capture_output=True, text=True, check=True
        ).stdout.split()[0]

        sha256_file_path = f"{local_path}.sha256"
        with open(sha256_file_path, "w", encoding="utf-8") as sha256_file:
            sha256_file.write(sha256_hash)

        print(
            f"Uploading {sha256_file_path} to {S3_BUCKEt_URL}/{BAZEL_VERSION}/{binary_name}.sha256..."
        )
        subprocess.run(
            [
                "aws",
                "s3",
                "cp",
                sha256_file_path,
                f"{S3_BUCKEt_URL}/{BAZEL_VERSION}/{binary_name}.sha256",
            ],
            check=True,
        )

    shutil.rmtree(tmpdir, ignore_errors=True)

    print("All binaries downloaded and uploaded successfully.")


if __name__ == "__main__":
    main()
