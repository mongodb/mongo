#!/usr/bin/env python3
"""
Generate a CycloneDX SBOM using scan results from Endor Labs.
Schema validation of output is not performed.
Use 'buildscripts/sbom_linter.py' for validation.

Invoke with ---help or -h for help message.
"""

import argparse
import json
import logging
import os
import re
import subprocess
import sys
import urllib.parse
import uuid
from datetime import datetime, timezone
from pathlib import Path

from config import (
    endor_components_remove,
    endor_components_rename,
    get_semver_from_release_version,
    is_valid_purl,
    process_component_special_cases,
)
from endorctl_utils import EndorCtl
from git import Commit, Repo

# region init


class WarningListHandler(logging.Handler):
    """Collect warnings"""

    def __init__(self):
        super().__init__()
        self.warnings = []

    def emit(self, record):
        if record.levelno >= logging.WARNING:
            self.warnings.append(record)


logging.basicConfig(stream=sys.stdout)
logger = logging.getLogger("generate_sbom")
logger.setLevel(logging.INFO)

# Create an instance of the custom handler
warning_handler = WarningListHandler()

# Add the handler to the logger
logger.addHandler(warning_handler)


# Get the absolute path of the script file and directory
script_path = Path(__file__).resolve()
script_directory = script_path.parent

# Regex for validation
REGEX_COMMIT_SHA = r"^[0-9a-fA-F]{40}$"
REGEX_GIT_BRANCH = r"^[a-zA-Z0-9_.\-/]+$"
REGEX_GITHUB_URL = r"^(https://github.com/)([a-zA-Z0-9-]{1,39}/[a-zA-Z0-9-_.]{1,100})(\.git)$"
REGEX_RELEASE_BRANCH = r"^v\d\.\d$"
REGEX_RELEASE_TAG = r"^r\d\.\d.\d(-\w*)?$"

# endregion init


# region functions and classes


class GitInfo:
    """Get, set, format git info"""

    def __init__(self):
        print_banner("Gathering git info")
        try:
            self.repo_root = Path(
                subprocess.run(
                    "git rev-parse --show-toplevel", shell=True, text=True, capture_output=True
                ).stdout.strip()
            )
            self._repo = Repo(self.repo_root)
        except Exception as e:
            logger.warning(
                "Unable to read git repo information. All necessary script arguments must be provided."
            )
            logger.warning(e)
            self._repo = None
        else:
            try:
                self.project = self._repo.remotes.origin.config_reader.get("url")
                if not self.project.endswith(".git"):
                    self.project += ".git"
                org_repo = extract_repo_from_git_url(self.project)
                self.org = org_repo["org"]
                self.repo = org_repo["repo"]
                self.commit = self._repo.head.commit.hexsha
                self.branch = self._repo.active_branch.name

                # filter tags for latest release e.g., r8.2.1
                release_tags = []
                filtered_tags = [
                    tag for tag in self._repo.tags if re.fullmatch(REGEX_RELEASE_TAG, tag.name)
                ]
                logging.info(f"GIT: Parsing {len(filtered_tags)} release tags for match to commit")
                for tag in filtered_tags:
                    if tag.commit == self.commit:
                        release_tags.append(tag.name)
                if len(release_tags) > 0:
                    self.release_tag = release_tags[-1]
                else:
                    self.release_tag = None
                logging.debug(f"GitInfo->release_tag(): {self.release_tag}")

                logging.debug(f"GitInfo->__init__: {self}")
            except Exception as e:
                logger.warning("Unable to fully parse git info.")
                logger.warning(e)

    def close(self):
        """Closes the underlying Git repo object to release resources."""
        if self._repo:
            logger.debug("Closing Git repo object.")
            self._repo.close()
            self._repo = None

    def added_new_3p_folder(self, commit: Commit) -> bool:
        """
        Checks if a given commit added a new third-party subfolder.

        Args:
            commit: The GitPython Commit object to analyze.

        Returns:
            True if the commit added a new subfolder, False otherwise.
        """
        if not commit.parents:
            # If it's the initial commit, all folders are "new"
            # You might want to refine this logic based on your definition of "new"
            # Check if there are any subfolders in the initial commit
            return bool(commit.tree.trees)

        parent_commit = commit.parents[0]
        diff_index = commit.diff(parent_commit)

        for diff in diff_index:
            # Check for added items that are directories
            if diff.change_type == "A" and diff.b_is_dir:
                return True
        return False


