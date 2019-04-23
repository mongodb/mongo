=====================
Retryable Write Tests
=====================

.. contents::

----

Introduction
============

The YAML and JSON files in this directory tree are platform-independent tests
that drivers can use to prove their conformance to the Retryable Writes spec.

Several prose tests, which are not easily expressed in YAML, are also presented
in this file. Those tests will need to be manually implemented by each driver.

Tests will require a MongoClient created with options defined in the tests.
Integration tests will require a running MongoDB cluster with server versions
3.6.0 or later. The ``{setFeatureCompatibilityVersion: 3.6}`` admin command
will also need to have been executed to enable support for retryable writes on
the cluster.

Server Fail Point
=================

The tests depend on a server fail point, ``onPrimaryTransactionalWrite``, which
allows us to force a network error before the server would return a write result
to the client. The fail point also allows control whether the server will
successfully commit the write via its ``failBeforeCommitExceptionCode`` option.
Keep in mind that the fail point only triggers for transaction writes (i.e. write
commands including ``txnNumber`` and ``lsid`` fields). See `SERVER-29606`_ for
more information.

.. _SERVER-29606: https://jira.mongodb.org/browse/SERVER-29606

The fail point may be configured like so::

    db.runCommand({
        configureFailPoint: "onPrimaryTransactionalWrite",
        mode: <string|document>,
        data: <document>
    });

``mode`` is a generic fail point option and may be assigned a string or document
value. The string values ``"alwaysOn"`` and ``"off"`` may be used to enable or
disable the fail point, respectively. A document may be used to specify either
``times`` or ``skip``, which are mutually exclusive:

- ``{ times: <integer> }`` may be used to limit the number of times the fail
  point may trigger before transitioning to ``"off"``.
- ``{ skip: <integer> }`` may be used to defer the first trigger of a fail
  point, after which it will transition to ``"alwaysOn"``.

The ``data`` option is a document that may be used to specify options that
control the fail point's behavior. As noted in `SERVER-29606`_,
``onPrimaryTransactionalWrite`` supports the following ``data`` options, which
may be combined if desired:

- ``closeConnection``: Boolean option, which defaults to ``true``. If ``true``,
  the connection on which the write is executed will be closed before a result
  can be returned.
- ``failBeforeCommitExceptionCode``: Integer option, which is unset by default.
  If set, the specified exception code will be thrown and the write will not be
  committed. If unset, the write will be allowed to commit.

Disabling Fail Point after Test Execution
-----------------------------------------

After each test that configures a fail point, drivers should disable the
``onPrimaryTransactionalWrite`` fail point to avoid spurious failures in
subsequent tests. The fail point may be disabled like so::

    db.runCommand({
        configureFailPoint: "onPrimaryTransactionalWrite",
        mode: "off"
    });

Network Error Tests
===================

Network error tests are expressed in YAML and should be run against a replica
set. These tests cannot be run against a shard cluster because mongos does not
support the necessary fail point.

The tests exercise the following scenarios:

- Single-statement write operations

  - Each test expecting a write result will encounter at-most one network error
    for the write command. Retry attempts should return without error and allow
    operation to succeed. Observation of the collection state will assert that
    the write occurred at-most once.

  - Each test expecting an error will encounter successive network errors for
    the write command. Observation of the collection state will assert that the
    write was never committed on the server.

- Multi-statement write operations

  - Each test expecting a write result will encounter at-most one network error
    for some write command(s) in the batch. Retry attempts should return without
    error and allow the batch to ultimately succeed. Observation of the
    collection state will assert that each write occurred at-most once.

  - Each test expecting an error will encounter successive network errors for
    some write command in the batch. The batch will ultimately fail with an
    error, but observation of the collection state will assert that the failing
    write was never committed on the server. We may observe that earlier writes
    in the batch occurred at-most once.

We cannot test a scenario where the first and second attempts both encounter
network errors but the write does actually commit during one of those attempts.
This is because (1) the fail point only triggers when a write would be committed
and (2) the skip and times options are mutually exclusive. That said, such a
test would mainly assert the server's correctness for at-most once semantics and
is not essential to assert driver correctness.

Test Format
-----------

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

  - ``clientOptions``: Parameters to pass to MongoClient().

  - ``failPoint``: The ``configureFailPoint`` command document to run to
    configure a fail point on the primary server. Drivers must ensure that
    ``configureFailPoint`` is the first field in the command.

  - ``operation``: Document describing the operation to be executed. The
    operation should be executed through a collection object derived from a
    client that has been created with ``clientOptions``. The operation will have
    some or all of the following fields:

    - ``name``: The name of the operation as defined in the CRUD specification.

    - ``arguments``: The names and values of arguments from the CRUD
      specification.

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

Split Batch Tests
=================

The YAML tests specify bulk write operations that are split by command type
(e.g. sequence of insert, update, and delete commands). Multi-statement write
operations may also be split due to ``maxWriteBatchSize``,
``maxBsonObjectSize``, or ``maxMessageSizeBytes``.

