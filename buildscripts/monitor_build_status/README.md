# Block-on-Red

> **TL;DR:** During times of high BF volume, code approvals and merging in 10gen/mongo master will be restricted to only allow changes that help reduce BFs, Bugs, Performance Regressions, and paying down technical debt.

### Motivation

The master branch should remain stable to develop the Server efficiently, and to be within 30 days of releasing at all times. If it becomes too unstable, or "too red," we want to aggressively focus on getting it back into the green. As a side benefit to releasability, a "greener" build should make patch build failures more meaningful. This will also reduce release time stress by having the release time period look and feel more like normal business.

### Strategy

Each team carries a quota (see below for details). When a team exceeds their quota - they enter a "code lockdown".

- **Team Level**: The intention here is to stop work with a small blast radius in the first instance, and address the releasability risk from that team and their owned code.
- **VP Level**: We roll the quotas up to a VP’s entire organization as the next step of "code lockdown". The expectation is that redirecting resources within a VP’s organization to help address BFs is likely more effective and less disruptive than a global freeze.
- **Global Level**: Finally, if the global quota is exceeded, the entire server organization enters a "code lockdown" until we meet the threshold for unfreezing.

## Impact of a "Code Lockdown"

### Allowed Code Changes

During a "code lockdown," Code Owners are expected to only approve **work that closes BFs or helps us reduce/avoid the _next_ Blocking state**. i.e. aimed at fixing a BF, a class of BFs, bugs, performance regression, etc.

If your PR does not meet this criteria, it may be pending for some time until the system becomes unblocked. There are of course reasonable exceptions, below.

### Feature Work

**All feature work stops** during a "code lockdown."
In exceptional circumstances VPs can approve exceptions.

### Non-feature Work

We understand that in many cases addressing the larger BF problem requires refactoring, modularity improvements, changes to our test and paying down other kinds of **technical debt**. During a "code lockdown" this work is **expressly permitted and mergeable** - with the guidance that teams index heavily on risk when deciding what to work on. If a piece of work feels like it makes the BF problem worse before it gets better, talk to your director about how to proceed.

Allowable Examples (not exclusive):

- Refactoring components to make them more unit testable
- Increasing code coverage through high quality tests that block PRs
- Making the development loop faster (decreasing build times, fixing slow tests, etc)
- Improving guardrails that improve code quality (fixing clang-tidy warnings, compiler warnings, etc)

If a team is in a lockdown, but the rest of the org is not - their focus should likely skew towards work that expedites their lockdown exit.

If the org is in a lockdown, but a team doesn’t have BFs to work on - they should balance helping other teams with the work they’ve identified as addressing the underlying BF problem.

The higher the risk of the work, the more involvement the Staff+ engineers and the Director/VP should have in the decision about what is ok to merge and what isn’t.

### Code Owner Responsibilities

Code Owners should join the `#10gen-mongo-code-lockdown` Slack channel to receive daily updates on the status of the build. It produces daily metrics with instructions if there is a state change.

If we change to a blocking state, code owners should use their discretion to only approve changes that are allowed (see above). If we exit the blocking state, code owners should approve PRs as usual.

## Quotas and State-Changes

Currently monitored thresholds:

| **Quota** | **Team**<br>(older than 48h) | **VP**<br>(older than 7d) | **Global**<br>(older than 7d) |
| --------- | ---------------------------- | ------------------------- | ----------------------------- |
| Hot       | 6                            | 16                        | 60                            |
| Cold      | 10                           | 32                        | 100                           |

Source-of-truth implementation: [etc/code_lockdown.yml](../../etc/code_lockdown.yml).

### Dashboards

A dashboard is available at [go/issue-quota](http://go/issue-quota).

This shows relevant JIRA queries for a more live and interactive view of the state.

- [This filter](https://jira.mongodb.org/issues/?filter=53085) describes all the relevant issues
- [This filter](https://jira.mongodb.org/issues/?filter=53200) determines if something is hot

## FAQ

### BFs remaining open only on older branches

Some teams may fix a BF in master, but are "waiting for fix" on older branches, which keeps the BF counted against the thresholds. Guidance here is currently evolving.

If the build failure is not frequently occurring, it can be marked as P5-Trivial, and it won’t count towards your team’s build failures for the block merge.

As we iterate on our processes for this, the `exclude-from-master-quota` label can be used to exclude BFs that should not be included in these quotas. The expectation is that this is an interim solution as we improve our processes especially around BFs that remain open pending backports.

Specifically:

- If a BF is only waiting for a backport on a branch older than master, apply the `exclude-from-master-quota` label to the ticket.
- If a BF is failing on master, not a serious bug (or a test-only issue that can't affect the real clients), not noisy, and we are choosing not to fix it, set the Priority to `P5 - Trivial` and apply the `keep-trivial` label.
- If a BF is failing on an older branch and we are choosing not to backport a fix, set the `Priority to P5 - Trivial` and apply the `keep-trivial-X.Y` label appropriately.

## Contributing

For any new proposals, changes to thresholds, or concerns regarding their application, please escalate to your Director/VP. **We want advocacy from all levels to make this a successful change to our engineering culture.**

### CLI

Run the following to read about supported options:

```
python buildscripts/monitor_build_status/cli.py --help
```

### Testing locally

For Jira API authentication, use the `JIRA_AUTH_PAT` env variable. More about Jira Personal Access Tokens (PATs) can be found [here](https://wiki.corp.mongodb.com/pages/viewpage.action?pageId=218995581).

Use your PAT to run the following and output its results:

```
JIRA_AUTH_PAT=<auth-token> python buildscripts/monitor_build_status/cli.py
```

The above will _not_ spam the slack channel, unless you explicitly use `--notify`.
