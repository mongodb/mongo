
  subunit: A streaming protocol for test results
  Copyright (C) 2005-2013 Robert Collins <robertc@robertcollins.net>

  Licensed under either the Apache License, Version 2.0 or the BSD 3-clause
  license at the users choice. A copy of both licenses are available in the
  project source as Apache-2.0 and BSD. You may not use this file except in
  compliance with one of these two licences.

  Unless required by applicable law or agreed to in writing, software
  distributed under these licenses is distributed on an "AS IS" BASIS, WITHOUT
  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
  license you chose for the specific language governing permissions and
  limitations under that license.

  See the COPYING file for full details on the licensing of Subunit.

Subunit
-------

Subunit is a streaming protocol for test results.

There are two major revisions of the protocol. Version 1 was trivially human
readable but had significant defects as far as highly parallel testing was
concerned - it had no room for doing discovery and execution in parallel,
required substantial buffering when multiplexing and was fragile - a corrupt
byte could cause an entire stream to be misparsed. Version 1.1 added
encapsulation of binary streams which mitigated some of the issues but the
core remained.

Version 2 shares many of the good characteristics of Version 1 - it can be
embedded into a regular text stream (e.g. from a build system) and it still
models xUnit style test execution. It also fixes many of the issues with
Version 1 - Version 2 can be multiplexed without excessive buffering (in
time or space), it has a well defined recovery mechanism for dealing with
corrupted streams (e.g. where two processes write to the same stream
concurrently, or where the stream generator suffers a bug).

More details on both protocol version s can be found in the 'Protocol' section
of this document.

Subunit comes with command line filters to process a subunit stream and
language bindings for python, C, C++ and shell. Bindings are easy to write
for other languages.

A number of useful things can be done easily with subunit:
 * Test aggregation: Tests run separately can be combined and then
   reported/displayed together. For instance, tests from different languages
   can be shown as a seamless whole, and tests running on multiple machines
   can be aggregated into a single stream through a multiplexer.
 * Test archiving: A test run may be recorded and replayed later.
 * Test isolation: Tests that may crash or otherwise interact badly with each
   other can be run seperately and then aggregated, rather than interfering
   with each other or requiring an adhoc test->runner reporting protocol.
 * Grid testing: subunit can act as the necessary serialisation and
   deserialiation to get test runs on distributed machines to be reported in
   real time.

Subunit supplies the following filters:
 * tap2subunit - convert perl's TestAnythingProtocol to subunit.
 * subunit2csv - convert a subunit stream to csv.
 * subunit2disk - export a subunit stream to files on disk.
 * subunit2pyunit - convert a subunit stream to pyunit test results.
 * subunit2gtk - show a subunit stream in GTK.
 * subunit2junitxml - convert a subunit stream to JUnit's XML format.
 * subunit-diff - compare two subunit streams.
 * subunit-filter - filter out tests from a subunit stream.
 * subunit-ls - list info about tests present in a subunit stream.
 * subunit-stats - generate a summary of a subunit stream.
 * subunit-tags - add or remove tags from a stream.

Integration with other tools
----------------------------

Subunit's language bindings act as integration with various test runners like
'check', 'cppunit', Python's 'unittest'. Beyond that a small amount of glue
(typically a few lines) will allow Subunit to be used in more sophisticated
ways.

Python
======

Subunit has excellent Python support: most of the filters and tools are written
in python and there are facilities for using Subunit to increase test isolation
seamlessly within a test suite.

The most common way is to run an existing python test suite and have it output
subunit via the ``subunit.run`` module::

  $ python -m subunit.run mypackage.tests.test_suite

For more information on the Python support Subunit offers , please see
``pydoc subunit``, or the source in ``python/subunit/``

C
=

Subunit has C bindings to emit the protocol. The 'check' C unit testing project
has included subunit support in their project for some years now. See
'c/README' for more details.

C++
===

The C library is includable and usable directly from C++. A TestListener for
CPPUnit is included in the Subunit distribution. See 'c++/README' for details.

shell
=====

There are two sets of shell tools. There are filters, which accept a subunit
stream on stdin and output processed data (or a transformed stream) on stdout.

Then there are unittest facilities similar to those for C : shell bindings
consisting of simple functions to output protocol elements, and a patch for
adding subunit output to the 'ShUnit' shell test runner. See 'shell/README' for
details.

Filter recipes
--------------

To ignore some failing tests whose root cause is already known::

  subunit-filter --without 'AttributeError.*flavor'


The xUnit test model
--------------------

Subunit implements a slightly modified xUnit test model. The stock standard
model is that there are tests, which have an id(), can be run, and when run
start, emit an outcome (like success or failure) and then finish.