def print_banner(text: str) -> None:
    """print() a padded status message to stdout"""
    print()
    print(text.center(len(text) + 2, " ").center(120, "="))


def extract_repo_from_git_url(git_url: str) -> dict:
    """Determine org/repo for a given git url"""
    git_org = git_url.split("/")[-2].replace(".git", "")
    git_repo = git_url.split("/")[-1].replace(".git", "")
    return {
        "org": git_org,
        "repo": git_repo,
    }


def sbom_components_to_dict(sbom: dict, with_version: bool = False) -> dict:
    """Create a dict of SBOM components with a version-less PURL as the key"""
    components = sbom["components"]
    if with_version:
        components_dict = {
            urllib.parse.unquote(component["bom-ref"]): component for component in components
        }
    else:
        components_dict = {
            urllib.parse.unquote(component["bom-ref"]).split("@")[0]: component
            for component in components
        }
    return components_dict


def read_sbom_json_file(file_path: str) -> dict:
    """Load a JSON SBOM file (schema is not validated)"""
    try:
        with open(file_path, "r", encoding="utf-8") as input_json:
            sbom_json = input_json.read()
        result = json.loads(sbom_json)
    except Exception as e:
        logger.error(f"Error loading SBOM file from {file_path}")
        logger.error(e)
    else:
        logger.info(f"SBOM loaded from {file_path} with {len(result['components'])} components")
        return result


def write_sbom_json_file(sbom_dict: dict, file_path: str) -> None:
    """Save a JSON SBOM file (schema is not validated)"""
    try:
        file_path = os.path.abspath(file_path)
        with open(file_path, "w", encoding="utf-8") as output_json:
            json.dump(sbom_dict, output_json, indent=2)
            output_json.write("\n")
    except Exception as e:
        logger.error(f"Error writing SBOM file to {file_path}")
        logger.error(e)
    else:
        logger.info(f"SBOM file saved to {file_path}")


def write_list_to_text_file(str_list: list, file_path: str) -> None:
    """Save a list of strings to a text file"""
    try:
        file_path = os.path.abspath(file_path)
        with open(file_path, "w", encoding="utf-8") as output_txt:
            for item in str_list:
                output_txt.write(f"{item}\n")
    except Exception as e:
        logger.error(f"Error writing text file to {file_path}")
        logger.error(e)
    else:
        logger.info(f"Text file saved to {file_path}")


def set_component_version(
    component: dict, version: str, purl_version: str = None, cpe_version: str = None
) -> None:
    """Update the appropriate version fields in a component from the metadata SBOM"""
    if not purl_version:
        purl_version = version

    if not cpe_version:
        cpe_version = version

    component["bom-ref"] = component["bom-ref"].replace("{{VERSION}}", purl_version)
    component["version"] = component["version"].replace("{{VERSION}}", version)
    if component.get("purl"):
        component["purl"] = component["purl"].replace("{{VERSION}}", purl_version)
        if not is_valid_purl(component["purl"]):
            logger.warning(f"PURL: Invalid PURL ({component['purl']})")
    if component.get("cpe"):
        component["cpe"] = component["cpe"].replace("{{VERSION}}", cpe_version)


def set_dependency_version(dependencies: list, meta_bom_ref: str, purl_version: str) -> None:
    """Update the appropriate dependency version fields in the metadata SBOM"""
    r = 0
    d = 0
    for dependency in dependencies:
        if "{{VERSION}}" in dependency["ref"] and dependency["ref"] == meta_bom_ref:
            dependency["ref"] = dependency["ref"].replace("{{VERSION}}", purl_version)
            r += 1
        for i in range(len(dependency["dependsOn"])):
            if dependency["dependsOn"][i] == meta_bom_ref:
                dependency["dependsOn"][i] = dependency["dependsOn"][i].replace(
                    "{{VERSION}}", purl_version
                )
                d += 1

    logger.debug(f"set_dependency_version: '{meta_bom_ref}' updated {r} refs and {d} dependsOn")


