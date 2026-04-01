#!/usr/bin/env python3
"""
Generate a CycloneDX SBOM using scan results from Endor Labs.
Schema validation of output is not performed.
Use 'buildscripts/sbom_linter.py' for validation.

Invoke with ---help or -h for help message.
"""

import argparse
import logging
import os
import re
import subprocess
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path

from config import (
    endor_components_remove,
    endor_components_rename,
    get_semver_from_release_version,
    process_component_special_cases,
    third_party_folders_remove,
)
from endorctl_utils import EndorCtl
from git import Commit, Repo

from buildscripts.sbom.sbom_utils import (
    add_component_dependsOn,
    add_component_property,
    check_metadata_sbom,
    convert_sbom_to_public,
    read_sbom_json_file,
    remove_sbom_component,
    sbom_components_to_dict,
    set_component_version,
    set_dependency_version,
    write_sbom_json_file,
)
from buildscripts.util.codeowners_utils import Owners

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
REGEX_RELEASE_BRANCH = r"^v\d\.\d(-staging)?$"
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
                    "git rev-parse --show-toplevel",
                    shell=True,
                    text=True,
                    capture_output=True,
                    check=True,
                ).stdout.strip()
            )
            self._repo = Repo(self.repo_root)
        except (OSError, subprocess.CalledProcessError, AttributeError, TypeError) as e:
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
                logging.info("GIT: Parsing %d release tags for match to commit", len(filtered_tags))
                for tag in filtered_tags:
                    if tag.commit == self.commit:
                        release_tags.append(tag.name)
                if len(release_tags) > 0:
                    self.release_tag = release_tags[-1]
                else:
                    self.release_tag = None
                logging.debug("GitInfo->release_tag(): %s", self.release_tag)

                logging.debug("GitInfo->__init__: %s", self)
            except (AttributeError, IndexError, ValueError, TypeError) as e:
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


def write_list_to_text_file(str_list: list, file_path: str) -> None:
    """Save a list of strings to a text file"""
    try:
        file_path = os.path.abspath(file_path)
        with open(file_path, "w", encoding="utf-8") as output_txt:
            for item in str_list:
                output_txt.write(f"{item}\n")
    except OSError as e:
        logger.error("Error writing text file to %s", file_path)
        logger.error(e)
    else:
        logger.info("Text file saved to %s", file_path)


def get_subfolders_list(repo_root: str, base_folder_path: str = ".", subfolders=None) -> list:
    """Get list of all directories in the specified path and subfolders"""

    if subfolders is None:
        subfolders = set()
    subfolders.add(
        ""
    )  # Ensure set includes blank to cover search of base folder without a subfolder
    folders = []

    try:
        for subfolder in subfolders:
            folder_path = os.path.join(repo_root, base_folder_path, subfolder)
            logger.info("Getting subfolders in: %s", folder_path)
            # Get all entries (files and directories) in the specified path
            folders.extend(
                [
                    os.path.join(base_folder_path, subfolder, item)
                    for item in os.listdir(folder_path)
                ]
            )
            logger.debug("Found folders: %s", folders)

        # Filter for directories
        folders = [folder for folder in folders if os.path.isdir(folder)]
        folders.sort()
        return folders
    except FileNotFoundError:
        logger.error("Error: Directory '%s' not found.", os.path.join(base_folder_path, subfolder))
    except (PermissionError, OSError) as e:
        logger.error(
            "An error occurred while accessing the directory '%s'.",
            os.path.join(base_folder_path, subfolder),
        )
        logger.error(e)


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


def get_component_priority_version_source(component: dict) -> str:
    """Get the priority version source, if defined in metadata file."""
    priority_version_source = [
        p.get("value")
        for p in component.get("properties", [])
        if p.get("name") == "internal:generate_sbom:priority_version_source"
    ]
    if len(priority_version_source):
        # There should only be 1 result, if any
        return priority_version_source[0]
    else:
        return None


