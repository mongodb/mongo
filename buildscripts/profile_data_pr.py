#!/usr/bin/env python3
"""
Script that opens a PR using a bot to update profile data links for PGO and BOLT.
This updates profiling_data.bzl and is reliant on the formatting of it to not change.
The script always expects 3 links, one to the bolt data, one to the gcc data, and one to the
clang data. It always expects one of either clang or gcc data to not actually contain data
because we only want to update one, however the build will work at updating either of them.
"""

import argparse
import hashlib
import os
import re
import sys
import tempfile

import requests
from github.GithubException import GithubException
from github.GithubIntegration import GithubIntegration

OWNER_NAME = "10gen"
REPO_NAME = "mongo"
PROFILE_DATA_FILE_PATH = "bazel/repository_rules/profiling_data.bzl"

def get_mongo_repository(app_id, private_key):
    """
    Gets the mongo github repository
    """
    app = GithubIntegration(int(app_id), private_key)
    installation = app.get_repo_installation(OWNER_NAME, REPO_NAME)
    g = installation.get_github_for_installation()
    return g.get_repo(f"{OWNER_NAME}/{REPO_NAME}")

def compute_sha256(file_path: str) -> str:
    """
    Compute the sha256 hash of a file
    """
    sha256 = hashlib.sha256()
    with open(file_path, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256.update(block)
    return sha256.hexdigest()

def download_file(url: str, output_location: str) -> bool:
    """
    Download a file to a specific output_location and return if the file existed remotely
    """
    try:  
        response = requests.get(url)
        response.raise_for_status()
        with open(output_location, "wb") as file:
            file.write(response.content)
        return True
    except requests.exceptions.RequestException:
        return False

def replace_quoted_text_in_tagged_line(text: str, tag: str, new_text: str) -> str:
    """
    Replace the text between quotes in a line that starts with a specific tag
    eg. FOO = "replace_this" -> FOO = "new_text"
    """
    if tag not in text:
        print(f"Tag: {tag} did not exist in the file.", file=sys.stderr)
        sys.exit(1)
    pattern = rf'({tag}.*?"(.*?)")'
    return re.sub(pattern, lambda match: match.group(0).replace(match.group(2), new_text), text)

def update_bolt_info(file_content: str, new_url: str, new_checksum: str) -> str:
    """
    Updates the bolt url and checksum lines in a file
    """
    bolt_url_tag = "DEFAULT_BOLT_DATA_URL"
    bolt_checksum_tag = "DEFAULT_BOLT_DATA_CHECKSUM"
    updated_text = replace_quoted_text_in_tagged_line(file_content, bolt_url_tag, new_url)
    return replace_quoted_text_in_tagged_line(updated_text, bolt_checksum_tag, new_checksum)

def update_clang_pgo_info(file_content: str, new_url: str, new_checksum: str) -> str:
    """
    Updates the clang pgo url and checksum lines in a file
    """
    clang_pgo_url_tag = "DEFAULT_CLANG_PGO_DATA_URL"
    clang_pgo_checksum_tag = "DEFAULT_CLANG_PGO_DATA_CHECKSUM"
    updated_text = replace_quoted_text_in_tagged_line(file_content, clang_pgo_url_tag, new_url)
    return replace_quoted_text_in_tagged_line(updated_text, clang_pgo_checksum_tag, new_checksum)

def update_gcc_pgo_info(file_content: str, new_url: str, new_checksum: str) -> str:
    """
    Updates the gcc pgo url and checksum lines in a file
    """
    gcc_pgo_url_tag = "DEFAULT_GCC_PGO_DATA_URL"
    gcc_pgo_checksum_tag = "DEFAULT_GCC_PGO_DATA_CHECKSUM"
    updated_text = replace_quoted_text_in_tagged_line(file_content, gcc_pgo_url_tag, new_url)
    return replace_quoted_text_in_tagged_line(updated_text, gcc_pgo_checksum_tag, new_checksum)

def create_pr(target_branch: str, new_branch: str, original_file, new_content: str):
    """
    Opens up a pr for a single file with new contents
    """
    target_repo_branch = repo.get_branch(target_branch)
    ref = f"refs/heads/{new_branch}"
    try:
        repo.get_branch(branch=new_branch)
    except GithubException as e:
        if e.status == 404:
            print(f"Branch doesn't exist, creating branch {new_branch}.")
            repo.create_git_ref(ref=ref, sha=target_repo_branch.commit.sha)
        else:
            raise 

    repo.update_file(path=PROFILE_DATA_FILE_PATH, content=new_content, branch=new_branch, message="Updating profile files.", sha=original_file.sha)
    repo.create_pull(base=target_branch, head=new_branch, title="SERVER-110427 Update profiling data", body="Automated PR updating the profiling data.")

def create_profile_data_pr(repo, args, target_branch, new_branch):
    """
    Get the new text needed and create a pr for updating the profiling_data.bzl
    """
    temp_dir = tempfile.mkdtemp()
    bolt_file = os.path.join(temp_dir, "bolt.fdata")
    clang_pgo_file = os.path.join(temp_dir, "clang_pgo.profdata")
    gcc_pgo_file = os.path.join(temp_dir, "gcc_pgo.tgz")

    bolt_file_exists = download_file(args.bolt_url, bolt_file)
    clang_pgo_file_exists = download_file(args.clang_pgo_url, clang_pgo_file)
    gcc_pgo_file_exists = download_file(args.gcc_pgo_url, gcc_pgo_file)

    # These are not errors because the script can run when no files were meant to be updated.
    if not bolt_file_exists:
        print(f"Bolt file did not exist at {args.bolt_url}. Not creating PR.")
        sys.exit(0)

    if clang_pgo_file_exists and gcc_pgo_file_exists:
        print(f"Both clang and gcc had pgo files that existed. Clang: {args.clang_pgo_url} GCC: {args.gcc_pgo_url}. Only one should be updated at a time. Not creating PR.")
        sys.exit(1)

    if not clang_pgo_file_exists and not gcc_pgo_file_exists:
        print(f"Neither clang nor gcc had pgo files that existed at either {args.clang_pgo_url} or {args.gcc_pgo_url}. Not creating PR.")
        sys.exit(0)

    profiling_data_file = repo.get_contents(PROFILE_DATA_FILE_PATH, ref=f"refs/heads/{target_branch}")
    profiling_data_file_content = profiling_data_file.decoded_content.decode()

    profiling_file_updated_text = update_bolt_info(profiling_data_file_content, args.bolt_url, compute_sha256(bolt_file))

    if clang_pgo_file_exists:
        profiling_file_updated_text = update_clang_pgo_info(profiling_file_updated_text, args.clang_pgo_url, compute_sha256(clang_pgo_file))
    else:
        profiling_file_updated_text = update_gcc_pgo_info(profiling_file_updated_text, args.gcc_pgo_url, compute_sha256(gcc_pgo_file))

    create_pr(target_branch, new_branch, profiling_data_file, profiling_file_updated_text)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="This script uses bolt file url, clang pgo file url and gcc pgo file url to create a PR updating the links to these files.")
    parser.add_argument("bolt_url", help="URL that BOLT data was uploaded to.")
    parser.add_argument("clang_pgo_url", help="URL that clang pgo data was uploaded to.")
    parser.add_argument("gcc_pgo_url", help="URL that gcc pgo data was uploaded to.")
    parser.add_argument("target_branch", help="The branch you want to create a PR into.")
    parser.add_argument("new_branch", help="The new branch to create a PR from.")
    parser.add_argument("app_id", help="App ID used for authentication.")
    parser.add_argument("private_key", help="Key to use for authentication.")
    args = parser.parse_args()
    repo = get_mongo_repository(args.app_id, args.private_key)
    create_profile_data_pr(repo, args, args.target_branch, args.new_branch)
