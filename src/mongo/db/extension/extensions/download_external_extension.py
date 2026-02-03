import argparse
import hashlib
import io
import os
import shutil
import sys
import tarfile

import requests
from retry import retry
import yaml


def get_extension_conf(file_path, extension_name):
    with open(file_path, "r") as file:
        data = yaml.safe_load(file)
    return data["extensions"][extension_name]


@retry(tries=3)
def get_tarball(base_url, name, version, variant, checksum):
    url = f"{base_url}{name.replace('_', '-')}-{version}-{variant}.tgz"
    response = requests.get(url)
    assert hashlib.sha256(response.content).hexdigest() == checksum
    tarball = tarfile.open(fileobj=io.BytesIO(response.content), mode="r:gz")

    return tarball


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--extension-name", required=True)
    parser.add_argument("--conf-file", required=True)
    parser.add_argument("--variant", required=True)
    parser.add_argument("--archive-files", required=True, nargs="+")
    parser.add_argument("--outputs", required=True, nargs="+")
    args = parser.parse_args(sys.argv[1:])

    conf_data = get_extension_conf(args.conf_file, args.extension_name)

    checksum = conf_data["variants"][args.variant]

    tarball = get_tarball(
        conf_data["base_url"], args.extension_name, conf_data["version"], args.variant, checksum
    )

    basename_to_output = dict(zip(args.archive_files, args.outputs))

    for tf in tarball.getmembers():
        base = os.path.basename(tf.name)
        if base in basename_to_output:
            output = basename_to_output[base]

            with open(output, "wb") as stream:
                archive_file_stream = tarball.extractfile(tf)
                shutil.copyfileobj(archive_file_stream, stream)
                archive_file_stream.close()
