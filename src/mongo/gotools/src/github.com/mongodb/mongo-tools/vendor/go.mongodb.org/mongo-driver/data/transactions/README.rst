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

- ``database_name`` and ``collection_name``: The database and collection to use
  for testing.

- ``data``: The data that should exist in the collection under test before each
  test run.

- ``tests``: An array of tests that are to be run independently of each other.
  Each test will have some or all of the following fields:

  - ``description``: The name of the test.

  - ``skipReason``: Optional, string describing why this test should be
    skipped.

  - ``clientOptions``: Optional, parameters to pass to MongoClient().

  - ``failPoint``: Optional, a server failpoint to enable expressed as the
    configureFailPoint command to run on the admin database.

  - ``sessionOptions``: Optional, parameters to pass to
    MongoClient.startSession().

  - ``operations``: Array of documents, each describing an operation to be
    executed. Each document has the following fields:

    - ``name``: The name of the operation on ``object``.

    - ``object``: The name of the object to perform the operation on. Can be
      "database", "collection", "session0", or "session1".

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
**server version 4.1.6 or later**. Including multiple mongoses (and
initializing the MongoClient with multiple mongos seeds!) ensures that
mongos transaction pinning works properly.

Load each YAML (or JSON) file using a Canonical Extended JSON parser.

Then for each element in ``tests``:

#. If the``skipReason`` field is present, skip this test completely.
#. Create a MongoClient and call
   ``client.admin.runCommand({killAllSessions: []})`` to clean up any open
   transactions from previous test failures.

   To workaround `SERVER-38335`_, ensure this command does not send
   an implicit session, otherwise the command will fail with an
   "operation was interrupted" error because it kills itself and (on a sharded
   cluster) future commands may fail with an error similar to:
   "Encountered error from localhost:27217 during a transaction :: caused by :: operation was interrupted".

   If your driver cannot run this command without an implicit session, then
   either skip this step and live with the fact that previous test failures
   may cause later tests to fail or use the `killAllSessionsByPattern` command
   instead. During each test record all session ids sent to the server and at
   the end of each test kill all the sessions ids (using a different session):

   .. code:: python

      # Be sure to use a distinct session to avoid "operation was interrupted".
      session_for_cleanup = client.start_session()
      recorded_session_uuids = []
      # Run test case and record session uuids...
      client.admin.runCommand({
          'killAllSessionsByPattern': [
              {'lsid': {'id': uuid}} for uuid in recorded_session_uuids]},
          session=session_for_cleanup)

   - When testing against a sharded cluster, create a list of MongoClients that
     are directly connected to each mongos. Run the `killAllSessions`
     (or `killAllSessionsByPattern`) command on ALL mongoses.

   .. _SERVER-38335: https://jira.mongodb.org/browse/SERVER-38335

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

   - When testing against a sharded cluster run this command on ALL mongoses.

#. Create a **new** MongoClient ``client``, with Command Monitoring listeners
   enabled. (Using a new MongoClient for each test ensures a fresh session pool
   that hasn't executed any transactions previously, so the tests can assert
   actual txnNumbers, starting from 1.) Pass this test's ``clientOptions`` if
   present.

   - When testing against a sharded cluster this client MUST be created with
     multiple (valid) mongos seed addreses.

#. Call ``client.startSession`` twice to create ClientSession objects
   ``session0`` and ``session1``, using the test's "sessionOptions" if they
   are present. Save their lsids so they are available after calling
   ``endSession``, see `Logical Session Id`.
#. For each element in ``operations``:

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

   - When testing against a sharded cluster run this command on ALL mongoses.

#. For each element in ``outcome``:

   - If ``name`` is "collection", verify that the test collection contains
     exactly the documents in the ``data`` array. Ensure this find reads the
     latest data by using **primary read preference** with
     **local read concern** even when the MongoClient is configured with
     another read preference or read concern.

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
