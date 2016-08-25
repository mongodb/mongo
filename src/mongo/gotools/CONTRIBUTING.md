Contributing to the MongoDB Tools Project
===================================

Pull requests are always welcome, and the MongoDB engineering team appreciates any help the community can give to make the MongoDB tools better.

For any particular improvement you want to make, you can begin a discussion on the
[MongoDB Developers Forum](https://groups.google.com/forum/?fromgroups#!forum/mongodb-dev).  This is the best place to discuss your proposed improvement (and its
implementation) with the core development team.

If you're interested in contributing, we have a list of some suggested tickets that are easy enough to get started on here:
https://jira.mongodb.org/issues/?jql=project%20%3D%20TOOLS%20AND%20labels%20%3D%20community%20and%20status%20%3D%20open

Getting Started
---------------

1. Create a [MongoDB JIRA account](https://jira.mongodb.org/secure/Signup!default.jspa).
2. Create a [Github account](https://github.com/signup/free).
3. [Fork](https://help.github.com/articles/fork-a-repo/) the repository on Github at https://github.com/mongodb/mongo-tools.
4. For more details see http://www.mongodb.org/about/contributors/.

JIRA Tickets
------------

1. File a JIRA ticket in the [TOOLS project](https://jira.mongodb.org/browse/TOOLS).
2. All commit messages to the MongoDB Tools repository must be prefaced with the relevant JIRA ticket number e.g. "TOOLS-XXX: add support for xyz".

In filing JIRA tickets for bugs, please clearly describe the issue you are resolving, including the platforms on which the issue is present and clear steps to reproduce.

For improvements or feature requests, be sure to explain the goal or use case, and the approach
your solution will take.

Style Guide
-----------

All commits to the MongoDB Tools repository must pass golint:

```go run vendor/src/github.com/3rf/mongo-lint/golint/golint.go mongo* bson* common/*```

_We use a modified version of [golint](https://github.com/golang/lint)_

Testing
-------

To run unit and integration tests:

```
go test -v -test.types=unit,integration
```

This should be run in all package directories - common, mongorestore, mongoexport, etc. 

The `test.types` flag indicates what kinds of tests to run. Integration tests require a `mongod` (running on port 33333) while unit tests do not.

To run the quality assurance tests, you need to have the latest stable version of the rebuilt tools, `mongod`, `mongos`, and `mongo` in your current working directory. 

```
cd test/qa-tests
python buildscripts/smoke.py bson export files import oplog restore stat top
```
_Some tests require older binaries that are named accordingly (e.g. `mongod-2.4`, `mongod-2.6`, etc). You can use [setup_multiversion_mongodb.py](test/qa-tests/buildscripts/setup_multiversion_mongodb.py) to download those binaries_
