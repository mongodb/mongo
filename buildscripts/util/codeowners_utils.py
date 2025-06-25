import fnmatch
import os
import re
from functools import lru_cache
from typing import Dict, List, Tuple

import yaml


@lru_cache
def process_owners(cur_dir: str) -> Tuple[Dict[re.Pattern, List[str]], bool]:
    if not cur_dir:
        return ({}, False)

    owners_file_path = cur_dir + "/OWNERS.yml"
    if not os.path.exists(owners_file_path):
        return process_owners(os.path.dirname(cur_dir))

    with open(owners_file_path, "r", encoding="utf8") as f:
        contents = yaml.safe_load(f)

        assert "version" in contents, f"Version not found in {owners_file_path}"
        assert contents["version"] == "1.0.0", f"Invalid version in {owners_file_path}"
        assert "filters" in contents

        no_parent_owners = False
        if "options" in contents:
            options = contents["options"]
            no_parent_owners = "no_parent_owners" in options and options["no_parent_owners"]

        filters = {}
        for file_filter in contents["filters"]:
            assert "approvers" in file_filter
            approvers = file_filter["approvers"]
            del file_filter["approvers"]
            file_filter.pop("metadata", None)

            assert len(file_filter) == 1
            pattern = next(iter(file_filter))
            pattern = f"{cur_dir}/{pattern}"

            regex_pattern = re.compile(fnmatch.translate(pattern))
            filters[regex_pattern] = []

            for approver in approvers:
                filters[regex_pattern].append(approver)

    return (filters, no_parent_owners)


class Owners:
    def __init__(self):
        self.co_jira_map = yaml.safe_load(
            open("buildscripts/util/co_jira_map.yml", "r", encoding="utf8")
        )

    def get_codeowners(self, file_path: str) -> List[str]:
        cur_dir = os.path.dirname(file_path)
        codeowners = []
        # search up tree until matching filter found
        while True:
            filters, no_parent = process_owners(cur_dir)
            if not cur_dir:
                break
            # latest applicable filter takes precedence
            for regex_filter, cur_codeowners in filters.items():
                if regex_filter.fullmatch(file_path):
                    codeowners = cur_codeowners
            if codeowners or no_parent:
                break
            cur_dir = os.path.dirname(cur_dir)
        return codeowners

    def get_jira_team_from_codeowner(self, codeowner: str) -> List[str]:
        return self.co_jira_map[codeowner]

    def get_jira_team_owner(self, file_path: str) -> List[str]:
        return [
            jira_team
            for codeowner in self.get_codeowners(file_path)
            for jira_team in self.get_jira_team_from_codeowner(codeowner)
        ]