For instance, an insertMany operation with five 10 MB documents executed using
OP_MSG payload type 0 (i.e. entire command in one document) would be split into
five insert commands in order to respect the 16 MB ``maxBsonObjectSize`` limit.
The same insertMany operation executed using OP_MSG payload type 1 (i.e. command
arguments pulled out into a separate payload vector) would be split into two
insert commands in order to respect the 48 MB ``maxMessageSizeBytes`` limit.

Noting when a driver might split operations, the ``onPrimaryTransactionalWrite``
fail point's ``skip`` option may be used to control when the fail point first
triggers. Once triggered, the fail point will transition to the ``alwaysOn``
state until disabled. Driver authors should also note that the server attempts
to process all documents in a single insert command within a single commit (i.e.
one insert command with five documents may only trigger the fail point once).
This behavior is unique to insert commands (each statement in an update and
delete command is processed independently).

If testing an insert that is split into two commands, a ``skip`` of one will
allow the fail point to trigger on the second insert command (because all
documents in the first command will be processed in the same commit). When
testing an update or delete that is split into two commands, the ``skip`` should
be set to the number of statements in the first command to allow the fail point
to trigger on the second command.

Replica Set Failover Test
=========================

In addition to network errors, writes should also be retried in the event of a
primary failover, which results in a "not master" command error (or similar).
The ``stepdownHangBeforePerformingPostMemberStateUpdateActions`` fail point
implemented in `d4eb562`_ for `SERVER-31355`_ may be used for this test, as it
allows a primary to keep its client connections open after a step down. This
fail point operates by hanging the step down procedure (i.e. ``replSetStepDown``
command) until the fail point is later deactivated.

.. _d4eb562: https://github.com/mongodb/mongo/commit/d4eb562ac63717904f24de4a22e395070687bc62
.. _SERVER-31355: https://jira.mongodb.org/browse/SERVER-31355

The following test requires three MongoClient instances and will generally
require two execution contexts (async drivers may get by with a single thread).

- The client under test will connect to the replica set and be used to execute
  write operations.
- The fail point client will connect directly to the initial primary and be used
  to toggle the fail point.
- The step down client will connect to the replica set and be used to step down
  the primary. This client will generally require its own execution context,
  since the step down will hang.

In order to guarantee that the client under test does not detect the stepped
down primary's state change via SDAM, it must be configured with a large
`heartbeatFrequencyMS`_ value (e.g. 60 seconds). Single-threaded drivers may
also need to set `serverSelectionTryOnce`_ to ``false`` to ensure that server
selection for the retry attempt waits until a new primary is elected.

.. _heartbeatFrequencyMS: https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#heartbeatfrequencyms
.. _serverSelectionTryOnce: https://github.com/mongodb/specifications/blob/master/source/server-selection/server-selection.rst#serverselectiontryonce

The test proceeds as follows:

- Using the client under test, insert a document and observe a successful write
  result. This will ensure that initial discovery takes place.
- Using the fail point client, activate the fail point by setting ``mode``
  to ``"alwaysOn"``.
- Using the step down client, step down the primary by executing the command
  ``{ replSetStepDown: 60, force: true}``. This operation will hang so long as
  the fail point is activated. When the fail point is later deactivated, the
  step down will complete and the primary's client connections will be dropped.
  At that point, any ensuing network error should be ignored.
- Using the client under test, insert a document and observe a successful write
  result. The test MUST assert that the insert command fails once against the
  stepped down node and is successfully retried on the newly elected primary
  (after SDAM discovers the topology change). The test MAY use APM or another
  means to observe both attempts.
- Using the fail point client, deactivate the fail point by setting ``mode``
  to ``"off"``.

Command Construction Tests
==========================

Drivers should also assert that command documents are properly constructed with
or without a transaction ID, depending on whether the write operation is
supported. `Command Monitoring`_ may be used to check for the presence of a
``txnNumber`` field in the command document. Note that command documents may
always include an ``lsid`` field per the `Driver Session`_ specification.

.. _Command Monitoring: ../../command-monitoring/command-monitoring.rst
.. _Driver Session: ../../sessions/driver-sessions.rst

These tests may be run against both a replica set and shard cluster.

Drivers should test that transaction IDs are never included in commands for
unsupported write operations:

* Write commands with unacknowledged write concerns (e.g. ``{w: 0}``)

* Unsupported single-statement write operations

  - ``updateMany()``
  - ``deleteMany()``

* Unsupported multi-statement write operations

  - ``bulkWrite()`` that includes ``UpdateMany`` or ``DeleteMany``

* Unsupported write commands

  - ``aggregate`` with ``$out`` pipeline operator

Drivers should test that transactions IDs are always included in commands for
supported write operations:

* Supported single-statement write operations

  - ``insertOne()``
  - ``updateOne()``
  - ``replaceOne()``
  - ``deleteOne()``
  - ``findOneAndDelete()``
  - ``findOneAndReplace()``
  - ``findOneAndUpdate()``

* Supported multi-statement write operations

  - ``insertMany()`` with ``ordered=true``
  - ``insertMany()`` with ``ordered=false``
  - ``bulkWrite()`` with ``ordered=true`` (no ``UpdateMany`` or ``DeleteMany``)
  - ``bulkWrite()`` with ``ordered=false`` (no ``UpdateMany`` or ``DeleteMany``)
