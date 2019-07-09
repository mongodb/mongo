.. role:: javascript(code)
  :language: javascript

==============
Change Streams
==============

.. contents::

--------

Introduction
============

The YAML and JSON files in this directory are platform-independent tests that
drivers can use to prove their conformance to the Change Streams Spec.

Several prose tests, which are not easily expressed in YAML, are also presented
in this file. Those tests will need to be manually implemented by each driver.

Spec Test Format
================

Each YAML file has the following keys:

- ``database_name``: The default database
- ``collection_name``: The default collection
- ``database2_name``: Another database
- ``collection2_name``: Another collection
- ``tests``: An array of tests that are to be run independently of each other.
  Each test will have some of the following fields:

  - ``description``: The name of the test.
  - ``minServerVersion``: The minimum server version to run this test against. If not present, assume there is no minimum server version.
  - ``maxServerVersion``: Reserved for later use
  - ``failPoint``(optional): The configureFailPoint command document to run to configure a fail point on the primary server.
  - ``target``: The entity on which to run the change stream. Valid values are:
  
    - ``collection``: Watch changes on collection ``database_name.collection_name``
    - ``database``: Watch changes on database ``database_name``
    - ``client``: Watch changes on entire clusters
  - ``topology``: An array of server topologies against which to run the test.
    Valid topologies are ``single``, ``replicaset``, and ``sharded``.
  - ``changeStreamPipeline``: An array of additional aggregation pipeline stages to add to the change stream
  - ``changeStreamOptions``: Additional options to add to the changeStream
  - ``operations``: Array of documents, each describing an operation. Each document has the following fields:

    - ``database``: Database against which to run the operation
    - ``collection``: Collection against which to run the operation
    - ``name``: Name of the command to run
    - ``arguments`` (optional): Object of arguments for the command (ex: document to insert)

  - ``expectations``: Optional list of command-started events in Extended JSON format
  - ``result``: Document with ONE of the following fields:

    - ``error``: Describes an error received during the test
    - ``success``: An Extended JSON array of documents expected to be received from the changeStream

Spec Test Match Function
========================

The definition of MATCH or MATCHES in the Spec Test Runner is as follows:

- MATCH takes two values, ``expected`` and ``actual``
- Notation is "Assert [actual] MATCHES [expected]
- Assertion passes if ``expected`` is a subset of ``actual``, with the values ``42`` and ``"42"`` acting as placeholders for "any value"

Pseudocode implementation of ``actual`` MATCHES ``expected``:

::
  
  If expected is "42" or 42:
    Assert that actual exists (is not null or undefined)
  Else:
    Assert that actual is of the same JSON type as expected
    If expected is a JSON array:
      For every idx/value in expected:
        Assert that actual[idx] MATCHES value
    Else if expected is a JSON object:
      For every key/value in expected
        Assert that actual[key] MATCHES value
    Else:
      Assert that expected equals actual

The expected values for ``result.success`` and ``expectations`` are written in Extended JSON. Drivers may adopt any of the following approaches to comparisons, as long as they are consistent:

- Convert ``actual`` to Extended JSON and compare to ``expected``
- Convert ``expected`` and ``actual`` to BSON, and compare them
- Convert ``expected`` and ``actual`` to native equivalents of JSON, and compare them

Spec Test Runner
================

Before running the tests

- Create a MongoClient ``globalClient``, and connect to the server

For each YAML file, for each element in ``tests``:

- If ``topology`` does not include the topology of the server instance(s), skip this test.
- Use ``globalClient`` to

  - Drop the database ``database_name``
  - Drop the database ``database2_name``
  - Create the database ``database_name`` and the collection ``database_name.collection_name``
  - Create the database ``database2_name`` and the collection ``database2_name.collection2_name``
  - If the the ``failPoint`` field is present, configure the fail point on the primary server. See
    `Server Fail Point <../../transactions/tests#server-fail-point>`_ in the
    Transactions spec test documentation for more information.

- Create a new MongoClient ``client``
- Begin monitoring all APM events for ``client``. (If the driver uses global listeners, filter out all events that do not originate with ``client``). Filter out any "internal" commands (e.g. ``isMaster``)
- Using ``client``, create a changeStream ``changeStream`` against the specified ``target``. Use ``changeStreamPipeline`` and ``changeStreamOptions`` if they are non-empty
- Using ``globalClient``, run every operation in ``operations`` in serial against the server
- Wait until either:

  - An error occurs
  - All operations have been successful AND the changeStream has received as many changes as there are in ``result.success``

