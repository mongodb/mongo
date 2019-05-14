==================
Transactions Tests
==================

.. contents::

----

Introduction
============

The YAML and JSON files in this directory are platform-independent tests that
drivers can use to prove their conformance to the Transactions Spec. They are
designed with the intention of sharing some test-runner code with the CRUD Spec
tests and the Command Monitoring Spec tests.

Several prose tests, which are not easily expressed in YAML, are also presented
in this file. Those tests will need to be manually implemented by each driver.

Server Fail Point
=================

Some tests depend on a server fail point, expressed in the ``failPoint`` field.
For example the ``failCommand`` fail point allows the client to force the
server to return an error. Keep in mind that the fail point only triggers for
commands listed in the "failCommands" field. See `SERVER-35004`_ and
`SERVER-35083`_ for more information.

.. _SERVER-35004: https://jira.mongodb.org/browse/SERVER-35004
.. _SERVER-35083: https://jira.mongodb.org/browse/SERVER-35083

The ``failCommand`` fail point may be configured like so::

    db.adminCommand({
        configureFailPoint: "failCommand",
        mode: <string|document>,
        data: {
          failCommands: ["commandName", "commandName2"],
          closeConnection: <true|false>,
          errorCode: <Number>,
          writeConcernError: <document>
        }
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
control the fail point's behavior. ``failCommand`` supports the following
``data`` options, which may be combined if desired:

- ``failCommands``: Required, the list of command names to fail.
- ``closeConnection``: Boolean option, which defaults to ``false``. If
  ``true``, the connection on which the command is executed will be closed
  and the client will see a network error.
- ``errorCode``: Integer option, which is unset by default. If set, the
  specified command error code will be returned as a command error.
- ``writeConcernError``: A document, which is unset by default. If set, the
  server will return this document in the "writeConcernError" field. This
  failure response only applies to commands that support write concern and
  happens *after* the command finishes (regardless of success or failure).

Test Format
===========

Each YAML file has the following keys:

- ``topology``: Optional. An array of server topologies against which to run
  the test. Valid topologies are "single", "replicaset" and "sharded". The
  default is all topologies (ie ``[single, replicaset", sharded]``).

- ``database_name`` and ``collection_name``: The database and collection to use
  for testing.

- ``data``: The data that should exist in the collection under test before each
  test run.

- ``tests``: An array of tests that are to be run independently of each other.
  Each test will have some or all of the following fields:

  - ``description``: The name of the test.

  - ``skipReason``: Optional, string describing why this test should be
    skipped.

  - ``useMultipleMongoses``: Optional, boolean. If true and this test is
    running against a sharded cluster, intialize the MongoClient for this
    test with multiple mongos seed addresses.

  - ``clientOptions``: Optional, parameters to pass to MongoClient().

  - ``failPoint``: Optional, a server failpoint to enable expressed as the
    configureFailPoint command to run on the admin database. This option and
    ``useMultipleMongoses: true`` are mutually exclusive.

  - ``sessionOptions``: Optional, parameters to pass to
    MongoClient.startSession().

  - ``operations``: Array of documents, each describing an operation to be
    executed. Each document has the following fields:

    - ``name``: The name of the operation on ``object``.

    - ``object``: The name of the object to perform the operation on. Can be
      "database", "collection", "session0", "session1", or "testRunner". See
      the "targetedFailPoint" operation in `Special Test Operations`_.

    - ``collectionOptions``: Optional, parameters to pass to the Collection()
      used for this operation.

    - ``command_name``: Present only when ``name`` is "runCommand". The name
      of the command to run. Required for languages that are unable preserve
      the order keys in the "command" argument when parsing JSON/YAML.

    - ``arguments``: Optional, the names and values of arguments.

    - ``result``: The return value from the operation, if any. This field may
      be a single document or an array of documents in the case of a
      multi-document read. If the operation is expected to return an error, the
      ``result`` is a single document that has one or more of the following
      fields:

      - ``errorContains``: A substring of the expected error message.

      - ``errorCodeName``: The expected "codeName" field in the server
        error response.

      - ``errorLabelsContain``: A list of error label strings that the
        error is expected to have.

      - ``errorLabelsOmit``: A list of error label strings that the
        error is expected not to have.

  - ``expectations``: Optional list of command-started events.

  - ``outcome``: Document describing the return value and/or expected state of
    the collection after the operation is executed. Contains the following
    fields:

    - ``collection``:

      - ``data``: The data that should exist in the collection after the
        operations have run.

Use as integration tests
========================

Run a MongoDB replica set with a primary, a secondary, and an arbiter,
**server version 4.0.0 or later**. (Including a secondary ensures that
server selection in a transaction works properly. Including an arbiter helps
ensure that no new bugs have been introduced related to arbiters.)

A driver that implements support for sharded transactions MUST also run these
tests against a MongoDB sharded cluster with multiple mongoses and
**server version 4.2 or later**. Some tests require
initializing the MongoClient with multiple mongos seeds to ensures that mongos
transaction pinning and the recoveryToken works properly.

Load each YAML (or JSON) file using a Canonical Extended JSON parser.

Then for each element in ``tests``:

#. If the ``skipReason`` field is present, skip this test completely.
#. Create a MongoClient and call
   ``client.admin.runCommand({killAllSessions: []})`` to clean up any open
   transactions from previous test failures.

   - Running ``killAllSessions`` cleans up any open transactions from
     a previously failed test to prevent the current test from blocking.
     It is sufficient to run this command once before starting the test suite
     and once after each failed test.
   - When testing against a sharded cluster run this command on ALL mongoses.

#. Create a collection object from the MongoClient, using the ``database_name``
   and ``collection_name`` fields of the YAML file.
#. Drop the test collection, using writeConcern "majority".
#. Execute the "create" command to recreate the collection, using writeConcern
   "majority". (Creating the collection inside a transaction is prohibited, so
   create it explicitly.)
#. If the YAML file contains a ``data`` array, insert the documents in ``data``
   into the test collection, using writeConcern "majority".
#. If ``failPoint`` is specified, its value is a configureFailPoint command.
   Run the command on the admin database to enable the fail point.
#. Create a **new** MongoClient ``client``, with Command Monitoring listeners
   enabled. (Using a new MongoClient for each test ensures a fresh session pool
   that hasn't executed any transactions previously, so the tests can assert
   actual txnNumbers, starting from 1.) Pass this test's ``clientOptions`` if
   present.

   - When testing against a sharded cluster and ``useMultipleMongoses`` is
     ``true`` the client MUST be created with multiple (valid) mongos seed
     addreses.

#. Call ``client.startSession`` twice to create ClientSession objects
   ``session0`` and ``session1``, using the test's "sessionOptions" if they
   are present. Save their lsids so they are available after calling
   ``endSession``, see `Logical Session Id`.
#. For each element in ``operations``:

   - If the operation ``name`` is a special test operation type, execute it and
     go to the next operation, otherwise proceed to the next step.
   - Enter a "try" block or your programming language's closest equivalent.
   - Create a Database object from the MongoClient, using the ``database_name``
     field at the top level of the test file.
   - Create a Collection object from the Database, using the
     ``collection_name`` field at the top level of the test file.
     If ``collectionOptions`` is present create the Collection object with the
     provided options. Otherwise create the object with the default options.
   - Execute the named method on the provided ``object``, passing the
     arguments listed. Pass ``session0`` or ``session1`` to the method,
     depending on which session's name is in the arguments list.
     If ``arguments`` contains no "session", pass no explicit session to the
     method.
   - If the driver throws an exception / returns an error while executing this
     series of operations, store the error message and server error code.
   - If the result document has an "errorContains" field, verify that the
     method threw an exception or returned an error, and that the value of the
     "errorContains" field matches the error string. "errorContains" is a
     substring (case-insensitive) of the actual error message.

     If the result document has an "errorCodeName" field, verify that the
     method threw a command failed exception or returned an error, and that
     the value of the "errorCodeName" field matches the "codeName" in the
     server error response.

     If the result document has an "errorLabelsContain" field, verify that the
     method threw an exception or returned an error. Verify that all of the
     error labels in "errorLabelsContain" are present in the error or exception
     using the ``hasErrorLabel`` method.

     If the result document has an "errorLabelsOmit" field, verify that the
     method threw an exception or returned an error. Verify that none of the
     error labels in "errorLabelsOmit" are present in the error or exception
     using the ``hasErrorLabel`` method.
   - If the operation returns a raw command response, eg from ``runCommand``,
     then compare only the fields present in the expected result document.
     Otherwise, compare the method's return value to ``result`` using the same
     logic as the CRUD Spec Tests runner.

#. Call ``session0.endSession()`` and ``session1.endSession``.
#. If the test includes a list of command-started events in ``expectations``,
   compare them to the actual command-started events using the
   same logic as the Command Monitoring Spec Tests runner, plus the rules in
   the Command-Started Events instructions below.
#. If ``failPoint`` is specified, disable the fail point to avoid spurious
   failures in subsequent tests. The fail point may be disabled like so::

    db.adminCommand({
        configureFailPoint: <fail point name>,
        mode: "off"
    });

#. For each element in ``outcome``:

   - If ``name`` is "collection", verify that the test collection contains
     exactly the documents in the ``data`` array. Ensure this find reads the
     latest data by using **primary read preference** with
     **local read concern** even when the MongoClient is configured with
     another read preference or read concern.

Special Test Operations
```````````````````````

Certain operations that appear in the "operations" array do not correspond to
API methods but instead represent special test operations. Such operations are
defined on the "testRunner" object and documented here:

targetedFailPoint
~~~~~~~~~~~~~~~~~

The "targetedFailPoint" operation instructs the test runner to configure a fail
point on a specific mongos. The mongos to run the ``configureFailPoint`` is
determined by the "session" argument (either "session0" or "session1").
The session must already be pinned to a mongos server. The "failPoint" argument
is the ``configureFailPoint`` command to run.

If a test uses ``targetedFailPoint``, disable the fail point after running
all ``operations`` to avoid spurious failures in subsequent tests. The fail
point may be disabled like so::

    db.adminCommand({
        configureFailPoint: <fail point name>,
        mode: "off"
    });

Here is an example which instructs the test runner to enable the failCommand
fail point on the mongos server which "session0" is pinned to::

      # Enable the fail point only on the Mongos that session0 is pinned to.
      - name: targetedFailPoint
        object: testRunner
        arguments:
          session: session0
          failPoint:
            configureFailPoint: failCommand
            mode: { times: 1 }
            data:
              failCommands: ["commitTransaction"]
              closeConnection: true

assertSessionPinned
~~~~~~~~~~~~~~~~~~~

The "assertSessionPinned" operation instructs the test runner to assert that
the given session is pinned to a mongos::

      - name: assertSessionPinned
        object: testRunner
        arguments:
          session: session0

assertSessionUnpinned
~~~~~~~~~~~~~~~~~~~~~

The "assertSessionUnpinned" operation instructs the test runner to assert that
the given session is not pinned to a mongos::

      - name: assertSessionPinned
        object: testRunner
        arguments:
          session: session0

Command-Started Events
``````````````````````

The event listener used for these tests MUST ignore the security commands
listed in the Command Monitoring Spec.

Logical Session Id
~~~~~~~~~~~~~~~~~~

Each command-started event in ``expectations`` includes an ``lsid`` with the
value "session0" or "session1". Tests MUST assert that the command's actual
``lsid`` matches the id of the correct ClientSession named ``session0`` or
``session1``.

Null Values
~~~~~~~~~~~

Some command-started events in ``expectations`` include ``null`` values for
fields such as ``txnNumber``, ``autocommit``, and ``writeConcern``.
Tests MUST assert that the actual command **omits** any field that has a
``null`` value in the expected command.

Cursor Id
^^^^^^^^^

A ``getMore`` value of ``"42"`` in a command-started event is a fake cursorId
that MUST be ignored. (In the Command Monitoring Spec tests, fake cursorIds are
correlated with real ones, but that is not necessary for Transactions Spec
tests.)

afterClusterTime
^^^^^^^^^^^^^^^^

A ``readConcern.afterClusterTime`` value of ``42`` in a command-started event
is a fake cluster time. Drivers MUST assert that the actual command includes an
afterClusterTime.

recoveryToken
^^^^^^^^^^^^^

A ``recoveryToken`` value of ``42`` in a command-started event is a
placeholder for an arbitrary recovery token. Drivers MUST assert that the
actual command includes a "recoveryToken" field and SHOULD assert that field
is a BSON document.

Mongos Pinning Prose Tests
==========================

The following tests ensure that a ClientSession is properly unpinned after
a sharded transaction. Initialize these tests with a MongoClient connected
to multiple mongoses.

These tests use a cursor's address field to track which server an operation
was run on. If this is not possible in your driver, use command monitoring
instead.

#. Test that starting a new transaction on a pinned ClientSession unpins the
   session and normal server selection is performed for the next operation.

   .. code:: python

      @require_server_version(4, 1, 6)
      @require_mongos_count_at_least(2)
      def test_unpin_for_next_transaction(self):
        # Increase localThresholdMS and wait until both nodes are discovered
        # to avoid false positives.
        client = MongoClient(mongos_hosts, localThresholdMS=1000)
        wait_until(lambda: len(client.nodes) > 1)
        # Create the collection.
        client.test.test.insert_one({})
        with client.start_session() as s:
          # Session is pinned to Mongos.
          with s.start_transaction():
            client.test.test.insert_one({}, session=s)

          addresses = set()
          for _ in range(50):
            with s.start_transaction():
              cursor = client.test.test.find({}, session=s)
              assert next(cursor)
              addresses.add(cursor.address)

          assert len(addresses) > 1

#. Test non-transaction operations using a pinned ClientSession unpins the
   session and normal server selection is performed.

   .. code:: python

      @require_server_version(4, 1, 6)
      @require_mongos_count_at_least(2)
      def test_unpin_for_non_transaction_operation(self):
        # Increase localThresholdMS and wait until both nodes are discovered
        # to avoid false positives.
        client = MongoClient(mongos_hosts, localThresholdMS=1000)
        wait_until(lambda: len(client.nodes) > 1)
        # Create the collection.
        client.test.test.insert_one({})
        with client.start_session() as s:
          # Session is pinned to Mongos.
          with s.start_transaction():
            client.test.test.insert_one({}, session=s)

          addresses = set()
          for _ in range(50):
            cursor = client.test.test.find({}, session=s)
            assert next(cursor)
            addresses.add(cursor.address)

          assert len(addresses) > 1

Q & A
=====

Why do some tests appear to hang for 60 seconds on a sharded cluster?
`````````````````````````````````````````````````````````````````````

There are two cases where this can happen. When the initial commitTransaction
attempt fails on mongos A and is retried on mongos B, mongos B will block
waiting for the transaction to complete. However because the initial commit
attempt failed, the command will only complete after the transaction is
automatically aborted for exceeding the shard's
transactionLifetimeLimitSeconds setting. `SERVER-39726`_ requests that
recovering the outcome of an uncommitted transaction should immediately abort
the transaction.

The second case is when a *single-shard* transaction is committed successfully
on mongos A and then explicitly committed again on mongos B. Mongos B will also
block until the transactionLifetimeLimitSeconds timeout is hit at which point
``{ok:1}`` will be returned. `SERVER-39349`_ requests that recovering the
outcome of a completed single-shard transaction should not block.
Note that this test suite only includes single shard transactions.

To workaround these issues, drivers SHOULD decrease the transaction timeout
setting by running setParameter **on each shard**. Setting the timeout to 3
seconds significantly speeds up the test suite without a high risk of
prematurely timing out any tests' transactions. To decrease the timeout, run::

  db.adminCommand( { setParameter: 1, transactionLifetimeLimitSeconds: 3 } )

Note that mongo-orchestration >=0.6.13 automatically sets this timeout to 3
seconds so drivers using mongo-orchestration do not need to run these commands
manually.

.. _SERVER-39726: https://jira.mongodb.org/browse/SERVER-39726

.. _SERVER-39349: https://jira.mongodb.org/browse/SERVER-39349

**Changelog**
=============

:2019-02-28: ``useMultipleMongoses: true`` and non-targeted fail points are
             mutually exclusive.
:2019-02-13: Modify test format for 4.2 sharded transactions, including
             "useMultipleMongoses", ``object: testRunner``, the
             ``targetedFailPoint`` operation, and recoveryToken assertions.
