#!/usr/bin/env python3
"""Download the binaries from a previous sys-perf run."""

import argparse

import requests

BASE_URI = "https://evergreen.mongodb.com/rest/v2/"


def _get_auth_headers(evergreen_api_user, evergreen_api_key):
    return {
        "Api-User": evergreen_api_user,
        "Api-Key": evergreen_api_key,
    }


def _get_build_id(build_variant_name, version_id, auth_headers):
    url = BASE_URI + "versions/" + version_id
    response = requests.get(url, headers=auth_headers)
    if response.status_code != 200:
        raise ValueError("Invalid version_id:", version_id)

    version_json = response.json()
    build_variants = version_json["build_variants_status"]
    for build_variant in build_variants:
        if build_variant["build_variant"] == build_variant_name:
            return build_variant["build_id"]

    raise RuntimeError(
        "The compile-variant "
        + build_variant_name
        + " does not exist for the build with version_id "
        + version_id
    )


# All of our sys-perf compile variants have exactly one task,
# so we can safely select the first one from the build variants tasks.
def _get_task_id(build_id, auth_headers):
    url = BASE_URI + "builds/" + build_id
    response = requests.get(url, headers=auth_headers)
    if response.status_code != 200:
        raise RuntimeError("Unexpected error when trying to reach" + url)

    build_json = response.json()
    task_list = build_json["tasks"]
    if len(task_list) != 1:
        raise RuntimeError("Recieved unexpected tasklist:", task_list)
    return task_list[0]


# The API used here always grabs the latest execution
def _get_binary_details(task_id, auth_headers):
    url = BASE_URI + "tasks/" + task_id
    response = requests.get(url, headers=auth_headers)
    if response.status_code != 200:
        raise RuntimeError("Unexpected error when trying to reach" + url)

    task_json = response.json()
    if task_json["status"] != "success":
        raise RuntimeError("The task " + task_id + " did not run sucessfully")

    # The binary will always be the first artifact, unless we make large changes to system_perf.yml
    artifacts = task_json["artifacts"]
    if len(artifacts) > 0 and artifacts[0]["name"].startswith("mongo"):
        return artifacts[0]
    raise RuntimeError("Unexpected list of artifacts:" + artifacts)


def _get_binary_url(version_id, build_variant, evergreen_api_user, evergreen_api_key):
    auth_headers = _get_auth_headers(evergreen_api_user, evergreen_api_key)
    build_id = _get_build_id(build_variant, version_id, auth_headers)
    task_id = _get_task_id(build_id, auth_headers)
    binary_json = _get_binary_details(task_id, auth_headers)
    return binary_json["url"]


def _download_binary_file(url, save_path):
    response = requests.get(url, stream=True)
    if response.status_code == 200:
        with open(save_path, "wb") as file:
            for chunk in response.iter_content(chunk_size=1024):
                file.write(chunk)
    else:
        raise RuntimeError("Failed to download the file " + url)


def _download_sys_perf_binaries(version_id, build_variant, evergreen_api_user, evergreen_api_key):
    url = _get_binary_url(version_id, build_variant, evergreen_api_user, evergreen_api_key)
    _download_binary_file(url, "binary.tar.gz")


if __name__ == "__main__":
    argParser = argparse.ArgumentParser()
    argParser.add_argument(
        "-v", "--version_id", help="Evergreen version_id from which binaries will be downloaded"
    )
    argParser.add_argument(
        "-b", "--build_variant", help="Build variant for which binaries will be downloaded"
    )
    argParser.add_argument(
        "-u",
        "--evergreen_api_user",
        help="Evergreen API user, see https://spruce.mongodb.com/preferences/cli",
    )
    argParser.add_argument(
        "-k",
        "--evergreen_api_key",
        help="Evergreen API key, see https://spruce.mongodb.com/preferences/cli",
    )
    args = argParser.parse_args()

    _download_sys_perf_binaries(
        args.version_id, args.build_variant, args.evergreen_api_user, args.evergreen_api_key
    )