- Close ``changeStream``
- If there was an error:

  - Assert that an error was expected for the test.
  - Assert that the error MATCHES ``result.error``

- Else:

  - Assert that no error was expected for the test
  - Assert that the changes received from ``changeStream`` MATCH the results in ``result.success``

- If there are any ``expectations``

  - For each (``expected``, ``idx``) in ``expectations``

    - Assert that ``actual[idx]`` MATCHES ``expected``

- Close the MongoClient ``client``

After running all tests

- Close the MongoClient ``globalClient``
- Drop database ``database_name``
- Drop database ``database2_name``


Prose Tests
===========

The following tests have not yet been automated, but MUST still be tested

#. ``ChangeStream`` must continuously track the last seen ``resumeToken``
#. ``ChangeStream`` will throw an exception if the server response is missing the resume token (if wire version is < 8, this is a driver-side error; for 8+, this is a server-side error)
#. ``ChangeStream`` will automatically resume one time on a resumable error (including `not master`) with the initial pipeline and options, except for the addition/update of a ``resumeToken``.
#. ``ChangeStream`` will not attempt to resume on any error encountered while executing an ``aggregate`` command.
#. ``ChangeStream`` will not attempt to resume after encountering error code 11601 (Interrupted), 136 (CappedPositionLost), or 237 (CursorKilled) while executing a ``getMore`` command.
#. ``ChangeStream`` will perform server selection before attempting to resume, using initial ``readPreference``
#. Ensure that a cursor returned from an aggregate command with a cursor id and an initial empty batch is not closed on the driver side.
#. The ``killCursors`` command sent during the "Resume Process" must not be allowed to throw an exception.
#. ``$changeStream`` stage for ``ChangeStream`` against a server ``>=4.0`` and ``<4.0.7`` that has not received any results yet MUST include a ``startAtOperationTime`` option when resuming a changestream.
#. ``ChangeStream`` will resume after a ``killCursors`` command is issued for its child cursor.
#. - For a ``ChangeStream`` under these conditions:
      - Running against a server ``>=4.0.7``.
      - The batch is empty or has been iterated to the last document.
   - Expected result: 
       - ``getResumeToken`` must return the ``postBatchResumeToken`` from the current command response.
#. - For a ``ChangeStream`` under these conditions:
      - Running against a server ``<4.0.7``.
      - The batch is empty or has been iterated to the last document.
   - Expected result: 
      - ``getResumeToken`` must return the ``_id`` of the last document returned if one exists.
      - ``getResumeToken`` must return ``startAfter`` from the initial aggregate if the option was specified.
      - ``getResumeToken`` must return ``resumeAfter`` from the initial aggregate if the option was specified.
      - If neither the ``startAfter`` nor ``resumeAfter`` options were specified, the ``getResumeToken`` result must be empty.
#. - For a ``ChangeStream`` under these conditions:
      - The batch is not empty.
      - The batch has been iterated up to but not including the last element.
   - Expected result:
      - ``getResumeToken`` must return the ``_id`` of the previous document returned.
#. - For a ``ChangeStream`` under these conditions:
      - The batch is not empty.
      - The batch hasn’t been iterated at all.
      - Only the initial ``aggregate`` command has been executed.
   - Expected result:
      - ``getResumeToken`` must return ``startAfter`` from the initial aggregate if the option was specified.
      - ``getResumeToken`` must return ``resumeAfter`` from the initial aggregate if the option was specified.
      - If neither the ``startAfter`` nor ``resumeAfter`` options were specified, the ``getResumeToken`` result must be empty.
#. - For a ``ChangeStream`` under these conditions:
      - Running against a server ``>=4.0.7``.
      - The batch is not empty.
      - The batch hasn’t been iterated at all.
      - The stream has iterated beyond a previous batch and a ``getMore`` command has just been executed.
   - Expected result:
      - ``getResumeToken`` must return the ``postBatchResumeToken`` from the previous command response.
#. - For a ``ChangeStream`` under these conditions:
      - Running against a server ``<4.0.7``.
      - The batch is not empty.
      - The batch hasn’t been iterated at all.
      - The stream has iterated beyond a previous batch and a ``getMore`` command has just been executed.
   - Expected result:
      - ``getResumeToken`` must return the ``_id`` of the previous document returned if one exists.
      - ``getResumeToken`` must return ``startAfter`` from the initial aggregate if the option was specified.
      - ``getResumeToken`` must return ``resumeAfter`` from the initial aggregate if the option was specified.
      - If neither the ``startAfter`` nor ``resumeAfter`` options were specified, the ``getResumeToken`` result must be empty.