def get_import_script_variable_name(component: dict) -> str:
    """Get the variable name used in the import script, if defined in metadata file."""
    import_script_variable_name = [
        p.get("value")
        for p in component.get("properties", [])
        if p.get("name") == "internal:generate_sbom:import_script_variable_name"
    ]
    if len(import_script_variable_name):
        # There should only be 1 result, if any
        return import_script_variable_name[0]
    else:
        return None


def get_version_from_import_script(file_path: str, variable_name: str) -> str:
    """A rudimentary parse of a shell or python script file to extract the static value defined for the VERSION variable"""
    try:
        with open(file_path, "r", encoding="utf-8") as file:
            for line in file:
                if line.strip().startswith(f"{variable_name}="):
                    return re.sub(
                        rf"^{variable_name}=(?P<quote>[\"']?)(?P<content>\S+)(?P=quote).*$",
                        r"\g<content>",
                        line.strip(),
                    )
                elif line.strip().startswith(f"{variable_name} = "):
                    return re.sub(
                        rf"^{variable_name}\s=\s(?P<quote>[\"']?)(?P<content>\S+)(?P=quote).*$",
                        r"\g<content>",
                        line.strip(),
                    )
    except OSError as e:
        logger.warning("Unable to load %s", file_path)
        logger.warning(e)
    else:
        return None


def deduplicate_list_of_dicts(list_of_dicts):
    """Deduplicate a list of dicts while preserving order. Dicts must be hashable (i.e., contain only hashable types)"""
    seen = set()
    unique_list = []
    for d in list_of_dicts:
        # Convert dict items to frozenset for hashability
        frozenset_items = frozenset(d.items())
        if frozenset_items not in seen:
            seen.add(frozenset_items)
            unique_list.append(d)
    return unique_list


# endregion functions and classes


