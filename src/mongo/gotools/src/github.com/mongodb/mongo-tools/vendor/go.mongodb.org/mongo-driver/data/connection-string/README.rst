=======================
Connection String Tests
=======================

The YAML and JSON files in this directory tree are platform-independent tests
that drivers can use to prove their conformance to the Connection String Spec.

As the spec is primarily concerned with parsing the parts of a URI, these tests
do not focus on host and option validation. Where necessary, the tests use
options known to be (un)supported by drivers to assert behavior such as issuing
a warning on repeated option keys.  As such these YAML tests are in no way a
replacement for more thorough testing. However, they can provide an initial
verification of your implementation.

Version
-------

Files in the "specifications" repository have no version scheme. They are not
tied to a MongoDB server version.

Format
------

Each YAML file contains an object with a single ``tests`` key. This key is an
array of test case objects, each of which have the following keys:

- ``description``: A string describing the test.
- ``uri``: A string containing the URI to be parsed.
- ``valid:`` A boolean indicating if the URI should be considered valid.
- ``warning:`` A boolean indicating whether URI parsing should emit a warning
  (independent of whether or not the URI is valid).
- ``hosts``: An array of host objects, each of which have the following keys:

  - ``type``: A string denoting the type of host. Possible values are "ipv4",
    "ip_literal", "hostname", and "unix". Asserting the type is *optional*.
  - ``host``: A string containing the parsed host.
  - ``port``: An integer containing the parsed port number.
- ``auth``: An object containing the following keys:

  - ``username``: A string containing the parsed username. For auth mechanisms
    that do not utilize a password, this may be the entire ``userinfo`` token
    (as discussed in `RFC 2396 <https://www.ietf.org/rfc/rfc2396.txt>`_).
  - ``password``: A string containing the parsed password.
  - ``db``: A string containing the parsed authentication database. For legacy
    implementations that support namespaces (databases and collections) this may
    be the full namespace eg: ``<db>.<coll>``
- ``options``: An object containing key/value pairs for each parsed query string
  option.

If a test case includes a null value for one of these keys (e.g. ``auth: ~``,
``port: ~``), no assertion is necessary. This both simplifies parsing of the
test files (keys should always exist) and allows flexibility for drivers that
might substitute default values *during* parsing (e.g. omitted ``port`` could be
parsed as 27017).

The ``valid`` and ``warning`` fields are boolean in order to keep the tests
flexible. We are not concerned with asserting the format of specific error or
warnings messages strings.

Use as unit tests
=================

Testing whether a URI is valid or not should simply be a matter of checking
whether URI parsing (or MongoClient construction) raises an error or exception.
Testing for emitted warnings may require more legwork (e.g. configuring a log
handler and watching for output).

Not all drivers may be able to directly assert the hosts, auth credentials, and
options. Doing so may require exposing the driver's URI parsing component.

The file valid-db-with-dotted-name.yml is a special case for testing drivers
that allow dotted namespaces, instead of only database names, in the Auth
Database portion of the URI.
