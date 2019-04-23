# Contributing to the MongoDB Go Driver

Thank you for your interest in contributing to the MongoDB Go driver.

We are building this software together and strongly encourage contributions from the community that are within the guidelines set forth
below.

## Bug Fixes and New Features

Before starting to write code, look for existing [tickets](https://jira.mongodb.org/browse/GODRIVER) or
[create one](https://jira.mongodb.org/secure/CreateIssue!default.jspa) for your bug, issue, or feature request. This helps the community
avoid working on something that might not be of interest or which has already been addressed.

## Pull Requests & Patches

The Go Driver team is experimenting with GerritHub for contributions. GerritHub uses GitHub for authentication and uses a patch based
workflow. Since GerritHub supports importing of Pull Requests we will also accept Pull Requests, but Code Review will be done in
GerritHub.

Patches should generally be made against the master (default) branch and include relevant tests, if applicable.

Code should compile and tests should pass under all go versions which the driver currently supports.  Currently the driver
supports a minimum version of go 1.7. Please ensure the following tools have been run on the code: gofmt, golint, errcheck,
go test (with coverage and with the race detector), and go vet. For convenience, you can run 'make' to run all these tools.
**By default, running the tests requires that you have a mongod server running on localhost, listening on the default port.**
At minimum, please test against the latest release version of the MongoDB server.

If any tests do not pass, or relevant tests are not included, the patch will not be considered.

If you are working on a bug or feature listed in Jira, please include the ticket number prefixed with GODRIVER in the commit,
e.g. GODRIVER-123. For the patch commit message itself, please follow the [How to Write a Git Commit Message](https://chris.beams.io/posts/git-commit/) guide.

## Talk To Us

If you want to work on the driver, write documentation, or have questions/complaints, please reach out to use either via
the [mongo-go-driver Google Group](https://groups.google.com/forum/#!forum/mongodb-go-driver) or by creating a Question
issue at (https://jira.mongodb.org/secure/CreateIssue!default.jspa).
