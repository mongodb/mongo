# Code Owners

## OWNERS.yml File Format

This is loosely based on [kubernetes](https://www.kubernetes.dev/docs/guide/owners/) and [chromium](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/code_reviews.md) OWNERS files.

`version` is the current version of the `OWNERS.yml` file format. This should always be 1.0.0.

`aliases` point to yaml files files that list aliases that can be used in this OWNERS.yml file.

`filters` are a list of globs that match [gitignore syntax](https://git-scm.com/docs/gitignore#_pattern_format). The filter must match at least once file and be unique to the file. Each filter must have a list of `approvers`. An approval from any single approver will allow the code to be merged. Each filter may have a list of `emeritus_approvers`. `emeritus_approvers` are folks that used to be approvers that no longer have approver privileges. This allows us to keep track of folks who built up a knowledge base of this code that might need to be consulted in a critical situation. Both `approvers` and `emeritus_approvers` should be either github usernames, emails, or aliases.

`options` are not required and are various options about how to use this OWNERS.yml file. Currently there is only a single option `no_parent_owners` which is defaulted to false. If this option is set to true it will stop upwards OWNERS resolution.

### Example file

```yaml
version: 1.0.0 # Should always be 1.0.0
aliases:
  # Contains the markdown-approvers alias
  - //buildscripts/resmokelib/devprod_correctness_aliases.yml
filters: # List of all filters
  - "*": # Select all files (will apply recursively)
    approvers: # Anyone on this list can approve PRs
      - devprod-correctness # alias for a group of users
      - IamXander # github username
      - user.name@mongodb.com # email address
    emeritus_approvers: # This list is just for historical reference
      - userB
  - "*.md":
    approvers:
      - markdown-approvers
options: # All options for this file
  no_parent_owners: false # See above for no_parent_owners. Defaulted to false so this line is not needed.
```

### Aliases Yaml File Format

`version` is the current version of the aliases file format. This should always be 1.0.0.

`aliases` are a list of group names. Each group name must have one or more reviewers. Reviewers should be github usernames.

## Example File

```yaml
version: 1.0.0
aliases:
  devprod-build:
    - IamXander # github username
    - user.name@mongodb.com # email address
```

## Filter resolution

a/b/c/OWNERS.yml

```yaml
version: 1.0.0
aliases:
  - //aliases.yml
filters:
  - "*.py":
    approvers:
      - teamC
  - "*.md":
    approvers:
      - teamMD
```

a/b/OWNERS.yml

```yaml
version: 1.0.0
aliases:
  - //aliases.yml
filters:
  - "*.json":
    approvers:
      - teamB
  - "*.py":
    approvers:
      - teamPY
options:
  no_parent_owners: true
```

a/OWNERS.yml

```yaml
version: 1.0.0
aliases:
  - //aliases.yml
filters:
  - "*":
    approvers:
      - teamA
  - "*.yaml":
    approvers:
      - teamYAML
```

### Example 1

If someone changes `a/b/c/file.py` the owner resolution will select teamC since the first file searched is `a/b/c/OWNERS.yml` First we compare if `file.py` matches `*.md`. It does not so we now check if `file.py` matches `*`. It does match so teamC is selected for review.

### Example 2

If someone changes `a/b/c/file.yaml` the owner resolution will not find a team. The first file searched is `a/b/c/OWNERS.yml`. No filters match file.yaml. Next we search in `a/b/OWNERS.yml`. No filters match there either. We stop searching up because `no_parent_owners` is set to true.
