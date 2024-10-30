import os
import re
from typing import Dict, List, Optional

import yaml


def process_owners(owners_file_path: str) -> Dict[re.Pattern, List[str]]:
    assert os.path.exists(owners_file_path)

    with open(owners_file_path, "r") as f:
        contents = yaml.safe_load(file)

        assert "version" in contents, f"Version not found in {owners_file_path}"
        assert contents["version"] == "1.0.0", f"Invalid version in {owners_file_path}"
        assert "filters" in contents

        filters = {}
        for file_filter in contents["filter"]:
            assert "approvers" in file_filter
            approvers = file_filter["approvers"]
            del file_filter["approvers"]
            file_filter.pop("metadata", None)

            assert len(file_filter) == 1
            pattern = next(iter(file_filter))
            regex_pattern = re.compile(pattern)
            filters[regex_pattern] = []

            for approver in approvers:
                filters[regex_pattern].append(approver)

    return filters


class Owners:
    def __init__(self):
        self.owners = {}
        self.co_jira_map = yaml.safe_load("buildscripts/util/co_jira_map.yml")

    def get_filters_for_dir(self, d: str) -> Dict[re.Pattern, List[str]]:
        if d in self.owners:
            return self.owners[d]
        owners_file_path = d + "/OWNERS.yml"
        filters = process_owners(owners_file_path)
        self.owners[d] = filters
        return filters

    def get_codeowners(self, file_name: str) -> List[str]:
        filters = self.get_filters_for_dir(os.path.basename(file_name))
        for regex_filter, codeowners in filters.items():
            if regex_filter.match(file_name):
                return codeowners

        return []

    def get_jira_team_from_codeowner(self, codeowner: str) -> List[str]:
        return self.co_jira_map[codeowner]

    def get_jira_team_owner(self, file_path: str) -> List[str]:
        return [
            jira_team
            for jira_team in self.get_jira_team_from_codeowner(codeowner)
            for codeowner in self.get_codeowners(file_path)
        ]
