# Code Owners

## BANNED_CODEOWNERS.txt File Format

This file enumerates code owners that are not allowed to own code.

Banned owners should be separated by newlines. Empty lines and lines starting with '#' are ignored.

### Example file

```
# Only assign ownership to sub-teams of Organization Team.
10gen/server-organization-team
```

### Configuration

This can be configured in any repo with `bazel_rules_mongo` by putting the following lines in your `.bazelrc` file:

```
common --define codeowners_have_banned_codeowners=True
common --define codeowners_banned_codeowners_file_path=.github/BANNED_CODEOWNERS.txt
```
