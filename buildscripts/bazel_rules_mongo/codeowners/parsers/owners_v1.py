import glob
import os
import pathlib
from functools import cache
from typing import Dict, List

import yaml


# Parser for OWNERS.yml files version 1.0.0
class OwnersParserV1:
    def parse(self, directory: str, owners_file_path: str, contents: Dict[str, any]) -> List[str]:
        lines = []
        no_parent_owners = False
        if "options" in contents:
            options = contents["options"]
            no_parent_owners = "no_parent_owners" in options and options["no_parent_owners"]

        if no_parent_owners:
            # Specfying no owners will ensure that no file in this directory has an owner unless it
            # matches one of the later patterns in the file.
            lines.append(self.get_owner_line(directory, pattern="*", owners=None))

        aliases = {}
        if "aliases" in contents:
            for alias_file in contents["aliases"]:
                aliases.update(self.process_alias_import(alias_file))
        if "filters" in contents:
            filters = contents["filters"]
            for _filter in filters:
                assert (
                    "approvers" in _filter
                ), f"Filter in {owners_file_path} does not have approvers."
                approvers = _filter["approvers"]
                del _filter["approvers"]
                if "metadata" in _filter:
                    del _filter["metadata"]

                # the last key remaining should be the pattern for the filter
                assert len(_filter) == 1, f"Filter in {owners_file_path} has incorrect values."
                pattern = next(iter(_filter))
                owners: set[str] = set()

                def process_owner(owner: str):
                    if "@" in owner:
                        # approver is email, just add as is
                        if not owner.endswith("@mongodb.com"):
                            raise RuntimeError("Any emails specified must be a mongodb.com email.")
                        owners.add(owner)
                    else:
                        # approver is github username, need to prefix with @
                        owners.add(f"@{owner}")

                NOOWNERS_NAME = "NOOWNERS"
                if NOOWNERS_NAME in approvers:
                    assert (
                        len(approvers) == 1
                    ), f"{NOOWNERS_NAME} must be the only approver when it is used."
                else:
                    for approver in approvers:
                        if approver in aliases:
                            for member in aliases[approver]:
                                process_owner(member)
                        else:
                            process_owner(approver)
                    # Add the auto revert bot
                    if self.should_add_auto_approver():
                        process_owner("svc-auto-approve-bot")

                lines.append(self.get_owner_line(directory, pattern, owners))
        return lines

    @cache
    def process_alias_import(self, path: str) -> Dict[str, List[str]]:
        if not path.startswith("//"):
            raise RuntimeError(
                f"Alias file paths must start with // and be relative to the repo root: {path}"
            )

        # remove // from beginning of path
        parsed_path = path[2::]

        if not os.path.exists(parsed_path):
            raise RuntimeError(f"Could not find alias file {path}")

        with open(parsed_path, "r", encoding="utf8") as file:
            contents = yaml.safe_load(file)
            assert "version" in contents, f"Version not found in {path}"
            assert "aliases" in contents, f"Alias not found in {path}"
            assert contents["version"] == "1.0.0", f"Unsupported version in {path}"
            return contents["aliases"]

    def get_owner_line(self, directory: str, pattern: str, owners: set[str]) -> str:
        # ensure the path is correct and consistent on all platforms
        directory = pathlib.PurePath(directory).as_posix()

        if directory == ".":
            # we are in the root dir and can directly pass the pattern
            parsed_pattern = pattern
        elif not pattern:
            # If there is no pattern add the directory as the pattern.
            parsed_pattern = f"/{directory}/"
        elif "/" in pattern:
            # if the pattern contains a slash the pattern should be treated as relative to the
            # directory it came from.
            if pattern.startswith("/"):
                parsed_pattern = f"/{directory}{pattern}"
            else:
                parsed_pattern = f"/{directory}/{pattern}"
        else:
            parsed_pattern = f"/{directory}/**/{pattern}"

        if not self.test_pattern(parsed_pattern):
            raise (RuntimeError(f"Can not find any files that match pattern: `{pattern}`"))

        return self.get_line(parsed_pattern, owners)

    def test_pattern(self, pattern: str) -> bool:
        test_pattern = f".{pattern}" if pattern.startswith("/") else f"./{pattern}"

        # ensure at least one file patches the pattern.
        first_file_found = glob.iglob(test_pattern, recursive=True)
        if all(False for _ in first_file_found):
            return False
        return True

    def get_line(self, pattern: str, owners: set[str]) -> str:
        if owners:
            return f"{pattern} {' '.join(sorted(owners))}"
        else:
            return pattern

    @cache
    def should_add_auto_approver(self) -> bool:
        env_opt = os.environ.get("ADD_AUTO_APPROVE_USER")
        if env_opt and env_opt.lower() == "true":
            return True
        return False
