Contributing to the BongoDB Tools Project
===================================

Pull requests are always welcome, and the BongoDB engineering team appreciates any help the community can give to make the BongoDB tools better.

For any particular improvement you want to make, you can begin a discussion on the
[BongoDB Developers Forum](https://groups.google.com/forum/?fromgroups#!forum/bongodb-dev).  This is the best place to discuss your proposed improvement (and its
implementation) with the core development team.

If you're interested in contributing, we have a list of some suggested tickets that are easy enough to get started on here:
https://jira.bongodb.org/issues/?jql=project%20%3D%20TOOLS%20AND%20labels%20%3D%20community%20and%20status%20%3D%20open

Getting Started
---------------

1. Create a [BongoDB JIRA account](https://jira.bongodb.org/secure/Signup!default.jspa).
2. Create a [Github account](https://github.com/signup/free).
3. [Fork](https://help.github.com/articles/fork-a-repo/) the repository on Github at https://github.com/bongodb/bongo-tools.
4. For more details see http://www.bongodb.org/about/contributors/.

JIRA Tickets
------------

1. File a JIRA ticket in the [TOOLS project](https://jira.bongodb.org/browse/TOOLS).
2. All commit messages to the BongoDB Tools repository must be prefaced with the relevant JIRA ticket number e.g. "TOOLS-XXX: add support for xyz".

In filing JIRA tickets for bugs, please clearly describe the issue you are resolving, including the platforms on which the issue is present and clear steps to reproduce.

For improvements or feature requests, be sure to explain the goal or use case, and the approach
your solution will take.

Style Guide
-----------

All commits to the BongoDB Tools repository must pass golint:

```go run vendor/src/github.com/3rf/bongo-lint/golint/golint.go bongo* bson* common/*```

_We use a modified version of [golint](https://github.com/golang/lint)_

Testing
-------

To run unit and integration tests:

```
go test -v -test.types=unit,integration
```

This should be run in all package directories - common, bongorestore, bongoexport, etc. 

The `test.types` flag indicates what kinds of tests to run. Integration tests require a `bongod` (running on port 33333) while unit tests do not.

To run the quality assurance tests, you need to have the latest stable version of the rebuilt tools, `bongod`, `bongos`, and `bongo` in your current working directory. 

```
cd test/qa-tests
python buildscripts/smoke.py bson export files import oplog restore stat top
```
_Some tests require older binaries that are named accordingly (e.g. `bongod-2.4`, `bongod-2.6`, etc). You can use [setup_multiversion_bongodb.py](test/qa-tests/buildscripts/setup_multiversion_bongodb.py) to download those binaries_
