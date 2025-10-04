Note: This most up-to-date version of this file lives at https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/tests/README.rst.

=====================================
Server Discovery And Monitoring Tests
=====================================

The YAML and JSON files in this directory tree are platform-independent tests
that drivers can use to prove their conformance to the
Server Discovery And Monitoring Spec.

Version
-------

Files in the "specifications" repository have no version scheme. They are not
tied to a MongoDB server version.

Format
------

Each YAML file has the following keys:

- description: A textual description of the test.
- uri: A connection string.
- phases: An array of "phase" objects.
  A phase of the test optionally sends inputs to the client,
  then tests the client's resulting TopologyDescription.

Each phase object has two keys:

- responses: (optional) An array of "response" objects. If not provided,
  the test runner should construct the client and perform assertions specified
  in the outcome object without processing any responses.
- outcome: An "outcome" object representing the TopologyDescription.

A response is a pair of values:

- The source, for example "a:27017".
  This is the address the client sent the "hello" command to.
- A "hello" response, for example `{ok: 1, isWritablePrimary: true}`.
  If the response includes an electionId it is shown in extended JSON like
  `{"$oid": "000000000000000000000002"}`.
  The empty response `{}` indicates a network error
  when attempting to call "hello".

In non-monitoring tests, an "outcome" represents the correct
TopologyDescription that results from processing the responses in the phases
so far. It has the following keys:

- topologyType: A string like "ReplicaSetNoPrimary".
- setName: A string with the expected replica set name, or null.
- servers: An object whose keys are addresses like "a:27017", and whose values
  are "server" objects.
- logicalSessionTimeoutMinutes: null or an integer.
- maxSetVersion: absent or an integer.
- maxElectionId: absent or a BSON ObjectId.
- compatible: absent or a bool.

A "server" object represents a correct ServerDescription within the client's
current TopologyDescription. It has the following keys:

- type: A ServerType name, like "RSSecondary".
- setName: A string with the expected replica set name, or null.
- setVersion: absent or an integer.
- electionId: absent, null, or an ObjectId.
- logicalSessionTimeoutMinutes: absent, null, or an integer.
- minWireVersion: absent or an integer.
- maxWireVersion: absent or an integer.

In monitoring tests, an "outcome" contains a list of SDAM events that should
have been published by the client as a result of processing "hello" responses
in the current phase. Any SDAM events published by the client during its
construction (that is, prior to processing any of the responses) should be
combined with the events published during processing of "hello" responses
of the first phase of the test. A test MAY explicitly verify events published
during client construction by providing an empty responses array for the
first phase.


Use as unittests
----------------

Mocking
~~~~~~~

Drivers should be able to test their server discovery and monitoring logic
without any network I/O, by parsing "hello" responses from the test file
and passing them into the driver code. Parts of the client and monitoring
code may need to be mocked or subclassed to achieve this. `A reference
implementation for PyMongo 3.x is available here
<https://github.com/mongodb/mongo-python-driver/blob/26d25cd74effc1e7a8d52224eac6c9a95769b371/test/test_discovery_and_monitoring.py>`.

Initialization
~~~~~~~~~~~~~~

For each file, create a fresh client object initialized with the file's "uri".

All files in the "single" directory include a connection string with one host
and no "replicaSet" option.
Set the client's initial TopologyType to Single, however that is achieved using the client's API.
(The spec says "The user MUST be able to set the initial TopologyType to Single"
without specifying how.)

All files in the "sharded" directory include a connection string with multiple hosts
and no "replicaSet" option.
Set the client's initial TopologyType to Unknown or Sharded, depending on the client's API.

All files in the "rs" directory include a connection string with a "replicaSet" option.
Set the client's initial TopologyType to ReplicaSetNoPrimary.
(For most clients, parsing a connection string with a "replicaSet" option
automatically sets the TopologyType to ReplicaSetNoPrimary.)

Set up a listener to collect SDAM events published by the client, including
events published during client construction.

Test Phases
~~~~~~~~~~~

For each phase in the file, parse the "responses" array.
Pass in the responses in order to the driver code.
If a response is the empty object `{}`, simulate a network error.

For non-monitoring tests,
once all responses are processed, assert that the phase's "outcome" object
is equivalent to the driver's current TopologyDescription.

For monitoring tests, once all responses are processed, assert that the
events collected so far by the SDAM event listener are equivalent to the
events specified in the phase.

Some fields such as "logicalSessionTimeoutMinutes" or "compatible" were added
later and haven't been added to all test files. If these fields are present,
test that they are equivalent to the fields of the driver's current
TopologyDescription.

For monitoring tests, clear the list of events collected so far.

Continue until all phases have been executed.
