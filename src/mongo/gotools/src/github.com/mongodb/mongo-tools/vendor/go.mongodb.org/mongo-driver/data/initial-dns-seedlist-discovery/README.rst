====================================
Initial DNS Seedlist Discovery tests
====================================

This directory contains platform-independent tests that drivers can use
to prove their conformance to the Initial DNS Seedlist Discovery spec.

Test Setup
----------

Start a three-node replica set on localhost, on ports 27017, 27018, and 27019,
with replica set name "repl0". The replica set MUST be started with SSL
enabled.

To run the tests that accompany this spec, you need to configure the SRV and
TXT records with a real name server. The following records are required for
these tests::

  Record                                    TTL    Class   Address
  localhost.test.build.10gen.cc.            86400  IN A    127.0.0.1
  localhost.sub.test.build.10gen.cc.        86400  IN A    127.0.0.1

  Record                                    TTL    Class   Port   Target
  _mongodb._tcp.test1.test.build.10gen.cc.  86400  IN SRV  27017  localhost.test.build.10gen.cc.
  _mongodb._tcp.test1.test.build.10gen.cc.  86400  IN SRV  27018  localhost.test.build.10gen.cc.
  _mongodb._tcp.test2.test.build.10gen.cc.  86400  IN SRV  27018  localhost.test.build.10gen.cc.
  _mongodb._tcp.test2.test.build.10gen.cc.  86400  IN SRV  27019  localhost.test.build.10gen.cc.
  _mongodb._tcp.test3.test.build.10gen.cc.  86400  IN SRV  27017  localhost.test.build.10gen.cc.
  _mongodb._tcp.test5.test.build.10gen.cc.  86400  IN SRV  27017  localhost.test.build.10gen.cc.
  _mongodb._tcp.test6.test.build.10gen.cc.  86400  IN SRV  27017  localhost.test.build.10gen.cc.
  _mongodb._tcp.test7.test.build.10gen.cc.  86400  IN SRV  27017  localhost.test.build.10gen.cc.
  _mongodb._tcp.test8.test.build.10gen.cc.  86400  IN SRV  27017  localhost.test.build.10gen.cc.
  _mongodb._tcp.test10.test.build.10gen.cc. 86400  IN SRV  27017  localhost.test.build.10gen.cc.
  _mongodb._tcp.test11.test.build.10gen.cc. 86400  IN SRV  27017  localhost.test.build.10gen.cc.
  _mongodb._tcp.test12.test.build.10gen.cc. 86400  IN SRV  27017  localhost.build.10gen.cc.
  _mongodb._tcp.test13.test.build.10gen.cc. 86400  IN SRV  27017  test.build.10gen.cc.
  _mongodb._tcp.test14.test.build.10gen.cc. 86400  IN SRV  27017  localhost.not-test.build.10gen.cc.
  _mongodb._tcp.test15.test.build.10gen.cc. 86400  IN SRV  27017  localhost.test.not-build.10gen.cc.
  _mongodb._tcp.test16.test.build.10gen.cc. 86400  IN SRV  27017  localhost.test.build.not-10gen.cc.
  _mongodb._tcp.test17.test.build.10gen.cc. 86400  IN SRV  27017  localhost.test.build.10gen.not-cc.
  _mongodb._tcp.test18.test.build.10gen.cc. 86400  IN SRV  27017  localhost.sub.test.build.10gen.cc.
  _mongodb._tcp.test19.test.build.10gen.cc. 86400  IN SRV  27017  localhost.evil.build.10gen.cc.
  _mongodb._tcp.test19.test.build.10gen.cc. 86400  IN SRV  27017  localhost.test.build.10gen.cc.

  Record                                    TTL    Class   Text
  test5.test.build.10gen.cc.                86400  IN TXT  "replicaSet=repl0&authSource=thisDB"
  test6.test.build.10gen.cc.                86400  IN TXT  "replicaSet=repl0"
  test6.test.build.10gen.cc.                86400  IN TXT  "authSource=otherDB"
  test7.test.build.10gen.cc.                86400  IN TXT  "ssl=false"
  test8.test.build.10gen.cc.                86400  IN TXT  "authSource"
  test10.test.build.10gen.cc.               86400  IN TXT  "socketTimeoutMS=500"
  test11.test.build.10gen.cc.               86400  IN TXT  "replicaS" "et=rep" "l0"

Note that ``test4`` is omitted deliberately to test what happens with no SRV
record. ``test9`` is missing because it was deleted during the development of
the tests. The missing ``test.`` sub-domain in the SRV record target for
``test12`` is deliberate.

In our tests we have used ``localhost.test.build.10gen.cc`` as the domain, and
then configured ``localhost.test.build.10gen.cc`` to resolve to 127.0.0.1.

You need to adapt the records shown above to replace ``test.build.10gen.cc``
with your own domain name, and update the "uri" field in the YAML or JSON files
in this directory with the actual domain.

Test Format and Use
-------------------

These YAML and JSON files contain the following fields:

- ``uri``: a mongodb+srv connection string
- ``seeds``: the expected set of initial seeds discovered from the SRV record
- ``hosts``: the discovered topology's list of hosts once SDAM completes a scan
- ``options``: the parsed connection string options as discovered from URI and
  TXT records
- ``error``: indicates that the parsing of the URI, or the resolving or
  contents of the SRV or TXT records included errors.
- ``comment``: a comment to indicate why a test would fail.

For each file, create MongoClient initialized with the mongodb+srv connection
string. You SHOULD verify that the client's initial seed list matches the list of
seeds. You MUST verify that the set of ServerDescriptions in the client's
TopologyDescription eventually matches the list of hosts. You MUST verify that
each of the values of the Connection String Options under ``options`` match the
Client's parsed value for that option. There may be other options parsed by
the Client as well, which a test does not verify. You MUST verify that an
error has been thrown if ``error`` is present.
