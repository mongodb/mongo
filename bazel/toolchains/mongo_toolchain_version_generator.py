# Use to update mongo_toolchain_version.bzl with hashes of a new toolchain from the toolchain-builder project.

import argparse
import hashlib
import json
import os
import pathlib
import tempfile
import urllib.request

def sha256_file(filename: str) -> str:
    sha256_hash = hashlib.sha256()
    with open(filename, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(block)
        return sha256_hash.hexdigest()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("patch_build_id",
                        help="Patch build id from toolchain-builder project.")
    parser.add_argument("patch_build_date_string",
                        help="Patch build date string from toolchain-builder project, get this at the task URL, ex the date is 24_01_09_16_10_07 for https://spruce.mongodb.com/task/toolchain_builder_amazon2023_compile_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07/")

    args = parser.parse_args()

    mongo_toolchain_version = {}
    version_file_path = os.path.join(pathlib.Path(__file__).parent.resolve(), "mongo_toolchain_version.bzl")
    with open(version_file_path, "r") as f:
        code = compile(f.read(), version_file_path, "exec")
        exec(code, {}, mongo_toolchain_version)
    
    for toolchain_name, toolchain in mongo_toolchain_version["TOOLCHAIN_MAP"].items():
        underscore_platform_name = toolchain['platform_name'].replace("-", "_")

        toolchain_url = mongo_toolchain_version["TOOLCHAIN_URL_FORMAT"].format(
            platform_name=toolchain["platform_name"],
            underscore_platform_name=underscore_platform_name,
            patch_build_id=args.patch_build_id,
            patch_build_date=args.patch_build_date_string,
        )
        
        temp_dir = tempfile.gettempdir()
        local_tarball_path = os.path.join(temp_dir, f"bazel_v4_toolchain_builder_{underscore_platform_name}_{args.patch_build_id}.tar.gz")

        print(f"Downloading {toolchain_url}...")

        urllib.request.urlretrieve(toolchain_url, local_tarball_path)
        sha = sha256_file(local_tarball_path)
        os.remove(local_tarball_path)

        mongo_toolchain_version["TOOLCHAIN_MAP"][toolchain_name]["sha"] = sha
        mongo_toolchain_version["TOOLCHAIN_MAP"][toolchain_name]["url"] = toolchain_url

    with open(version_file_path, "w") as f:
        print(f"Writing toolchain map to {version_file_path}...")
        print("# Use mongo/bazel/toolchains/toolchain_generator.py to generate this mapping for a given patch build.\n", file=f)
        print(f"TOOLCHAIN_URL_FORMAT = \"{mongo_toolchain_version['TOOLCHAIN_URL_FORMAT']}\"", file=f)
        print(f"TOOLCHAIN_PATCH_BUILD_ID = \"{args.patch_build_id}\"", file=f)
        print(f"TOOLCHAIN_PATCH_BUILD_DATE = \"{args.patch_build_date_string}\"", file=f)
        print("TOOLCHAIN_MAP = {", file=f)
        for key, value in sorted(mongo_toolchain_version["TOOLCHAIN_MAP"].items(), key=lambda x: x[0]): 
            print(f"    \"{key}\": {{", file=f)
            for subkey, subvalue in sorted(value.items(), key=lambda x: x[0]): 
                print(f"        \"{subkey}\": \"{subvalue}\",", file=f)
            print("    },", file=f)
        print("}", file=f)

    with open(version_file_path, "r") as f:
        print(f"Finished writing to {version_file_path}:")
        print(f.read())

if __name__ == '__main__':
    main()
