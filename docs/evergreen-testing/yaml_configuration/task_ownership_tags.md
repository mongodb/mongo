# Task ownership tags

This document describes task ownership tags that are used in `mongodb-mongo-master` and `mongodb-mongo-master-nightly` projects.

Every task in in `mongodb-mongo-master` and `mongodb-mongo-master-nightly` projects should be tag with exactly one `assigned_to_jira_team_.+` tag.
Team names (the part after `assigned_to_jira_team_`) should match `evergreen_tag_name` from team configurations in [mothra](https://github.com/10gen/mothra/tree/main/mothra/teams).

This is enforced by linter. YAML linter configuration could be found [here](../../../etc/evergreen_lint.yml).

If the linter configuration is missing your team:

1. Make sure that your team configuration exists or add it in mothra
2. Make sure that your team configuration in mothra has `evergreen_tag_name`
3. Update the tag list with `assigned_to_jira_team_{evergreen_tag_name}` tag for your team
