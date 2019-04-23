===================
Max Staleness Tests
===================

This directory contains platform-independent tests that drivers can use
to prove their conformance to the Max Staleness Spec. The tests
are provided in both YAML and JSON formats, and drivers may test against
whichever format is more convenient for them.

Test Format and Use
-------------------

YAML files contain the following setup for each test:

- ``heartbeatFrequencyMS``: optional int

- ``topology_description``: the state of a mocked cluster

  - ``type``: the TopologyType

  - ``servers``: a list of ServerDescriptions, each with:

    - ``address``: a "host:port"

    - ``type``: a ServerType

    - ``avg_rtt_ms``: average round trip time in milliseconds [1]_

    - ``lastWrite``: subdocument

      - ``lastWriteDate``: nonzero int64, milliseconds since some past time

    - ``maxWireVersion``: an int

    - ``lastUpdateTime``: milliseconds since the Unix epoch

- ``read_preference``: a read preference document

For each test, create a MongoClient.
Configure it with the heartbeatFrequencyMS specified by the test,
or accept the driver's default heartbeatFrequencyMS if the test omits this field.

(Single-threaded and multi-threaded clients now make heartbeatFrequencyMS configurable.
This is a change in Server Discovery and Monitoring to support maxStalenessSeconds.
Before, multi-threaded clients were allowed to make it configurable or not.)

For each test, create a new TopologyDescription object initialized with the
values from ``topology_description``. Initialize ServerDescriptions from the
provided data. Create a ReadPreference object initialized with the values
from ``read_preference``. Select servers that match the ReadPreference.

Each test specifies that it expects an error, or specifies two sets of servers:

- ``error: true``
- ``suitable_servers``: the set of servers in the TopologyDescription
  that are suitable for the ReadPreference, without taking ``avg_rtt_ms``
  into account.
- ``in_latency_window``: the set of suitable servers whose round trip time
  qualifies them according to the default latency threshold of 15ms.
  In each test there is one server in the latency window, to ensure
  tests pass or fail deterministically.

If the file contains ``error: true``, drivers MUST test that they throw an
error during server selection, due to an invalid read preference or
incompatible wire versions. For other files, drivers MUST test that they
correctly select the set of servers in ``in_latency_window``.

Drivers MAY also test that before filtration by latency, they select the
specified set of "suitable" servers.

.. [1] ``avg_rtt_ms`` is included merely for consistency with
   Server Selection tests. It is not significant in Max Staleness tests.
