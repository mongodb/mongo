======================
Server Selection Tests
======================

This directory contains platform-independent tests that drivers can use
to prove their conformance to the Server Selection spec. The tests
are provided in both YAML and JSON formats, and drivers may test against
whichever format is more convenient for them.

Version
-------

Specifications have no version scheme.
They are not tied to a MongoDB server version,
and it is our intention that each specification moves from "draft" to "final"
with no further versions; it is superseded by a future spec, not revised.

However, implementers must have stable sets of tests to target.
As test files evolve they will be occasionally tagged like
"server-selection-tests-2015-01-04", until the spec is final.

Test Format and Use
-------------------

There are two types of tests for the server selection spec, tests for
round trip time (RTT) calculation, and tests for server selection logic.

Drivers should be able to test their server selection logic
without any network I/O, by parsing topology descriptions and read preference
documents from the test files and passing them into driver code. Parts of the
server selection code may need to be mocked or subclassed to achieve this.

RTT Calculation Tests
>>>>>>>>>>>>>>>>>>>>>

These YAML files contain the following keys:

- ``avg_rtt_ms``: a server's previous average RTT, in milliseconds
- ``new_rtt_ms``: a new RTT value for this server, in milliseconds
- ``new_avg_rtt``: this server's newly-calculated average RTT, in milliseconds

For each file, create a server description object initialized with ``avg_rtt_ms``.
Parse ``new_rtt_ms``, and ensure that the new RTT value for the mocked server
description is equal to ``new_avg_rtt``.

If driver architecture doesn't easily allow construction of server description
objects in isolation, unit testing the EWMA algorithm using these inputs
and expected outputs is acceptable.

Server Selection Logic Tests
>>>>>>>>>>>>>>>>>>>>>>>>>>>>

These YAML files contain the following setup for each test:

- ``topology_description``: the state of a mocked cluster
- ``operation``: the kind of operation to perform, either read or write
- ``read_preference``: a read preference document

For each file, create a new TopologyDescription object initialized with the values
from ``topology_description``. Create a ReadPreference object initialized with the
values from ``read_preference``.

Together with "operation", pass the newly-created TopologyDescription and ReadPreference
to server selection, and ensure that it selects the correct subset of servers from
the TopologyDescription. Each YAML file contains a key for these stages of server selection:

- ``suitable_servers``: the set of servers in topology_description that are suitable, as
  per the Server Selection spec, given operation and read_preference
- ``in_latency_window``: the set of suitable_servers that fall within the latency window

Drivers implementing server selection MUST test that their implementation
correctly returns the set of servers in ``in_latency_window``. Drivers SHOULD also test
against ``suitable_servers`` if possible.
