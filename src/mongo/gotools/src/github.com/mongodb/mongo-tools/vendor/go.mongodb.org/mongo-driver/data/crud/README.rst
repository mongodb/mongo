==========
CRUD Tests
==========

The YAML and JSON files in this directory tree are platform-independent tests
meant to exercise the translation from the API to underlying commands that
MongoDB understands. Given the variety of languages and implementations and
limited nature of a description of a test, there are a number of things that
aren't testable. For instance, none of these tests assert that maxTimeMS was
properly sent to the server. This would involve a lot of infrastructure to
define and setup. Therefore, these YAML tests are in no way a replacement for
more thorough testing. However, they can provide an initial verification of your
implementation.

Version
=======

Files in the "specifications" repository have no version scheme. They are not
tied to a MongoDB server version, and it is our intention that each
specification moves from "draft" to "final" with no further revisions; it is
superseded by a future spec, not revised.

However, implementers must have stable sets of tests to target. As test files
evolve they will occasionally be tagged like "crud-tests-YYYY-MM-DD", until the
spec is final.

Format
======

Each YAML file has the following keys:

- ``data``: The data that should exist in the collection under test before each
  test run.

- ``minServerVersion`` (optional): The minimum server version (inclusive)
  required to successfully run the test. If this field is not present, it should
  be assumed that there is no lower bound on the required server version.

- ``maxServerVersion`` (optional): The maximum server version (exclusive)
  against which this test can run successfully. If this field is not present,
  it should be assumed that there is no upper bound on the required server
  version.

- ``tests``: An array of tests that are to be run independently of each other.
  Each test will have some or all of the following fields:

  - ``description``: The name of the test.

  - ``operation``: Document describing the operation to be executed. This will
    have the following fields:

      - ``name``: The name of the operation as defined in the specification.

      - ``arguments``: The names and values of arguments from the specification.

  - ``outcome``: Document describing the return value and/or expected state of
    the collection after the operation is executed. This will have some or all
    of the following fields:

        - ``result``: The return value from the operation. Note that some tests
          specify an ``upsertedCount`` field when the server does not provide
          one in the result document. In these cases, an ``upsertedCount`` field
          with a value of 0 should be manually added to the document received
          from the server to facilitate comparison.

      - ``collection``:

        - ``name`` (optional): The name of the collection to verify. If this
          isn't present then use the collection under test.

        - ``data``: The data that should exist in the collection after the
          operation has been run.

Use as integration tests
========================

Running these as integration tests will require a running mongod server. Each of
these tests is valid against a standalone mongod, a replica set, and a sharded
system for server version 3.0 and later. Many of them will run against 2.6, but
some will require conditional code.
