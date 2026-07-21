# Copilot instructions for this repository

## Flag likely CVE / security-vulnerability fixes

Commits on `master` are automatically synced to the public `mongodb/mongo` repository. A security
fix that lands on `master` before the corresponding CVE is disclosed publicly reveals the
vulnerability while released versions are still unpatched.

When reviewing a pull request, flag it prominently if it looks like a fix for a security
vulnerability or CVE. Signals include:

- The PR, commit message, or linked ticket mentions a CVE, "vulnerability", "security fix",
  "embargo", or a security-team ticket.
- The change fixes spoofing, forgery, injection, authentication/authorization bypass, privilege
  escalation, out-of-bounds access, or similar exploitable behavior.
- The change hardens a trust boundary (e.g. validating previously trusted input from the network, a
  proxy header, or another node) or corrects audit-log attribution.
- Documentation changes that describe how existing behavior can be abused by an attacker.

If any of these apply, add a review comment telling the author:

> This PR looks like it may fix a security vulnerability. Because `master` is synced to the public
> repository, this change must only be merged after the corresponding CVE has been publicly
> disclosed and fixed releases are available. Confirm disclosure timing with the security team
> before merging.

Do not speculate about exploit details in review comments; just flag the disclosure-timing concern.
