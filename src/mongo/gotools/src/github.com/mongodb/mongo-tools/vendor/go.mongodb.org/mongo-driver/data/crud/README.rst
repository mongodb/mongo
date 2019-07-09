==========
CRUD Tests
==========

.. contents::

----

Introduction
============

The YAML and JSON files in this directory tree are platform-independent tests
that drivers can use to prove their conformance to the CRUD spec.

Given the variety of languages and implementations and limited nature of a
description of a test, there are a number of things that aren't testable. For
instance, none of these tests assert that maxTimeMS was properly sent to the
server. This would involve a lot of infrastructure to define and setup.
Therefore, these YAML tests are in no way a replacement for more thorough
testing. However, they can provide an initial verification of your
implementation.

Running these integration tests will require a running MongoDB server or
cluster with server versions 2.6.0 or later. Some tests have specific server
version requirements as noted by ``minServerVersion`` and ``maxServerVersion``.

Version
=======

Files in the "specifications" repository have no version scheme. They are not
tied to a MongoDB server version.

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

    - ``name``: The name of the operation as defined in the specification. The
      name `db-aggregate` refers to database-level aggregation.

    - ``database_name`` (optional): The name of the database against which to execute the operation.

    - ``arguments``: The names and values of arguments from the specification.

  - ``outcome``: Document describing the return value and/or expected state of
    the collection after the operation is executed. This will have some or all
    of the following fields:

    - ``error``: If ``true``, the test should expect an error or exception. Note
      that some drivers may report server-side errors as a write error within a
      write result object.

    - ``result``: The return value from the operation. This will correspond to
      an operation's result object as defined in the CRUD specification. This
      field may be omitted if ``error`` is ``true``. If this field is present
      and ``error`` is ``true`` (generally for multi-statement tests), the
      result reports information about operations that succeeded before an
      unrecoverable failure. In that case, drivers may choose to check the
      result object if their BulkWriteException (or equivalent) provides access
      to a write result object.

    - ``collection``:

      - ``name`` (optional): The name of the collection to verify. If this isn't
        present then use the collection under test.

      - ``data``: The data that should exist in the collection after the
        operation has been run.

Expectations
============

Expected results for some tests may include optional fields, such as
``insertedId`` (for InsertOneResult), ``insertedIds`` (for InsertManyResult),
and ``upsertedCount`` (for UpdateResult). Drivers that do not implement these
fields can ignore them.
