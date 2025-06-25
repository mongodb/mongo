import glob

import yaml

if __name__ == "__main__":
    approvers = set()
    owners_paths = glob.glob("**/OWNERS.yml", recursive=True)
    print(len(owners_paths))
    for path in owners_paths:
        with open(path, "r", encoding="utf8") as owner_file:
            contents = yaml.safe_load(owner_file)
            if "filters" not in contents:
                continue
            for file_filter in contents["filters"]:
                assert "approvers" in file_filter
                approvers.update(set(file_filter["approvers"]))

    f = open("co_jira_map.yml", "w+", encoding="utf8")
    yaml.dump({approver: "jira_team" for approver in approvers}, f)