Subunit extends this with the idea of test enumeration (find out about tests
a runner has without running them), tags (allow users to describe tests in
ways the test framework doesn't apply any semantic value to), file attachments
(allow arbitrary data to make analysing a failure easy) and timestamps.

The protocol
------------

Version 2, or v2 is new and still under development, but is intended to
supercede version 1 in the very near future. Subunit's bundled tools accept
only version 2 and only emit version 2, but the new filters subunit-1to2 and
subunit-2to1 can be used to interoperate with older third party libraries.

Version 2
=========

Version 2 is a binary protocol consisting of independent packets that can be
embedded in the output from tools like make - as long as each packet has no
other bytes mixed in with it (which 'make -j N>1' has a tendency of doing).
Version 2 is currently in draft form, and early adopters should be willing
to either discard stored results (if protocol changes are made), or bulk
convert them back to v1 and then to a newer edition of v2.

The protocol synchronises at the start of the stream, after a packet, or
after any 0x0A byte. That is, a subunit v2 packet starts after a newline or
directly after the end of the prior packet.

Subunit is intended to be transported over a reliable streaming protocol such
as TCP. As such it does not concern itself with out of order delivery of
packets. However, because of the possibility of corruption due to either
bugs in the sender, or due to mixed up data from concurrent writes to the same
fd when being embedded, subunit strives to recover reasonably gracefully from
damaged data.

A key design goal for Subunit version 2 is to allow processing and multiplexing
without forcing buffering for semantic correctness, as buffering tends to hide
hung or otherwise misbehaving tests. That said, limited time based buffering
for network efficiency is a good idea - this is ultimately implementator
choice. Line buffering is also discouraged for subunit streams, as dropping
into a debugger or other tool may require interactive traffic even if line
buffering would not otherwise be a problem.

In version two there are two conceptual events - a test status event and a file
attachment event. Events may have timestamps, and the path of multiplexers that
an event is routed through is recorded to permit sending actions back to the
source (such as new tests to run or stdin for driving debuggers and other
interactive input). Test status events are used to enumerate tests, to report
tests and test helpers as they run. Tests may have tags, used to allow
tunnelling extra meanings through subunit without requiring parsing of
arbitrary file attachments. Things that are not standalone tests get marked
as such by setting the 'Runnable' flag to false. (For instance, individual
assertions in TAP are not runnable tests, only the top level TAP test script
is runnable).

File attachments are used to provide rich detail about the nature of a failure.
File attachments can also be used to encapsulate stdout and stderr both during
and outside tests.

Most numbers are stored in network byte order - Most Significant Byte first
encoded using a variation of http://www.dlugosz.com/ZIP2/VLI.html. The first
byte's top 2 high order bits encode the total number of octets in the number.
This encoding can encode values from 0 to 2**30-1, enough to encode a
nanosecond. Numbers that are not variable length encoded are still stored in
MSB order.

+--------+--------+---------+------------+
| prefix | octets | max     | max        |
+========+========+=========+============+
| 00     |      1 |  2**6-1 |         63 |
+--------+--------+---------+------------+
| 01     |      2 | 2**14-1 |      16383 |
+--------+--------+---------+------------+
| 10     |      3 | 2**22-1 |    4194303 |
+--------+--------+---------+------------+
| 11     |      4 | 2**30-1 | 1073741823 |
+--------+--------+---------+------------+

All variable length elements of the packet are stored with a length prefix
number allowing them to be skipped over for consumers that don't need to
interpret them.

UTF-8 strings are with no terminating NUL and should not have any embedded NULs
(implementations SHOULD validate any such strings that they process and take
some remedial action (such as discarding the packet as corrupt).

In short the structure of a packet is:

  PACKET := SIGNATURE FLAGS PACKET_LENGTH TIMESTAMP? TESTID? TAGS? MIME?
            FILECONTENT? ROUTING_CODE? CRC32

In more detail...

Packets are identified by a single byte signature - 0xB3, which is never legal
in a UTF-8 stream as the first byte of a character. 0xB3 starts with the first
bit set and the second not, which is the UTF-8 signature for a continuation
byte. 0xB3 was chosen as 0x73 ('s' in ASCII') with the top two bits replaced by
the 1 and 0 for a continuation byte.

If subunit packets are being embedded in a non-UTF-8 text stream, where 0x73 is
a legal character, consider either recoding the text to UTF-8, or using
subunit's 'file' packets to embed the text stream in subunit, rather than the
other way around.

Following the signature byte comes a 16-bit flags field, which includes a
4-bit version field - if the version is not 0x2 then the packet cannot be
read. It is recommended to signal an error at this point (e.g. by emitting
a synthetic error packet and returning to the top level loop to look for
new packets, or exiting with an error). If recovery is desired, treat the
packet signature as an opaque byte and scan for a new synchronisation point.
NB: Subunit V1 and V2 packets may legitimately included 0xB3 internally,
as they are an 8-bit safe container format, so recovery from this situation
may involve an arbitrary number of false positives until an actual packet
is encountered : and even then it may still be false, failing after passing
the version check due to coincidence.

Flags are stored in network byte order too.

+------------+------------+------------------------+
| High byte               | Low byte               |
+------------+------------+------------------------+
| 15 14 13 12 11 10  9  8 | 7  6  5  4  3  2  1  0 |
+------------+------------+------------------------+
| VERSION    |      feature bits                   |
+------------+-------------------------------------+

Valid version values are:
0x2 - version 2

Feature bits:

+---------+-------------+---------------------------+
| Bit 11  | mask 0x0800 | Test id present.          |
+---------+-------------+---------------------------+
| Bit 10  | mask 0x0400 | Routing code present.     |
+---------+-------------+---------------------------+
| Bit  9  | mask 0x0200 | Timestamp present.        |
+---------+-------------+---------------------------+
| Bit  8  | mask 0x0100 | Test is 'runnable'.       |
+---------+-------------+---------------------------+
| Bit  7  | mask 0x0080 | Tags are present.         |
+---------+-------------+---------------------------+
| Bit  6  | mask 0x0040 | File content is present.  |
+---------+-------------+---------------------------+
| Bit  5  | mask 0x0020 | File MIME type is present.|
+---------+-------------+---------------------------+
| Bit  4  | mask 0x0010 | EOF marker.               |
+---------+-------------+---------------------------+
| Bit  3  | mask 0x0008 | Must be zero in version 2.|
+---------+-------------+---------------------------+

Test status gets three bits:
Bit 2 | Bit 1 | Bit 0 - mask 0x0007 - A test status enum lookup:

* 000 - undefined / no test
* 001 - Enumeration / existence
* 002 - In progress
* 003 - Success
* 004 - Unexpected Success
* 005 - Skipped
* 006 - Failed
* 007 - Expected failure

After the flags field is a number field giving the length in bytes for the
entire packet including the signature and the checksum. This length must
be less than 4MiB - 4194303 bytes. The encoding can obviously record a larger
number but one of the goals is to avoid requiring large buffers, or causing
large latency in the packet forward/processing pipeline. Larger file
attachments can be communicated in multiple packets, and the overhead in such a
4MiB packet is approximately 0.2%.

The rest of the packet is a series of optional features as specified by the set
feature bits in the flags field. When absent they are entirely absent.

Forwarding and multiplexing of packets can be done without interpreting the
remainder of the packet until the routing code and checksum (which are both at
the end of the packet). Additionally, routers can often avoid copying or moving
the bulk of the packet, as long as the routing code size increase doesn't force
the length encoding to take up a new byte (which will only happen to packets
less than or equal to 16KiB in length) - large packets are very efficient to
route.

Timestamp when present is a 32 bit unsigned integer for seconds, and a variable
length number for nanoseconds, representing UTC time since Unix Epoch in
seconds and nanoseconds.

Test id when present is a UTF-8 string. The test id should uniquely identify
runnable tests such that they can be selected individually. For tests and other
actions which cannot be individually run (such as test
fixtures/layers/subtests) uniqueness is not required (though being human
meaningful is highly recommended).

Tags when present is a length prefixed vector of UTF-8 strings, one per tag.
There are no restrictions on tag content (other than the restrictions on UTF-8
strings in subunit in general). Tags have no ordering.

When a MIME type is present, it defines the MIME type for the file across all
packets same file (routing code + testid + name uniquely identifies a file,
reset when EOF is flagged). If a file never has a MIME type set, it should be
treated as application/octet-stream.

File content when present is a UTF-8 string for the name followed by the length
in bytes of the content, and then the content octets.

If present routing code is a UTF-8 string. The routing code is used to
determine which test backend a test was running on when doing data analysis,
and to route stdin to the test process if interaction is required.

Multiplexers SHOULD add a routing code if none is present, and prefix any
existing routing code with a routing code ('/' separated) if one is already
present. For example, a multiplexer might label each stream it is multiplexing
with a simple ordinal ('0', '1' etc), and given an incoming packet with route
code '3' from stream '0' would adjust the route code when forwarding the packet
to be '0/3'.

Following the end of the packet is a CRC-32 checksum of the contents of the
packet including the signature.

Example packets
~~~~~~~~~~~~~~~

Trivial test "foo" enumeration packet, with test id, runnable set,
status=enumeration. Spaces below are to visually break up signature / flags /
length / testid / crc32

b3 2901 0c 03666f6f 08555f1b


Version 1 (and 1.1)
===================

Version 1 (and 1.1) are mostly human readable protocols.

Sample subunit wire contents
----------------------------

The following::

  test: test foo works
  success: test foo works
  test: tar a file.
  failure: tar a file. [
  ..
   ]..  space is eaten.
  foo.c:34 WARNING foo is not defined.
  ]
  a writeln to stdout

When run through subunit2pyunit::

  .F
  a writeln to stdout

  ========================
  FAILURE: tar a file.
  -------------------
  ..
  ]..  space is eaten.
  foo.c:34 WARNING foo is not defined.


Subunit v1 protocol description
===============================

This description is being ported to an EBNF style. Currently its only partly in
that style, but should be fairly clear all the same. When in doubt, refer the
source (and ideally help fix up the description!). Generally the protocol is
line orientated and consists of either directives and their parameters, or
when outside a DETAILS region unexpected lines which are not interpreted by
the parser - they should be forwarded unaltered::

    test|testing|test:|testing: test LABEL
    success|success:|successful|successful: test LABEL
    success|success:|successful|successful: test LABEL DETAILS
    failure: test LABEL
    failure: test LABEL DETAILS
    error: test LABEL
    error: test LABEL DETAILS
    skip[:] test LABEL
    skip[:] test LABEL DETAILS
    xfail[:] test LABEL
    xfail[:] test LABEL DETAILS
    uxsuccess[:] test LABEL
    uxsuccess[:] test LABEL DETAILS
    progress: [+|-]X
    progress: push
    progress: pop
    tags: [-]TAG ...
    time: YYYY-MM-DD HH:MM:SSZ

    LABEL: UTF8*
    NAME: UTF8*
    DETAILS ::= BRACKETED | MULTIPART
    BRACKETED ::= '[' CR UTF8-lines ']' CR
    MULTIPART ::= '[ multipart' CR PART* ']' CR
    PART ::= PART_TYPE CR NAME CR PART_BYTES CR
    PART_TYPE ::= Content-Type: type/sub-type(;parameter=value,parameter=value)
    PART_BYTES ::= (DIGITS CR LF BYTE{DIGITS})* '0' CR LF

unexpected output on stdout -> stdout.
exit w/0 or last test completing -> error

Tags given outside a test are applied to all following tests
Tags given after a test: line and before the result line for the same test
apply only to that test, and inherit the current global tags.
A '-' before a tag is used to remove tags - e.g. to prevent a global tag
applying to a single test, or to cancel a global tag.

The progress directive is used to provide progress information about a stream
so that stream consumer can provide completion estimates, progress bars and so
on. Stream generators that know how many tests will be present in the stream
should output "progress: COUNT". Stream filters that add tests should output
"progress: +COUNT", and those that remove tests should output
"progress: -COUNT". An absolute count should reset the progress indicators in
use - it indicates that two separate streams from different generators have
been trivially concatenated together, and there is no knowledge of how many
more complete streams are incoming. Smart concatenation could scan each stream
for their count and sum them, or alternatively translate absolute counts into
relative counts inline. It is recommended that outputters avoid absolute counts
unless necessary. The push and pop directives are used to provide local regions
for progress reporting. This fits with hierarchically operating test
environments - such as those that organise tests into suites - the top-most
runner can report on the number of suites, and each suite surround its output
with a (push, pop) pair. Interpreters should interpret a pop as also advancing
the progress of the restored level by one step. Encountering progress
directives between the start and end of a test pair indicates that a previous
test was interrupted and did not cleanly terminate: it should be implicitly
closed with an error (the same as when a stream ends with no closing test
directive for the most recently started test).

The time directive acts as a clock event - it sets the time for all future
events. The value should be a valid ISO8601 time.

The skip, xfail and uxsuccess outcomes are not supported by all testing
environments. In Python the testttools (https://launchpad.net/testtools)
library is used to translate these automatically if an older Python version
that does not support them is in use. See the testtools documentation for the
translation policy.

skip is used to indicate a test was discovered but not executed. xfail is used
to indicate a test that errored in some expected fashion (also know as "TODO"
tests in some frameworks). uxsuccess is used to indicate and unexpected success
where a test though to be failing actually passes. It is complementary to
xfail.

Hacking on subunit
------------------

Releases
========

* Update versions in configure.ac and python/subunit/__init__.py.
* Update NEWS.
* Do a make distcheck, which will update Makefile etc.
* Do a PyPI release: PYTHONPATH=python python setup.py sdist bdist_wheel; twine upload -s dist/*
* Upload the regular one to LP.
* Push a tagged commit.
  git push -t origin master:master
