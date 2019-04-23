=======================
Connection String Tests
=======================

The YAML and JSON files in this directory tree are platform-independent tests
that drivers can use to prove their conformance to the Read and Write Concern 
specification.

Version
-------

Files in the "specifications" repository have no version scheme. They are not
tied to a MongoDB server version, and it is our intention that each
specification moves from "draft" to "final" with no further versions; it is
superseded by a future spec, not revised.

However, implementers must have stable sets of tests to target. As test files
evolve they will be occasionally tagged like "uri-tests-tests-2015-07-16", until
the spec is final.

Format
------

Connection String
~~~~~~~~~~~~~~~~~

These tests are designed to exercise the connection string parsing related
to read concern and write concern.

Each YAML file contains an object with a single ``tests`` key. This key is an
array of test case objects, each of which have the following keys:

- ``description``: A string describing the test.
- ``uri``: A string containing the URI to be parsed.
- ``valid:``: a boolean indicating if parsing the uri should result in an error.
- ``writeConcern:`` A document indicating the expected write concern.
- ``readConcern:`` A document indicating the expected read concern.

If a test case includes a null value for one of these keys, or if the key is missing,
no assertion is necessary. This both simplifies parsing of the test files and allows flexibility
for drivers that might substitute default values *during* parsing.

Document
~~~~~~~~

These tests are designed to ensure compliance with the spec in relation to what should be 
sent to the server.

Each YAML file contains an object with a single ``tests`` key. This key is an
array of test case objects, each of which have the following keys:

- ``description``: A string describing the test.
- ``uri``: A string containing the URI to be parsed.
- ``valid:``: a boolean indicating if the write concern created from the document is valid.
- ``writeConcern:`` A document indicating the write concern to use.
- ``writeConcernDocument:`` A document indicating the write concern to be sent to the server.
- ``readConcern:`` A document indicating the read concern to use.
- ``readConcernDocument:`` A document indicating the read concern to be sent to the server.
- ``isServerDefault:`` Indicates whether the read or write concern is considered the server's default.
- ``isAcknowledged:`` Indicates if the write concern should be considered acknowledged.

Use as unit tests
=================

Testing whether a URI is valid or not should simply be a matter of checking
whether URI parsing raises an error or exception.
Testing for emitted warnings may require more legwork (e.g. configuring a log
handler and watching for output).