def get_subfolders_dict(folder_path: str = ".") -> dict:
    """Get list of all directories in the specified path"""
    subfolders = []
    try:
        # Get all entries (files and directories) in the specified path
        entries = os.listdir(folder_path)

        # Filter for directories
        for entry in entries:
            full_path = os.path.join(folder_path, entry)
            if os.path.isdir(full_path):
                subfolders.append(entry)
    except FileNotFoundError:
        logger.error(f"Error: Directory '{folder_path}' not found.")
    except Exception as e:
        logger.error(f"An error occurred: {e}")

    subfolders.sort()
    return {key: 0 for key in subfolders}


def get_component_import_script_path(component: dict) -> str:
    """Extract the path to a third-party library import script as defined in component 'properties' as 'import_script_path'"""
    import_script_path = [
        p.get("value")
        for p in component.get("properties", [])
        if p.get("name") == "import_script_path"
    ]
    if len(import_script_path):
        # There should only be 1 result, if any
        return import_script_path[0]
    else:
        return None


def get_version_from_import_script(file_path: str) -> str:
    """A rudimentary parse of a shell script file to extract the static value defined for the VERSION variable"""
    try:
        with open(file_path, "r", encoding="utf-8") as file:
            for line in file:
                if line.strip().startswith("VERSION="):
                    return re.sub(
                        r"^VERSION=(?P<quote>[\"']?)(?P<content>\S+)(?P=quote).*$",
                        r"\g<content>",
                        line.strip(),
                    )
    except Exception as e:
        logger.warning(f"Unable to load {file_path}")
        logger.warning(e)
    else:
        return None


# endregion functions and classes


