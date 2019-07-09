==========
Auth Tests
==========

The YAML and JSON files in this directory tree are platform-independent tests
that drivers can use to prove their conformance to the Auth Spec at least with
respect to connection string URI input.

Drivers should do additional unit testing if there are alternate ways of
configuring credentials on a client.

Driver must also conduct the prose tests in the Auth Spec test plan section.

Format
------

Each YAML file contains an object with a single ``tests`` key. This key is an
array of test case objects, each of which have the following keys:

- ``description``: A string describing the test.
- ``uri``: A string containing the URI to be parsed.
- ``valid:`` A boolean indicating if the URI should be considered valid.
- ``credential``: If null, the credential must not be considered configured for the
  the purpose of deciding if the driver should authenticate to the topology.  If non-null,
  it is an object containing one or more of the following properties of a credential:

  - ``username``: A string containing the username. For auth mechanisms
    that do not utilize a password, this may be the entire ``userinfo`` token
    from the connection string.
  - ``password``: A string containing the password.
  - ``source``: A string containing the authentication database.
  - ``mechanism``: A string containing the authentication mechanism.  A null value for
    this key is used to indicate that a mechanism wasn't specified and that mechanism
    negotiation is required.  Test harnesses should modify the mechanism test as needed
    to assert this condition.
  - ``mechanism_properties``: A document containing mechanism-specific properties.  It
    specifies a subset of properties that must match.  If a key exists in the test data,
    it must exist with the corresponding value in the credential.  Other values may
    exist in the credential without failing the test.

If any key is missing, no assertion about that key is necessary.  Except as
specified explicitly above, if a key is present, but the test value is null,
the observed value for that key must be uninitialized (whatever that means for
a given driver and data type).

Implementation notes
====================

Testing whether a URI is valid or not should simply be a matter of checking
whether URI parsing (or MongoClient construction) raises an error or exception.

If a credential is configured, its properties must be compared to the
``credential`` field.