def main() -> None:
    """Main function to generate SBOM"""

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
        help="Target for generated SBOM. Commit: results from running a CI scan, PR: the # of the scanned PR, Branch: results from latest monitoring scan, Project: results from latest monitoring scan of the 'default' branch (default: project)",
        choices=["commit", "pr", "branch", "project"],
        default="project",
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
    exclusive_target.add_argument(
        "--pr",
        help="PR number",
        default=0,
        type=int,
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
        help="Input path for previous SBOM file (Default: './sbom.private.json')",
        default="./sbom.private.json",
        type=str,
    )
    files.add_argument(
        "--sbom-out-public",
        help="Output path for public SBOM file (Default: './sbom.json')",
        default="./sbom.json",
        type=str,
    )
    files.add_argument(
        "--sbom-out-internal",
        help="Output path for internal SBOM file (Default: './sbom.private.json')",
        default="./sbom.private.json",
        type=str,
    )
    parser.add_argument(
        "--branch-filter",
        help="Run only if Git repo branch matches regex (Default: '.*')",
        default=".*",
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

    # Check if branch matches the branch filter regex
    if not re.fullmatch(args.branch_filter, git_info.branch):
        print(
            f"Branch '{git_info.branch}' does not match branch filter '{args.branch_filter}'. Terminating as successful."
        )
        sys.exit(0)

    # files
    sbom_out_public_path = args.sbom_out_public
    sbom_out_internal_path = args.sbom_out_internal
    sbom_in_path = args.sbom_in
    sbom_metadata_path = args.sbom_metadata
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

    endor_bom = None
    if target == "project":
        git_info.branch = "master"
        endor_bom = endorctl.get_sbom(git_info.project)
    else:
        if target == "branch":
            ref = git_info.branch
        elif target == "commit":
            ref = git_info.commit
        elif target == "pr":
            ref = f"pr/{args.pr}"
        endor_bom = endorctl.get_sbom(git_info.project, target, ref)

    if not endor_bom:
        logger.error("Empty result for Endor SBOM!")
        if target in ["commit", "pr"]:
            logger.error(
                f"Check Endor Labs for any unanticipated issues with the target {target} scan."
            )
        else:
            logger.error(f"Check Endor Labs for status of the target {target} monitoring scan.")
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
            if "components" in endor_bom["metadata"]["component"]:
                endor_bom["metadata"]["component"]["components"] = [
                    c
                    for c in endor_bom["metadata"]["component"]["components"]
                    if not c.get("bom-ref", "").startswith(remove)
                ]
            if component["bom-ref"].startswith(remove):
                logger.info("ENDOR SBOM PRE-PROCESS: removing %s", component["bom-ref"])
                del endor_bom["components"][i]
                removed = True
                break
        if not removed:
            for rename in endor_components_rename:
                old = rename[0]
                new = rename[1]
                if component["bom-ref"].startswith(old):
                    # property
                    logger.info(
                        "ENDOR SBOM PRE-PROCESS: replacing start of bom-ref '%s' with '%s'",
                        component["bom-ref"],
                        new,
                    )
                    add_component_property(
                        component, "internal:endor_labs_bom-ref", component["bom-ref"]
                    )
                    component["bom-ref"] = component["bom-ref"].replace(old, new)
                if component["purl"].startswith(old):
                    logger.info(
                        "ENDOR SBOM PRE-PROCESS: replacing start of purl '%s' with '%s'",
                        component["purl"],
                        new,
                    )
                    # add_component_property(component, "Endor Labs purl", component["purl"])
                    component["purl"] = component["purl"].replace(old, new)

    logger.info("Endor Labs SBOM pre-processed with %s components", len(endor_bom["components"]))

    # endregion Pre-process Endor Labs SBOM

    # region load metadata and previous SBOMs

    print_banner("Loading metadata SBOM and previous SBOM")

    if os.path.exists(sbom_metadata_path):
        meta_bom = read_sbom_json_file(sbom_metadata_path)
    else:
        logger.error("No SBOM metadata file at '%s'. This is fatal.", sbom_metadata_path)
        sys.exit(1)

    if os.path.exists(sbom_in_path):
        prev_bom = read_sbom_json_file(sbom_in_path)
    else:
        logger.warning(
            "PREVIOUS SBOM: No previous SBOM file at `%s`. The new SBOM will be generated without any previous context. This is unexpected, but not fatal.",
            sbom_in_path,
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

    # Check metadata SBOM for completeness
    check_metadata_sbom(meta_bom)

    # Create SBOM component lookup dicts
    endor_components = sbom_components_to_dict(endor_bom)
    prev_components = sbom_components_to_dict(prev_bom)

    meta_bom_ref = meta_bom["metadata"]["component"]["bom-ref"]

    # If this is a multi-package SBOM export, add the Endor SBOM metadata.component.components[] as dependencies to the parent component in the metadata SBOM, so they are included in the dependency graph.
    for component in endor_bom["metadata"]["component"].get("components", []):
        add_component_dependsOn(meta_bom["dependencies"], meta_bom_ref, component["bom-ref"])

    # region MongoDB primary component

    # Attempt to determine the MongoDB Version being scanned
    logger.debug(
        "Available MongoDB version options, tag: %s, branch: %s, previous SBOM: %s",
        git_info.release_tag,
        git_info.branch,
        prev_bom["metadata"]["component"]["version"],
    )

    # Project scan always set to 'master' or if using 'master' branch
    if target == "project" or git_info.branch in ["master", "main"]:
        version = git_info.branch
        purl_version = version
        cpe_version = version
        logger.info("Using branch '%s' as MongoDB version", git_info.branch)

    # tagged release. e.g., r8.1.0, r8.2.1-rc0
    elif git_info.release_tag:
        version = git_info.release_tag[1:]  # remove leading 'r'
        purl_version = git_info.release_tag
        cpe_version = version  # without leading 'r'
        logger.info("Using release_tag '%s' as MongoDB version", git_info.release_tag)

    # Release branch staging e.g., v7.0-staging or v8.2-staging
    elif target == "branch" and re.fullmatch(REGEX_RELEASE_BRANCH, git_info.branch):
        version = git_info.branch.replace("-staging", "")
        purl_version = version
        # remove leading 'v', add wildcard. e.g. 8.2.*
        cpe_version = version[1:] + ".*"
        logger.info("Using release branch '%s' as MongoDB version", version)

    # Previous SBOM app version, if all needed specifiers exist
    elif (
        prev_bom.get("metadata", {}).get("component", {}).get("version")
        and prev_bom.get("metadata", {}).get("component", {}).get("purl")
        and prev_bom.get("metadata", {}).get("component", {}).get("cpe")
    ):
        version = prev_bom["metadata"]["component"]["version"]
        purl_version = prev_bom["metadata"]["component"]["purl"].split("@")[-1]
        cpe_version = prev_bom["metadata"]["component"]["cpe"].split(":")[5]
        logger.info("Using previous SBOM version '%s' as MongoDB version", version)

    else:
        # Fall back to the version specified in the Endor SBOM
        # This is unlikely to be accurate
        version = endor_bom["metadata"]["component"]["version"]
        purl_version = version
        cpe_version = version
        logger.warning(
            "Using SBOM version '%s' from Endor Labs scan. This is unlikely to be accurate and may specify a PR #.",
            version,
        )

    # Set main component version
    set_component_version(meta_bom["metadata"]["component"], version, purl_version, cpe_version)

    # Run through 'dependency' objects to set main component version
    set_dependency_version(meta_bom["dependencies"], meta_bom_ref, purl_version)

    # endregion MongoDB primary component

    # region SBOM components

    # region Parse metadata SBOM components

    third_party_folders = get_subfolders_list(
        git_info.repo_root.as_posix(), "src/third_party", {"private"}
    )
    logger.debug("Initial list of 'src/third_party' subfolders: %s", third_party_folders)

    # Convert to a dictionary to count instances folders found in SBOM locations
    third_party_folders = dict.fromkeys(third_party_folders, 0)

    # exclude folders specified in config.py
    for folder in third_party_folders_remove:
        if folder in third_party_folders:
            del third_party_folders[folder]
        else:
            logger.warning(
                "THIRD_PARTY FOLDERS: folder '%s' specified for removal in config.py not found in 'src/third_party' folders list. Consider updating config.py.",
                folder,
            )

    # Load codeowners data for later lookup
    owners = Owners()

    for component in meta_bom["components"]:
        versions = {
            "endor": None,
            "import_script": None,
            "metadata": None,
            "priority_version_source": None,
        }

        component_key = component["bom-ref"].split("@")[0]
        if "properties" not in component:
            component["properties"] = []

        print_banner("Component: " + component_key)

        ############## Priority Version Source ###############
        # Priority version source, if exists
        priority_version_source = get_component_priority_version_source(component)
        if priority_version_source:
            versions["priority_version_source"] = priority_version_source
            logger.info(
                "PRIORITY VERSION SOURCE: %s: Set priority version source to '%s'",
                component_key,
                priority_version_source,
            )

        ################ Endor Labs ################
        if component_key in endor_components:
            # Pop component from dict so we are left with only unmatched components
            endor_component = endor_components.pop(component_key)
            # Preserve Endor Labs component properties, if any
            component["properties"].extend(endor_component.get("properties", []))
            versions["endor"] = endor_component.get("version")
            logger.debug(
                "VERSION ENDOR: %s: Found version '%s' in Endor Labs results",
                component_key,
                versions["endor"],
            )

        ############## Import Script ###############
        # Import script version, if exists
        import_script_path = get_component_import_script_path(component)
        if import_script_path:
            import_script = Path(import_script_path)
            if import_script.exists():
                versions["import_script"] = get_version_from_import_script(
                    import_script_path, get_import_script_variable_name(component) or "VERSION"
                )
                if versions["import_script"]:
                    versions["import_script"] = versions["import_script"].replace("release-", "")
                if versions["import_script"]:
                    logger.debug(
                        "VERSION IMPORT SCRIPT: %s: Found version '%s' in import script '%s'",
                        component_key,
                        versions["import_script"],
                        import_script_path,
                    )
            else:
                logger.debug(
                    "VERSION IMPORT SCRIPT: %s: Import script not found! '%s'",
                    component_key,
                    import_script_path,
                )

        ############## Metadata ###############
        # Hard-coded metadata version, if exists
        if "{{VERSION}}" not in component["version"]:
            versions["metadata"] = component.get("version")

        logger.info("VERSIONS: %s: %s", component_key, str(versions))

        ############## Component Special Cases ###############
        process_component_special_cases(
            component_key, component, versions, git_info.repo_root.as_posix()
        )

        # Log a warning if Endor and import scripts versions do not match
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
                        str(versions["endor"]),
                        "semver(endor):",
                        get_semver_from_release_version(versions["endor"]),
                        "import_script:",
                        str(versions["import_script"]),
                        "semver(import_script):",
                        get_semver_from_release_version(versions["import_script"]),
                        "priority_version_source:",
                        str(versions["priority_version_source"]),
                    ]
                )
            )
            logger.warning(
                "VERSION MISMATCH: %s: Endor version %s; Import script version %s. 'priority_version_source' from metadata: %s",
                component_key,
                versions["endor"],
                versions["import_script"],
                versions["priority_version_source"],
            )

        # For the standard workflow, we favor the pre-set priority version source,
        # followed by Endor Labs version, followed by import script, followed by hard coded
        if versions["priority_version_source"] and versions["priority_version_source"] in versions:
            version = versions[versions["priority_version_source"]]
            logger.info(
                "VERSION: %s: Using priority_version_source '%s' from metadata file.",
                component_key,
                priority_version_source,
            )
        else:
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

            # check against third_party folders and log codeowners if location is defined in evidence occurrences
            component_defines_location = False
            for occurrence in component.get("evidence", {}).get("occurrences", []):
                location = occurrence.get("location")
                if location:
                    component_defines_location = True
                    # Look up the codeowner for the folder and add as a property
                    component_codeowners = owners.get_codeowners(location)
                    logger.debug(
                        "CODEOWNER: %s code owners for location %s are %s",
                        component_key,
                        location,
                        component_codeowners,
                    )
                    if not component_codeowners:
                        component_codeowners = ["unknown"]
                        logger.warning(
                            "CODEOWNER: %s could not determine code owners for location %s",
                            component_key,
                            location,
                        )
                    else:
                        for codeowner in component_codeowners:
                            try:
                                jira_teams = owners.get_jira_team_from_codeowner(codeowner)
                            except KeyError:
                                logger.warning(
                                    "CODEOWNER: %s could not determine JIRA teams for codeowner %s. Mapping may be missing from buildscripts/util/co_jira_map.yml",
                                    component_key,
                                    codeowner,
                                )
                                jira_teams = [codeowner]
                                continue
                            for jira_team in jira_teams:
                                add_component_property(
                                    component, "internal:team_responsible", jira_team
                                )
                                logger.info(
                                    "CODEOWNER: %s code owner team determined to be %s based on location %s",
                                    component_key,
                                    jira_team,
                                    location,
                                )
                if location.startswith("src/third_party/"):
                    if location in third_party_folders:
                        third_party_folders[location] += 1
                        logger.debug(
                            "THIRD_PARTY FOLDER: %s matched folder %s specified in SBOM",
                            component_key,
                            location,
                        )
                    elif os.path.isdir(git_info.repo_root.as_posix() + "/" + location):
                        logger.debug(
                            "THIRD_PARTY FOLDER: %s folder %s specified in SBOM exists",
                            component_key,
                            location,
                        )
                    else:
                        logger.warning(
                            "THIRD_PARTY FOLDER: %s lists third-party location folder as %s, which does not exist!",
                            component_key,
                            location,
                        )
                else:
                    logger.warning(
                        "THIRD_PARTY FOLDER: %s lists a location as '%s'. Ideally, all third-party components are located under 'src/third_party/'.",
                        component_key,
                        location,
                    )
            if not component_defines_location:
                logger.warning(
                    "THIRD_PARTY FOLDER: %s does not define a location in '.evidence.occurrences[]'",
                    component_key,
                )

            # Deduplicate properties list
            component["properties"] = deduplicate_list_of_dicts(component.get("properties", []))

        else:
            logger.warning(
                "VERSION NOT FOUND: Could not find version information for '%s'! Removing from SBOM. Component may need to be removed from the %s file.",
                component_key,
                sbom_metadata_path,
            )
            remove_sbom_component(meta_bom, component_key)

    print_banner("Third Party Folders")
    third_party_folders_missed = {
        key: value for key, value in third_party_folders.items() if value == 0
    }
    if third_party_folders_missed:
        logger.warning(
            "THIRD_PARTY FOLDERS: 'src/third_party' folders not matched with a component: %s",
            ",".join(third_party_folders_missed.keys()),
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
            "ENDOR SBOM: There are %d unmatched components in the Endor Labs SBOM. Adding as-is. The applicable metadata should be added to the metadata SBOM for the next run.",
            len(endor_components),
        )
        for component in endor_components:
            # set scope to excluded by default until the component is evaluated
            endor_components[component]["scope"] = "excluded"

            # Add blank object for missing fields to avoid issues for downstream processing expecting those fields to exist
            if "licenses" not in endor_components[component]:
                endor_components[component]["licenses"] = []
                logger.warning(
                    "LICENSES: %s does not have a 'licenses' field. Adding empty list to component.",
                    endor_components[component]["bom-ref"],
                )
            add_component_property(endor_components[component], "internal:as-is_component", "true")
            meta_bom["components"].append(endor_components[component])

            meta_bom["dependencies"].extend(
                [
                    d
                    for d in endor_bom["dependencies"]
                    if d.get("ref") == endor_components[component]["bom-ref"]
                ]
            )
            if component.startswith(("pkg:github/", "pkg:generic/")):
                logger.warning("SBOM AS-IS COMPONENT: Added %s", component)

    # endregion Parse unmatched Endor Labs components

    # region Finalize SBOM

    # Have the SBOM app version changed?
    sbom_app_version_changed = (
        prev_bom["metadata"]["component"]["version"] != meta_bom["metadata"]["component"]["version"]
    )
    logger.info("SUMMARY: MongoDB version changed: %s", sbom_app_version_changed)

    # Have the components changed?
    prev_components = sbom_components_to_dict(prev_bom, with_version=True)
    meta_components = sbom_components_to_dict(meta_bom, with_version=True)
    sbom_components_changed = prev_components.keys() != meta_components.keys()
    logger.info(
        "SBOM_DIFF: SBOM components changed (added, removed, or version): %s. Previous SBOM has %d components; New SBOM has %d components",
        sbom_components_changed,
        len(prev_components),
        len(meta_components),
    )

    # Components in prev SBOM but not in generated SBOM
    prev_components = sbom_components_to_dict(prev_bom, with_version=False)
    meta_components = sbom_components_to_dict(meta_bom, with_version=False)
    prev_components_diff = list(set(prev_components.keys()) - set(meta_components.keys()))
    if prev_components_diff:
        logger.info(
            "SBOM_DIFF: Components in previous SBOM and not in generated SBOM: %s",
            ",".join(prev_components_diff),
        )

    # Components in generated SBOM but not in prev SBOM
    meta_components_diff = list(set(meta_components.keys()) - set(prev_components.keys()))
    if meta_components_diff:
        logger.info(
            "SBOM_DIFF: Components in generated SBOM and not in previous SBOM: %s",
            ",".join(meta_components_diff),
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

    write_sbom_json_file(meta_bom, sbom_out_internal_path)

    convert_sbom_to_public(meta_bom)
    write_sbom_json_file(meta_bom, sbom_out_public_path)

    # Access the collected warnings
    print_banner("CONSOLIDATED WARNINGS")
    warnings = []
    for record in warning_handler.warnings:
        warnings.append("- " + record.getMessage())
    warnings.sort()

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
