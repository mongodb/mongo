.. role:: javascript(code)
  :language: javascript

========================================
Connection Monitoring and Pooling (CMAP)
========================================

.. contents::

--------

Introduction
============

The YAML and JSON files in this directory are platform-independent tests that
drivers can use to prove their conformance to the Connection Monitoring and Pooling (CMAP) Spec.

Several prose tests, which are not easily expressed in YAML, are also presented
in this file. Those tests will need to be manually implemented by each driver.

Common Test Format
==================

Each YAML file has the following keys:

- ``version``: A version number indicating the expected format of the spec tests (current version = 1)
- ``style``: A string indicating what style of tests this file contains. Currently ``unit`` is the only valid value
- ``description``: A text description of what the test is meant to assert

Unit Test Format:
=================

All Unit Tests have some of the following fields:

- ``poolOptions``: if present, connection pool options to use when creating a pool
- ``operations``: A list of operations to perform. All operations support the following fields:

  - ``name``: A string describing which operation to issue.
  - ``thread``: The name of the thread in which to run this operation. If not specified, runs in the default thread

- ``error``: Indicates that the main thread is expected to error during this test. An error may include of the following fields:

  - ``type``: the type of error emitted
  - ``message``: the message associated with that error
  - ``address``: Address of pool emitting error

- ``events``: An array of all connection monitoring events expected to occur while running ``operations``. An event may contain any of the following fields

  - ``type``: The type of event emitted
  - ``address``: The address of the pool emitting the event
  - ``connectionId``: The id of a connection associated with the event
  - ``options``: Options used to create the pool
  - ``reason``: A reason giving mroe information on why the event was emitted

- ``ignore``: An array of event names to ignore

Valid Unit Test Operations are the following:

- ``start(target)``: Starts a new thread named ``target``

  - ``target``: The name of the new thread to start

- ``wait(ms)``: Sleep the current thread for ``ms`` milliseconds

  - ``ms``: The number of milliseconds to sleep the current thread for

- ``waitForThread(target)``: wait for thread ``target`` to finish executing. Propagate any errors to the main thread.

  - ``target``: The name of the thread to wait for.

- ``waitForEvent(event, count)``: block the current thread until ``event`` has occurred ``count`` times

  - ``event``: The name of the event
  - ``count``: The number of times the event must occur (counting from the start of the test)

- ``label = pool.checkOut()``: call ``checkOut`` on pool, returning the checked out connection

  - ``label``: If specified, associate this label with the returned connection, so that it may be referenced in later operations

- ``pool.checkIn(connection)``: call ``checkIn`` on pool

  - ``connection``: A string label identifying which connection to check in. Should be a label that was previously set with ``checkOut``

- ``pool.clear()``: call ``clear`` on Pool
- ``pool.close()``: call ``close`` on Pool

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

Unit Test Runner:
=================

For the unit tests, the behavior of a Connection is irrelevant beyond the need to asserting ``connection.id``. Drivers MAY use a mock connection class for testing the pool behavior in unit tests

For each YAML file with ``style: unit``:

- Create a Pool ``pool``, subscribe and capture any Connection Monitoring events emitted in order.

  - If ``poolOptions`` is specified, use those options to initialize both pools
  - The returned pool must have an ``address`` set as a string value.

- Execute each ``operation`` in ``operations``

  - If a ``thread`` is specified, execute in that corresponding thread. Otherwise, execute in the main thread.

- Wait for the main thread to finish executing all of its operations
- If ``error`` is presented

  - Assert that an actual error ``actualError`` was thrown by the main thread
  - Assert that ``actualError`` MATCHES ``error``

- Else: 

  - Assert that no errors were thrown by the main thread

- calculate ``actualEvents`` as every Connection Event emitted whose ``type`` is not in ``ignore``
- if ``events`` is not empty, then for every ``idx``/``expectedEvent`` in ``events``

  - Assert that ``actualEvents[idx]`` exists
  - Assert that ``actualEvents[idx]`` MATCHES ``expectedEvent``


It is important to note that the ``ignore`` list is used for calculating ``actualEvents``, but is NOT used for the ``waitForEvent`` command

Prose Tests
===========

The following tests have not yet been automated, but MUST still be tested

#. All ConnectionPoolOptions MUST be specified at the MongoClient level
#. All ConnectionPoolOptions MUST be the same for all pools created by a MongoClient
#. A user MUST be able to specify all ConnectionPoolOptions via a URI string
#. A user MUST be able to subscribe to Connection Monitoring Events in a manner idiomatic to their language and driver
#. When a check out attempt fails because connection set up throws an error,
   assert that a ConnectionCheckOutFailedEvent with reason="connectionError" is emitted.