def main() -> None:
    # region define args

    parser = argparse.ArgumentParser(
        description="""Generate a CycloneDX v1.5 JSON SBOM file using a combination of scan results from Endor Labs, pre-defined SBOM metadata, and the existing SBOM.
            Requires endorctl to be installed and configured, which can be done using 'buildscripts/sbom/install_endorctl.sh'.
            For use in CI, script may be run with no arguments.""",
        epilog="Note: The git-related default values are dynamically generated.",
        formatter_class=argparse.MetavarTypeHelpFormatter,
    )

    endor = parser.add_argument_group("Endor Labs API (via 'endorctl')")
    endor.add_argument(
        "--endorctl-path",
        help="Path to endorctl, the Endor Labs CLI (Default: 'endorctl')",
        default="endorctl",
        type=str,
    )
    endor.add_argument(
        "--config-path",
        help="Path to endor config directory containing config.yaml (Default: '$HOME/.endorctl')",
        default=None,
        type=str,
    )
    endor.add_argument(
        "--namespace", help="Endor Labs namespace (Default: mongodb.{git org})", type=str
    )
    endor.add_argument(
        "--target",
        help="Target for generated SBOM. Commit: results from running/completed PR scan, Branch: results from latest monitoring scan, Project: results from latest monitoring scan of the 'default' branch (default: commit)",
        choices=["commit", "branch", "project"],
        default="commit",
        type=str,
    )
    endor.add_argument(
        "--project",
        help="Full GitHub git URL [e.g., https://github.com/10gen/mongo.git] (Default: current git URL)",
        type=str,
    )

    target = parser.add_argument_group("Target values. Apply only if --target is not 'project'")
    exclusive_target = target.add_mutually_exclusive_group()
    exclusive_target.add_argument(
        "--commit",
        help="PR commit SHA [40-character hex string] (Default: current git commit)",
        type=str,
    )
    exclusive_target.add_argument(
        "--branch",
        help="Git repo branch monitored by Endor Labs [e.g., v8.0] (Default: current git org/repo)",
        type=str,
    )

    files = parser.add_argument_group("SBOM files")
    files.add_argument(
        "--sbom-metadata",
        help="Input path for template SBOM file with metadata (Default: './buildscripts/sbom/metadata.cdx.json')",
        default="./buildscripts/sbom/metadata.cdx.json",
        type=str,
    )
    files.add_argument(
        "--sbom-in",
        help="Input path for previous SBOM file (Default: './sbom.json')",
        default="./sbom.json",
        type=str,
    )
    files.add_argument(
        "--sbom-out",
        help="Output path for SBOM file (Default: './sbom.json')",
        default="./sbom.json",
        type=str,
    )
    parser.add_argument(
        "--retry-limit",
        help="Maximum number of times to retry when a target PR scan has not started (Default: 5)",
        default=5,
        type=int,
    )
    parser.add_argument(
        "--sleep-duration",
        help="Number of seconds to wait between retries (Default: 30)",
        default=30,
        type=int,
    )
    parser.add_argument(
        "--save-warnings",
        help="Save warning messages to a specified file (Default: None)",
        default=None,
        type=str,
    )
    parser.add_argument("--debug", help="Set logging level to DEBUG", action="store_true")

    # endregion define args

    # region parse args

    args = parser.parse_args()

    git_info = GitInfo()

    # endor
    endorctl_path = args.endorctl_path
    config_path = args.config_path
    namespace = args.namespace if args.namespace else f"mongodb.{git_info.org}"
    target = args.target

    # project
    if args.project and args.project != git_info.project:
        if not re.fullmatch(REGEX_GITHUB_URL, args.project):
            parser.error(f"Invalid Git URL: {args.project}.")
        git_info.project = args.project
        git_info.org, git_info.repo = map(
            extract_repo_from_git_url(git_info.project).get, ("org", "repo")
        )
        git_info.release_tag = None

    # targets
    # commit
    if args.commit and args.commit != git_info.commit:
        if not re.fullmatch(REGEX_COMMIT_SHA, args.commit):
            parser.error(
                f"Invalid Git commit SHA: {args.commit}. Must be a 40-character hexadecimal string (SHA-1)."
            )
        git_info.commit = args.commit

    # branch
    if args.branch and args.branch != git_info.branch:
        if len(args.branch.encode("utf-8")) > 244 or not re.fullmatch(
            REGEX_GIT_BRANCH, args.branch
        ):
            parser.error(
                f"Invalid Git branch name: {args.branch}. Limit is 244 bytes with allowed characters: [a-zA-Z0-9_.-/]"
            )
        git_info.branch = args.branch

    # files
    sbom_out_path = args.sbom_out
    sbom_in_path = args.sbom_in
    sbom_metadata_path = args.sbom_metadata
    if args.save_warnings:
        save_warnings = args.save_warnings

    # environment
    retry_limit = args.retry_limit
    sleep_duration = args.sleep_duration

    if args.debug:
        logger.setLevel(logging.DEBUG)

    # endregion parse args

    # region export Endor Labs SBOM

    print_banner(f"Exporting Endor Labs SBOM for {target} {getattr(git_info, target)}")
    endorctl = EndorCtl(namespace, retry_limit, sleep_duration, endorctl_path, config_path)
    if target == "commit":
        endor_bom = endorctl.get_sbom_for_commit(git_info.project, git_info.commit)
    elif target == "branch":
        endor_bom = endorctl.get_sbom_for_branch(git_info.project, git_info.branch)
    elif target == "project":
        endor_bom = endorctl.get_sbom_for_project(git_info.project)

    if not endor_bom:
        logger.error("Empty result for Endor SBOM!")
        if target == "commit":
            logger.error("Check Endor Labs for any unanticipated issues with the target PR scan.")
        else:
            logger.error("Check Endor Labs for status of the target monitoring scan.")
        sys.exit(1)

    # endregion export Endor Labs SBOM

    # region Pre-process Endor Labs SBOM

    print_banner("Pre-Processing Endor Labs SBOM")

    ## remove uneeded components ##
    # [list]endor_components_remove is defined in config.py

    # Reverse iterate the SBOM components list to safely modify in situ
    for i in range(len(endor_bom["components"]) - 1, -1, -1):
        component = endor_bom["components"][i]
        removed = False
        for remove in endor_components_remove:
            if component["bom-ref"].startswith(remove):
                logger.info("ENDOR SBOM PRE-PROCESS: removing " + component["bom-ref"])
                del endor_bom["components"][i]
                removed = True
                break
        if not removed:
            for rename in endor_components_rename:
                old = rename[0]
                new = rename[1]
                component["bom-ref"] = component["bom-ref"].replace(old, new)
                component["purl"] = component["purl"].replace(old, new)

    logger.info(f"Endor Labs SBOM pre-processed with {len(endor_bom['components'])} components")

    # endregion Pre-process Endor Labs SBOM

    # region load metadata and previous SBOMs

    print_banner("Loading metadata SBOM and previous SBOM")

    meta_bom = read_sbom_json_file(sbom_metadata_path)
    if not meta_bom:
        logger.error("No SBOM metadata. This is fatal.")
        sys.exit(1)

    prev_bom = read_sbom_json_file(sbom_in_path)
    if not prev_bom:
        logger.warning(
            "Unable to load previous SBOM data. The new SBOM will be generated without any previous context. This is unexpected, but not fatal."
        )
        # Create empty prev_bom to avoid downstream processing errors
        prev_bom = {
            "bom-ref": None,
            "metadata": {
                "timestamp": endor_bom["metadata"]["timestamp"],
                "component": {
                    "version": None,
                },
            },
            "components": [],
        }

    # endregion load metadata and previous SBOMs

    # region Build composite SBOM
    # Note: No exception handling here. The most likely reason for an exception is missing data elements
    # in SBOM files, which is fatal if it happens. Code is in place to handle the situation
    # where there is no previous SBOM to include, but we want to fail if required data is absent.
    print_banner("Building composite SBOM (metadata + endor + previous)")

    # Sort components by bom-ref
    endor_bom["components"].sort(key=lambda c: c["bom-ref"])
    meta_bom["components"].sort(key=lambda c: c["bom-ref"])
    prev_bom["components"].sort(key=lambda c: c["bom-ref"])

    # Create SBOM component lookup dicts
    endor_components = sbom_components_to_dict(endor_bom)
    prev_components = sbom_components_to_dict(prev_bom)

    # region MongoDB primary component

    # Attempt to determine the MongoDB Version being scanned
    logger.debug(
        f"Available MongoDB version options, tag: {git_info.release_tag}, branch: {git_info.branch}, previous SBOM: {prev_bom['metadata']['component']['version']}"
    )
    meta_bom_ref = meta_bom["metadata"]["component"]["bom-ref"]

    # Project scan always set to 'master' or if using 'master' branch
    if target == "project" or git_info.branch == "master":
        version = "master"
        purl_version = "master"
        cpe_version = "master"
        logger.info("Using branch 'master' as MongoDB version")

    # tagged release. e.g., r8.1.0, r8.2.1-rc0
    elif git_info.release_tag:
        version = git_info.release_tag[1:]  # remove leading 'r'
        purl_version = git_info.release_tag
        cpe_version = version  # without leading 'r'
        logger.info(f"Using release_tag '{git_info.release_tag}' as MongoDB version")

    # Release branch e.g., v7.0 or v8.2
    elif target == "branch" and re.fullmatch(REGEX_RELEASE_BRANCH, git_info.branch):
        version = git_info.branch
        purl_version = git_info.branch
        # remove leading 'v', add wildcard. e.g. 8.2.*
        cpe_version = version[1:] + ".*"
        logger.info(f"Using release branch '{git_info.branch}' as MongoDB version")

    # Previous SBOM app version, if all needed specifiers exist
    elif (
        prev_bom.get("metadata", {}).get("component", {}).get("version")
        and prev_bom.get("metadata", {}).get("component", {}).get("purl")
        and prev_bom.get("metadata", {}).get("component", {}).get("cpe")
    ):
        version = prev_bom["metadata"]["component"]["version"]
        purl_version = prev_bom["metadata"]["component"]["purl"].split("@")[-1]
        cpe_version = prev_bom["metadata"]["component"]["cpe"].split(":")[5]
        logger.info(f"Using previous SBOM version '{version}' as MongoDB version")

    else:
        # Fall back to the version specified in the Endor SBOM
        # This is unlikely to be accurate
        version = endor_bom["metadata"]["component"]["version"]
        purl_version = version
        cpe_version = version
        logger.warning(
            f"Using SBOM version '{version}' from Endor Labs scan. This is unlikely to be accurate and may specify a PR #."
        )

    # Set main component version
    set_component_version(meta_bom["metadata"]["component"], version, purl_version, cpe_version)
    # Run through 'dependency' objects to set main component version
    set_dependency_version(meta_bom["dependencies"], meta_bom_ref, purl_version)

    # endregion MongoDB primary component

    # region SBOM components

    # region Parse metadata SBOM components

    third_party_folders = get_subfolders_dict(git_info.repo_root.as_posix() + "/src/third_party")
    # pre-exclude 'scripts' folder
    del third_party_folders["scripts"]

    for component in meta_bom["components"]:
        versions = {
            "endor": None,
            "import_script": None,
            "metadata": None,
        }

        component_key = component["bom-ref"].split("@")[0]

        print_banner("Component: " + component_key)

        ################ Endor Labs ################
        if component_key in endor_components:
            # Pop component from dict so we are left with only unmatched components
            endor_component = endor_components.pop(component_key)
            versions["endor"] = endor_component.get("version")
            logger.debug(
                f"VERSION ENDOR: {component_key}: Found version '{versions['endor']}' in Endor Labs results"
            )

        ############## Import Script ###############
        # Import script version, if exists
        import_script_path = get_component_import_script_path(component)
        if import_script_path:
            import_script = Path(import_script_path)
            if import_script.exists():
                versions["import_script"] = get_version_from_import_script(import_script_path)
                if versions["import_script"]:
                    versions["import_script"] = versions["import_script"].replace("release-", "")
                if versions["import_script"]:
                    logger.debug(
                        f"VERSION IMPORT SCRIPT: {component_key}: Found version '{versions['import_script']}' in import script '{import_script_path}'"
                    )
            else:
                logger.debug(
                    f"VERSION IMPORT SCRIPT: {component_key}: Import script not found! '{import_script_path}'"
                )

        ############## Metadata ###############
        # Hard-coded metadata version, if exists
        if "{{VERSION}}" not in component["version"]:
            versions["metadata"] = component.get("version")

        logger.info(f"VERSIONS: {component_key}: " + str(versions))

        ############## Component Special Cases ###############
        process_component_special_cases(
            component_key, component, versions, git_info.repo_root.as_posix()
        )

        # For the standard workflow, we favor the Endor Labs version, followed by import script, followed by hard coded
        if (
            versions["endor"]
            and versions["import_script"]
            and get_semver_from_release_version(versions["endor"])
            != get_semver_from_release_version(versions["import_script"])
        ):
            logger.debug(
                ",".join(
                    [
                        "endor:",
                        versions["endor"],
                        "semver(endor):",
                        get_semver_from_release_version(versions["endor"]),
                        "import_script:",
                        versions["import_script"],
                        "semver(import_script):",
                        get_semver_from_release_version(versions["import_script"]),
                    ]
                )
            )
            logger.warning(
                f"VERSION MISMATCH: {component_key}: Endor version {versions['endor']} does not match import script version {versions['import_script']}"
            )

        version = versions["endor"] or versions["import_script"] or versions["metadata"]

        ############## Assign Version ###############
        if version:
            meta_bom_ref = component["bom-ref"]

            ## Special case for FireFox ##
            # The CPE for FireFox ESR needs the 'esr' removed from the version, as it is specified in another section
            if component["bom-ref"].startswith("pkg:deb/debian/firefox-esr@"):
                set_component_version(component, version, cpe_version=version.replace("esr", ""))
            else:
                semver = get_semver_from_release_version(version)
                set_component_version(component, semver, version, semver)

            set_dependency_version(meta_bom["dependencies"], meta_bom_ref, version)

            # check against third_party folders
            component_defines_location = False
            for occurrence in component.get("evidence", {}).get("occurrences", []):
                location = occurrence.get("location")
                if location:
                    component_defines_location = True
                if location.startswith("src/third_party/"):
                    location = location.replace("src/third_party/", "")
                    if location in third_party_folders:
                        third_party_folders[location] += 1
                        logger.debug(
                            f"THIRD_PARTY FOLDER: {component_key} matched folder {location} specified in SBOM"
                        )
                    else:
                        logger.warning(
                            f"THIRD_PARTY FOLDER: {component_key} lists third-party location folder as {location}, which does not exist!"
                        )
                else:
                    logger.warning(
                        f"THIRD_PARTY FOLDER: {component_key} lists a location as '{location}'. Ideally, all third-party components are located under 'src/third_party/'."
                    )
            if not component_defines_location:
                logger.warning(
                    f"THIRD_PARTY FOLDER: {component_key} does not define a location in '.evidence.occurrences[]'"
                )
        else:
            logger.warning(
                f"VERSION NOT FOUND: Could not find a version for {component_key}! Removing from SBOM. Component may need to be removed from the {sbom_metadata_path} file."
            )
            del component

    print_banner("Third Party Folders")
    third_party_folders_missed = {
        key: value for key, value in third_party_folders.items() if value == 0
    }
    if third_party_folders_missed:
        logger.warning(
            "THIRD_PARTY FOLDERS: 'src/third_party' folders not matched with a component: "
            + ",".join(third_party_folders_missed.keys())
        )
    else:
        logger.info(
            "THIRD_PARTY FOLDERS: All 'src/third_party' folders successfully matched with one or more components."
        )

    # explicit cleanup to avoid gc race condition on script temination
    git_info.close()
    del git_info

    # endregion Parse metadata SBOM components

    # region Parse unmatched Endor Labs components

    print_banner("New Endor Labs components")
    if endor_components:
        logger.info(
            f"ENDOR SBOM: There are {len(endor_components)} unmatched components in the Endor Labs SBOM. Adding as-is. The applicable metadata should be added to the metadata SBOM for the next run."
        )
        for component in endor_components:
            # set scope to excluded by default until the component is evaluated
            endor_components[component]["scope"] = "excluded"
            meta_bom["components"].append(endor_components[component])
            meta_bom["dependencies"].append(
                {"ref": endor_components[component]["bom-ref"], "dependsOn": []}
            )
            logger.info(f"SBOM AS-IS COMPONENT: Added {component}")

    # endregion Parse unmatched Endor Labs components

    # region Finalize SBOM

    # Have the SBOM app version changed?
    sbom_app_version_changed = (
        prev_bom["metadata"]["component"]["version"] != meta_bom["metadata"]["component"]["version"]
    )
    logger.info(f"SUMMARY: MongoDB version changed: {sbom_app_version_changed}")

    # Have the components changed?
    prev_components = sbom_components_to_dict(prev_bom, with_version=True)
    meta_components = sbom_components_to_dict(meta_bom, with_version=True)
    sbom_components_changed = prev_components.keys() != meta_components.keys()
    logger.info(
        f"SBOM_DIFF: SBOM components changed (added, removed, or version): {sbom_components_changed}. Previous SBOM has {len(prev_components)} components; New SBOM has {len(meta_components)} components"
    )

    # Components in prev SBOM but not in generated SBOM
    prev_components = sbom_components_to_dict(prev_bom, with_version=False)
    meta_components = sbom_components_to_dict(meta_bom, with_version=False)
    prev_components_diff = list(set(prev_components.keys()) - set(meta_components.keys()))
    if prev_components_diff:
        logger.info(
            "SBOM_DIFF: Components in previous SBOM and not in generated SBOM: "
            + ",".join(prev_components_diff)
        )

    # Components in generated SBOM but not in prev SBOM
    meta_components_diff = list(set(meta_components.keys()) - set(prev_components.keys()))
    if meta_components_diff:
        logger.info(
            "SBOM_DIFF: Components in generated SBOM and not in previous SBOM: "
            + ",".join(meta_components_diff)
        )

    # serialNumber https://cyclonedx.org/docs/1.5/json/#serialNumber
    # version (SBOM version) https://cyclonedx.org/docs/1.5/json/#version
    if sbom_app_version_changed:
        # New MongoDB version requires a unique serial number and version 1
        meta_bom["serialNumber"] = uuid.uuid4().urn
        meta_bom["version"] = 1
    else:
        # MongoDB version is the same, so reuse the serial number and SBOM version
        meta_bom["serialNumber"] = prev_bom["serialNumber"]
        meta_bom["version"] = prev_bom["version"]
        # If the components have changed, bump the SBOM version
        if sbom_components_changed:
            meta_bom["version"] += 1

    # metadata.timestamp https://cyclonedx.org/docs/1.5/json/#metadata_timestamp
    # Only update the timestamp if something has changed
    if sbom_app_version_changed or sbom_components_changed:
        meta_bom["metadata"]["timestamp"] = (
            datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
        )
    else:
        meta_bom["metadata"]["timestamp"] = prev_bom["metadata"]["timestamp"]

    # metadata.tools https://cyclonedx.org/docs/1.5/json/#metadata_tools
    meta_bom["metadata"]["tools"] = endor_bom["metadata"]["tools"]

    write_sbom_json_file(meta_bom, sbom_out_path)

    # Access the collected warnings
    print_banner("CONSOLIDATED WARNINGS")
    warnings = []
    for record in warning_handler.warnings:
        warnings.append(record.getMessage())

    print("\n".join(warnings))

    if save_warnings:
        write_list_to_text_file(warnings, save_warnings)

    print_banner("COMPLETED")
    if not os.getenv("CI"):
        print("Be sure to add the SBOM to your next commit if the file content has changed.")

    # endregion Finalize SBOM

    # endregion Build composite SBOM


if __name__ == "__main__":
    main()
