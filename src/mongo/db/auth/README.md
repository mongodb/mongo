# Identity And Access Management

## Table of Contents

- [High Level Overview](#high-level-overview)
- [Authentication](#authentication)
  - [SASL](#sasl)
    - [Speculative Auth](#speculative-authentication)
    - [SASL Supported Mechs](#sasl-supported-mechs)
  - [X509 Authentication](#x509-authentication)
  - [Cluster Authentication](#cluster-authentication)
  - [Localhost Auth Bypass](#localhost-auth-bypass)
- [Authorization](#authorization)
  - [AuthName](#authname) (`UserName` and `RoleName`)
  - [Users](#users)
    - [User Roles](#user-roles)
    - [User Credentials](#user-credentials)
    - [User Authentication Restrictions](#user-authentication-restrictions)
  - [Roles](#roles)
    - [Role subordinate Roles](#role-subordinate-roles)
    - [Role Privileges](#role-privileges)
    - [Role Authentication Restrictions](#role-authentication-restrictions)
  - [User and Role Management](#user-and-role-management)
    - [UMC Transactions](#umc-transactions)
  - [Privilege](#privilege)
    - [ResourcePattern](#resourcepattern)
    - [ActionType](#actiontype)
  - [Command Execution](#command-execution)
  - [Authorization Caching](#authorization-caching)
  - [Authorization Manager External State](#authorization-manager-external-state)
  - [Types of Authorization](#types-of-authorization)
    - [Local Authorization](#local-authorization)
    - [LDAP Authorization](#ldap-authorization)
    - [X.509 Authorization](#x509-authorization)
  - [Cursors and Operations](#cursors-and-operations)
  - [Contracts](#contracts)
- [External References](#external-references)

## High Level Overview

Authentication and Authorization are important steps in connection establishment. After a client
with authentication credentials establishes a network session, it will begin the authentication
process, which begins with the client sending a cmdHello which specifies a username. If the client
supports speculative authentication, it will try to guess a mechanism which might be supported by
the user and send the first authentication message for that mechanism in the `CmdHello`. The server
responds with [`saslSupportedMechs`](#sasl-supported-mechs) available for the specified username,
and if possible, begins [`speculativeAuth`](#speculative-authentication) by attempting to perform
the `saslStart` step by using the authentication message, cutting down one network roundtrip. If
speculative auth was not possible, the server indicates so in the response to the `CmdHello` and the
client performs a
[`saslStart`](https://github.com/mongodb/mongo/blob/r4.7.0/src/mongo/db/auth/sasl_commands.cpp#L68)
command with a selected mechanism. If another step is required, the client sends a
[`saslContinue`](https://github.com/mongodb/mongo/blob/r4.7.0/src/mongo/db/auth/sasl_commands.cpp#L102)
command to the server. The saslContinue step iterates until the authentication is either accepted or
rejected. If the authentication attempt is successful, the server grants the authorization rights of
the user by calling functions from the [`AuthorizationManager`](#authorization). When a user becomes
authorized, the [`AuthorizationSession`](#authorization) for the `Client` becomes populated with the
user credentials and roles. The authorization session is then used to check permissions when the
`Client` issues commands against the database.

## Authentication

On a server with authentication enabled, all but a small handful of commands require clients to
authenticate before performing any action.  This typically occurs with a 1 to 3 round trip
conversation using the `saslStart` and `saslContinue` commands, or though a single call to the
`authenticate` command. See [SASL](#SASL) and [X.509](#X509) below for the details of these
exchanges.

### SASL

`SASL` (Simple Authentication and Security Layer) is a framework for authentication that allows
users to authenticate using different mechanisms without much code duplication. A SASL mechanism is
a series of challenge and responses that occur when authentication is being performed. There are a
specified list of authentication mechanisms that SASL supports
[here](https://www.iana.org/assignments/sasl-mechanisms/sasl-mechanisms.xhtml).
[`CyrusSASL`](https://www.cyrusimap.org/sasl/) provides a SASL framework for clients and servers and
an independent set of packages for different authentication mechanisms which are loaded dynamically
at runtime. `SASL` mechanisms define a method of communication between a client and a server. It
does not, however, define where the user credentials can be stored. With some `SASL` mechanisms,
`PLAIN` for example, the credentials can be stored in the database itself or in `LDAP`.

Before running authentication, the server initializes an
[`AuthenticationSession`](https://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authentication_session.h)
on the `Client`. This session persists information between authentications steps and is released
when authentication concludes, either successfully or unsuccessfully.

During the first step of authentication, the client invokes `{saslStart: ...}`, which reaches
[`doSaslStart`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/sasl_commands.cpp#L237-L242)
which gets the mechanism used and performs the actual authentication by calling the step function
(inherited from
[`ServerMechanismBase::step`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/sasl_mechanism_registry.h#L161-L172))
on the chosen mechanism (more on the mechanisms in the [SASL](#sasl) section). The server then sends
a reply to the client with information regarding the status of authentication. If both
authentication and authorization are complete, the client can begin executing commands against the
server. If authentication requires more information to complete, the server requests this
information. If authentication fails, then the client receives that information and potentially
closes the session.

If, after the first SASL step, there is more work to be done, the client sends a
[`CMDSaslContinue`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/sasl_commands.cpp#L98)
to the server with whatever extra information the server requested. The server then performs another
SASL step. The server then sends the client a similar reply as it did from the `SASLStart` command.
The `SASLContinue` phase repeats until the client is either authenticated or an error is
encountered.

#### Speculative Authentication

To reduce connection overhead time, clients may begin and possibly complete their authentication
exchange as part of the
[`CmdHello`]((https://github.com/mongodb/mongo/blob/r4.7.0/src/mongo/db/repl/replication_info.cpp#L234))
exchange.  In this mode, the body of the `saslStart` or `authenticate` command used for
authentication may be embedded into the `hello` command under the field `{speculativeAuthenticate:
$bodyOfAuthCmd}`.

On success, the reply typically emitted by such a command when invoked directly, will then be
returned by the server in the `{speculativeAuthenticate: ...}` field of the `hello` command's reply.
If the authentication sub-command fails, the error is swallowed by the server, and no reply is
included in the `hello` command response.

#### SASL Supported Mechs

When using the [SASL](#SASL) authentication workflow, it is necessary to select a specific mechanism
to authenticate with (e.g. SCRAM-SHA-1, SCRAM-SHA-256, PLAIN, GSSAPI, etc...).  If the user has not
included the mechanism in the mongodb:// URI, then the client can ask the server what mechanisms are
available on a per-user basis before attempting to authenticate.

Therefore, during the initial handshake using
[`CmdHello`](https://github.com/mongodb/mongo/blob/r4.7.0/src/mongo/db/repl/replication_info.cpp#L234),
the client will notify the server of the user it is about to authenticate by including
`{saslSupportedMechs: 'username'}` with the `hello` command.  The server will then include
`{saslSupportedMechs: [$listOfMechanisms]}` in the `hello` command's response.

This allows clients to proceed with authentication by choosing an appropriate mechanism. The
different named SASL mechanisms are listed below. If a mechanism can use a different storage method,
the storage mechanism is listed as a sub-bullet below.

- [**SCRAM-SHA-1**](https://tools.ietf.org/html/rfc5802)
  - See the section on `SCRAM-SHA-256` for details on `SCRAM`. `SCRAM-SHA-1` uses `SHA-1` for the
    hashing algorithm.
- [**SCRAM-SHA-256**](https://tools.ietf.org/html/rfc7677)
  - `SCRAM` stands for Salted Challenge Response Authentication Mechanism. `SCRAM-SHA-256` implements
    the `SCRAM` protocol and uses `SHA-256` as a hashing algorithm to complement it. `SCRAM`
    involves four steps, a client and server first, and a client and server final. During the client
    first, the client sends the username for lookup. The server uses the username to retrieve the
    relevant authentication information for the client. This generally includes the salt, StoredKey,
    ServerKey, and iteration count. The client then computes a set of values (defined in [section
    3](https://tools.ietf.org/html/rfc5802#section-3) of the `SCRAM` RFC), most notably the client
    proof and the server signature. It sends the client proof (used to authenticate the client) to
    the server, and the server then responds by sending the server proof. The hashing function used
    to hash the client password that is stored by the server is what differentiates `SCRAM-SHA-1` vs
    `SCRAM-SHA-256`, `SHA-1` is used in `SCRAM-SHA-1`. `SCRAM-SHA-256` is the preferred mechanism
    over `SCRAM-SHA-1`. Note also that `SCRAM-SHA-256` performs [RFC 4013 SASLprep Unicode
    normalization](https://tools.ietf.org/html/rfc4013) on all provided passwords before hashing,
    while for backward compatibility reasons, `SCRAM-SHA-1` does not.
- [**PLAIN**](https://tools.ietf.org/html/rfc4616)
  - The `PLAIN` mechanism involves two steps for authentication. First, the client concatenates a
    message using the authorization id, the authentication id (also the username), and the password
    for a user and sends it to the server. The server validates that the information is correct and
    authenticates the user. For storage, the server hashes one copy using SHA-1 and another using
    SHA-256 so that the password is not stored in plaintext. Even when using the PLAIN mechanism,
    the same secrets as used for the SCRAM methods are stored and used for validation. The chief
    difference between using PLAIN and SCRAM-SHA-256 (or SCRAM-SHA-1) is that using SCRAM provides
    mutual authentication and avoids transmitting the password to the server.  With PLAIN, it is
    less difficult for a MitM attacker to compromise original credentials.
  - **With local users**
    - When the PLAIN mechanism is used with internal users, the user information is stored in the
      [user
      collection](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authorization_manager.cpp#L56)
      on the database. See [authorization](#authorization) for more information.
  - **With Native LDAP**
    - When the PLAIN mechanism uses `Native LDAP`, the credential information is sent to and
      received from LDAP when creating and authorizing a user. The mongo server sends user
      credentials over the wire to the LDAP server and the LDAP server requests a password. The
      mongo server sends the password in plain text and LDAP responds with whether the password is
      correct. Here the communication with the driver and the mongod is the same, but the storage
      mechanism for the credential information is different.
  - **With Cyrus SASL / saslauthd**
    - When using saslauthd, the mongo server communicates with a process called saslauthd running on
      the same machine. The saslauthd process has ways of communicating with many other servers,
      LDAP servers included. Saslauthd works in the same way as Native LDAP except that the
      mongo process communicates using unix domain sockets.
- [**GSSAPI**](https://tools.ietf.org/html/rfc4752)
  - GSSAPI is an authentication mechanism that supports [Kerberos](https://web.mit.edu/kerberos/)
    authentication. GSSAPI is the communication method used to communicate with Kerberos servers and
    with clients. When initializing this auth mechanism, the server tries to acquire its credential
    information from the KDC by calling
    [`tryAcquireServerCredential`](https://github.com/10gen/mongo-enterprise-modules/blob/r4.4.0/src/sasl/mongo_gssapi.h#L36).
    If this is not approved, the server fasserts and the mechanism is not registered. On Windows,
    SChannel provides a `GSSAPI` library for the server to use. On other platforms, the Cyrus SASL
    library is used to make calls to the KDC (Kerberos key distribution center).

The specific properties that each SASL mechanism provides is outlined in this table below.

|               | Mutual Auth | No Plain Text |
|---------------|-------------|---------------|
| SCRAM         | X           | X             |
| PLAIN         |             |               |
| GSS-API       | X           | X             |

### <a name="x509atn"></a>X509 Authentication

`MONGODB-X509` is an authentication method that uses the x509 certificates from the SSL/TLS
certificate key exchange. When the peer certificate validation happens during the SSL handshake, an
[`SSLPeerInfo`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/util/net/ssl_types.h#L113-L143)
is created and attached to the transport layer SessionHandle. During `MONGODB-X509` auth, the server
grabs the client's username from the `SSLPeerInfo` struct and, if the client is a driver, verifies
that the client name matches the username provided by the command object. If the client is
performing intracluster authentication, see the details below in the authentication section and the
code comments
[here](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/authentication_commands.cpp#L74-L139).

### Cluster Authentication

There are a few different clients that can authenticate to a mongodb server. Three of the most
common clients are drivers (including the shell), mongods, and mongos'. When clients authenticate to
a server, they can use any of the authentication mechanisms described [below in the sasl
section](#sasl). When a mongod or a mongos needs to authenticate to a mongodb server, it does not
pass in distinguishing user credentials to authenticate (all servers authenticate to other servers
as the `__system` user), so most of the options described below will not necessarily work. However,
two options are available for authentication - keyfile auth and X509 auth. X509 auth is described in
more detail above, but a precondition to using it is having TLS enabled.

`keyfile` auth instructors servers to authenticate to each other using the `SCRAM-SHA-256` mechanism
as the `local.__system` user who's password can be found in the named key file. A keyfile is a file
stored on disk that servers load on startup, sending them when they behave as clients to another
server. The keyfile contains the shared password between the servers.
[`clusterAuthMode`](https://docs.mongodb.com/manual/reference/parameters/#param.clusterAuthMode) is
a server parameter that configures how servers authenticate to each other. There are four modes used
to allow a transition from keyfile, which provides minimal security, to x509 authentication, which
provides the most security.

#### Special Case: Arbiter

The only purpose of an arbiter is to participate in elections for replica set primary. An arbiter
does not have a copy of data set, including system tables which contain user and role definitions,
and therefore can not authenticate local users. It is possible to authenticate to arbiter using
external authentication methods such as cluster authentication or
[x.509 authentication](#x509atn) and acquire a role using [x.509 authorization](#x509azn).

It is also possible to connect to an arbiter with limited access using the
[localhost auth bypass](#lhabp). If the localhost auth bypass is disabled using the
[`enableLocalhostAuthBypass`](https://docs.mongodb.com/manual/reference/parameters/#param.enableLocalhostAuthBypass)
option, then all non cluster-auth connections will be denied access.

### Sharding Authentication

Sharded databases use the same authentication mechanism as previously described in "Cluster
Authentication".

Sharding query router (mongos) is an interface between client applications and the data bearing nodes
of a sharded cluster. It does not store any data, however it does maintain some in-memory caches. In
order to authenticate users, mongos contacts config servers to obtain a user's entire definition,
which it then deserializes to obtain roles, privileges, and credentials. It does this by invoking the
[`{usersInfo:...}` command](https://docs.mongodb.com/manual/reference/command/usersInfo/)
against a config server, see
[`getUserDescription`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authz_manager_external_state_s.cpp#L120)
function for details.

Data bearing members of a sharded cluster have no special provisions and do not normally have access
to any user or role definitions, making non-cluster authentication impossible under normal circumstances.
While it is possible to create local users and roles on a data bearing shard (making non-cluster
authentication possible), this should be avoided. All connecting clients should access members
via mongos only.

### <a name="lhabp">Localhost Auth Bypass

When first setting up database authentication (using the `--auth` command to start a server), there
is a feature called `localhostAuthBypass`. The `localhostAuthBypass` allows a client to connect over
localhost without any user credentials to create a user as long as there are no users or roles
registered in the database (no documents stored in the user collection). Once there is a user or
role registered, the `localhostAuthBypass` is disabled and a user has to authenticate to perform
most actions.

## Authorization

Auth**orization** describes the set of access controls conferred by the system on a connected
client. It naturally flows from Auth**entication**, as authenticated clients may be trusted with
additional access, thus upon completing [authentication](#Authentication), a client's authorization
tends to expand. Similarly, upon logout a client's authorization tends to shrink. It is important to
pay attention to the distinction between these two similar, closely related words.

An auth enabled server establishes an
[`AuthorizationSession`](https://github.com/mongodb/mongo/blob/r4.7.0/src/mongo/db/auth/authorization_session.h),
attached to the `Client` object as a decoration. The Authorization Session's job is to handle all
the authorization information for a single `Client` object. Note that if auth is not enabled, this
decoration remains uninitialized, and all further steps are skipped. Until the client chooses to
authenticate, this AuthorizationSession contains an empty AuthorizedUsers set (and by extension an
empty AuthorizedRoles set), and is thus "unauthorized", also known as "pre-auth".

When a client connects to a database and authorization is enabled, authentication sends a request to
get the authorization information of a specific user by calling addAndAuthorizeUser() on the
AuthorizationSession and passing in the `UserName` as an identifier.  The `AuthorizationSession` calls
functions defined in the
[`AuthorizationManager`](https://github.com/mongodb/mongo/blob/r4.7.0/src/mongo/db/auth/authorization_manager.h)
(described in the next paragraph) to both get the correct `User` object (defined below) from the
database and to check that the users attributed to a specific Client have the correct permissions to
execute commands.
[Here](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authorization_session_impl.cpp#L126)
is the authorization session calling into the authorization manager to acquire a user.

Clients are expected to authenticate at most one time on a connection.
Attempting to reauthenticate as the currently authenticated user results
in a warning being emitted to the global log, but the operation succeeds.
Attempting to authenticate as a new user on an already authenticated connection is an error.

### AuthName

The [AuthName](auth_name.h) template
provides the generic implementation for `UserName` and `RoleName` implementations.
Each of these objects is made up of three component pieces of information.

| Field | Accessor | Use |
| -- | -- | -- |
| `_name` | `getName()` | The symbolic name associated with the user or role, (e.g. 'Alice') |
| `_db` | `getDB()` | The authentication database associated with the named auth identifier (e.g. 'admin' or 'test') |
| `_tenant` | `getTenant()` | When used in multitenancy mode, this value retains a `TenantId` for authorization checking. |

[`UserName`](user_name.h) and [`RoleName`](role_name.h) specializations are CRTP defined
to include additional `getUser()` and `getRole()` accessors which proxy to `getName()`,
and provide a set of `constexpr StringData` identifiers relating to their type.

#### Serializations

* `getDisplayName()` and `toString()` create a new string of the form `name@db` for use in log messages.
* `getUnambiguousName()` creates a new string of the form `db.name` for use in generating `_id` fields for authzn documents and generating unique hashes for logical session identifiers.

#### Multitenancy

`AuthName` objects may be associated with a `TenantId` either separately via `AuthName(StringData name, StringData db, boost::optional<TenantId> tenant = boost::none)` or using the compound `DatabaseName` type `AuthName(StringData name, DatabaseName db)`.

When a `TenantId` is associated with an `AuthName`, it will NOT be included in `BSON` or `String` serializations unless explicitly requested with a boolean argument to these functions.

### Users

`User` objects contain authorization information with regards to a specific user in a database. The
`AuthorizationManager` has control over creation, management, and deletion of a `UserHandle` object,
which is a cache value object from the ReadThroughCache (described in [Authorization
Caching](#authorization-caching)). There can be multiple authenticated users for a single `Client`
object. The most important elements of a `User` document are the username and the roles set that the
user has.  While each `AuthorizationManagerExternalState` implementation may define its own
storage mechanism for `User` object data, they all ultimately surface this data in a format
compatible with the `Local` implementation, stored in the `admin.system.users` collection
with a schema as follows:

```javascript
{
    _id: "dbname.username",
    db: "dbname",
    user: "username",
    userId: UUIDv4(...),
    roles: [
      { db: "dbname", role: "role1" },
      { db: "dbname", role: "role2" },
      { db: "dbname", role: "role3" },
    ],
    credentials: {
        // This subdocument will contain $external: 1 *or* one or more SCRAM docs.
        "SCRAM-SHA-1": {
            iterationCount: 10000,
            salt: "base64DataForSCRAMsalt",
            serverKey: "base64DataForServerKey",
            storedKey: "base64DataForStoredKey",
        },
        "SCRAM-SHA-256": {
            iterationCount: 15000,
            salt: "base64DataForSCRAMsalt",
            serverKey: "base64DataForServerKey",
            storedKey: "base64DataForStoredKey",
        },
        "$external": 1,
    },
    authenticationRestrictions: [
        { clientSource: [ "127.0.0.1/8" ] },
        { serverAddress: [ "::1/128" ] },
        {
            clientSource: "172.16.12.34/32",
            serverAddress: "fe80::dead:beef:cafe/128",
        },
    ],
}
```

#### User Roles

In order to define a set of privileges (see [role privileges](#role-privileges) below)
granted to a given user, the user must be granted one or more `roles` on their user document,
or by their external authentication provider.  Again, a user with no roles has no privileges.

#### User Credentials

The contents of the `credentials` field will depend on the configured authentication
mechanisms enabled for the user.  For external authentication providers,
this will simply contain `$external: 1`.  For `local` authentication providers,
this will contain any necessary parameters for validating authentications
such as the `SCRAM-SHA-256` example above.

#### User Authentication Restrictions

A user definition may optionally list any number of authentication restrictions.
Currently, only endpoint based restrictions are permitted.  These require that a
connecting client must come from a specific IP address range (given in
[CIDR notation](https://en.wikipedia.org/wiki/Classless_Inter-Domain_Routing)) and/or
connect to a specific server address.

#### Any versus All criteria

For a given `authenticationRestriction` document to be satisfied,
all restrictions types (`clientSource` and/or `serverAddress` when provided)
must be satisfied.

Each of these restrictions types are considered to be satisfied when
the client endpoint is in a range specified by either the string `CIDR`
range, or any **one** of the elements of a list of ranges.

For example, a client connecting from `172.16.30.40` to a server at
address `192.168.70.80` will satisfy (or not) the following individual rules.

```javascript
// Succeeds as the clientSource is in range, and the server address is ignored.
{ clientSource: "172.16.0.0/12" }

// Fails as the client source is in range, but the serverAddress is not.
{ clientSource: "172.16.0.0/12", serverAddress: "10.0.0.0/8" }

// Succeeds as both addresses are in rage.
{ clientSource: "172.16.70.0/25", serverAddress: "192.168.70.80" }

// Succeeds as client address is in one of the allowed ranges.
{ clientSource: ["10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "fe80::/10"] }

// Fails as the server address is in none of the allowed ranges.
{ serverAddress: ["127.0.0.0/8", "::1"] }
```

Only **one** of the specified top-level `authenticationRestrictions` must be met
for a connection to be permitted.

Note that `authenticationRestrictions` may also be inherited from direct roles
and/or subordinate roles.
See [Role Authentication Restrictions](#role-authentication-restrictions) below.

The `{usersInfo: ...}` and `{rolesInfo: ...}` commands may be used to see
the combined, effective set of authentication restrictions by specifying
the `showAuthenticationRestrictions: true` argument.

### Roles

Similar to local user documents, role documents are managed in the `admin.system.roles`
collection on config and standalone servers.
Unlike users, the roles collection is always used regardless of external state implementation.
The schema of the `roles` collection is as follows:

```javascript
{
    _id: "dbname.rolename",
    db: "dbname",
    role: "rolename",
    roles: [
      // Subordinate roles
      { db: "dbname", roles: "otherRole1" },
      { db: "dbname", roles: "otherRole2" },
      { db: "dbname", roles: "otherRole3" },
    ],
    privileges: [
        // Cluster-wide
        { resource: { cluster: true }, actions: ['shutdown'] },
        // Specific database
        { resource: { db: "test", collection: "" }, actions: ['dropDatabase'] },
        // Collection name on any database
        { resource: { db: "", collection: "system.views" }, actions: ['insert'] },
        // Specific namespace
        { resource: { db: "admin", collection: "system.views", actions: ['update'] } },
        // Any "normal" resource
        { resource: {}, actions: ['find'] },
    ],
    authenticationRestrictions: [
        // See admin.system.users{authenticationRestrictions}
    ],
}
```

#### Role subordinate roles

The `roles` field in a role document defines the path of a tree with
each role "possessing" other roles, which in turn may possess others still.
For users possessing a given set of roles, their effective privileges and
`authenticationRestrictions` make up the union of all roles throughout the tree.

#### Role Privileges

Each role imparts privileges in the form of a set of `actions` permitted
against a given `resource`.  The strings in the `actions` list correspond
1:1 with `ActionType` values as specified [here](https://github.com/mongodb/mongo/blob/92cc84b0171942375ccbd2312a052bc7e9f159dd/src/mongo/db/auth/action_type.h).
Resources may be specified in any of the following nine formats:

| `resource` | Meaning |
| --- | --- |
| {} | Any `normal` collection |
| { db: 'test', collection: '' } | All `normal` collections on the named DB |
| { db: '', collection: 'system.views' } | The specific named collection on all DBs |
| { db: 'test', collection: 'system.view' } | The specific namespace (db+collection) as written |
| { cluster: true } | Used only by cluster-level actions such as `replsetConfigure`. |
| { system_bucket: '' } | Any collection with a prefix of `system.buckets.` in any db|
| { db: '', system_buckets: 'example' } | A collection named `system.buckets.example` in any db|
| { db: 'test', system_buckets: '' } | Any collection with a prefix of `system.buckets.` in `test` db|
| { db: 'test', system_buckets: 'example' } | A collected named `system.buckets.example` in `test` db|

#### Normal resources

Collection names starting with `system.` on any database,
or starting with `replset.` on the `local` database are considered "special"
and are not covered by the "Any normal collection" resource case.
All other collections are considered `normal` collections.

#### Role Authentication Restrictions

Authentication restrictions defined on a role have the same meaning as
those defined directly on users.  The effective set of `authenticationRestrictions`
imposed on a user is the union of all direct and indirect authentication restrictions.

### Privilege

A [Privilege](privilege.h) represents a tuple of [ResourcePattern](resource_pattern.h) and
[set](action_set.h) of [ActionType](action_type.idl)s which describe the resources which
may be acted upon by a user, and what actions they may perform, respectively.

A [PrivilegeVector](privilege.h) is an alias for `std::vector<Privilege>` and represents
the full set of privileges across all resource and actionype conbinations for the user or role.

#### ResourcePattern

A resource pattern is a combination of a [MatchType](action_type.idl) with a `NamespaceString` to possibly narrow the scope of that `MatchType`.  Most MatchTypes refer to some storage resource, such as a specific collection or database, however `kMatchClusterResource` refers to an entire host, replica set, or cluster.

| MatchType | As encoded in a privilege doc | Usage |
| -- | -- | -- |
| `kMatchNever` | _Unexpressable_ | A base type only used internally to indicate that the privilege specified by the ResourcePattern can not match any real resource |
| `kMatchClusterResource` | `{ cluster : true }` | Commonly used with host and cluster management actions such as `ActionType::addShard`, `ActionType::setParameter`, or `ActionType::shutdown`. |
| `kMatchAnyResource` | `{ anyResource: true }` | Matches all storage resources, even [non-normal namespaces](#normal-namespace) such as `db.system.views`. |
| `kMatchAnyNormalResource` | `{ db: '', collection: '' }` | Matches all [normal](#normal-namespace) storage resources. Used with [builtin role](builtin_roles.cpp) `readWriteAnyDatabase`. |
| `kMatchDatabaseName` | `{ db: 'dbname', collection: '' }` | Matches all [normal](#normal-namespace) storage resources for a specific named database. Used with [builtin role](builtin_roles.cpp) `readWrite`. |
| `kMatchCollectionName` | `{ db: '', collection: 'collname' }` | Matches all storage resources, normal or not, which have the exact collection suffix '`collname`'.  For example, to provide read-only access to `*.system.js`. |
| `kMatchExactNamespace` | `{ db: 'dbname', collection: 'collname' }` | Matches the exact namespace '`dbname`.`collname`'. |
| `kMatchAnySystemBucketResource` | `{ db: '', system_buckets: '' }` | Matches the namespace pattern `*.system.buckets.*`. |
| `kMatchAnySystemBucketInDBResource` | `{ db: 'dbname', system_buckets: '' }` | Matches the namespace pattern `dbname.system.buckets.*`. |
| `kMatchAnySystemBucketInAnyDBResource` | `{ db: '', system_buckets: 'suffix' }` | Matches the namespace pattern `*.system.buckets.suffix`. |
| `kMatchExactSystemBucketResource` | `{ db: 'dbname', system_buckets: 'suffix' }` | Matches the exact namespace `dbname.system.buckets.suffix`. |

##### Normal Namespace

A "normal" resource is a `namespace` which does not match either of the following patterns:

| Namespace pattern | Examples | Usage |
| -- | -- | -- |
| `local.replset.*` | `local.replset.initialSyncId` | Namespaces used by Replication to manage per-host state. |
| `*.system.*` | `admin.system.version` `myDB.system.views` | Collections used by the database to support user collections. |

See also: [NamespaceString::isNormalCollection()](../namespace_string.h)

#### ActionType

An [ActionType](action_type.idl) is a task which a client may be expected to perform.  These are combined with [ResourcePattern](#resourcepattern)s to produce a [Privilege](#privilege).  Note that not all `ActionType`s make sense with all `ResourcePattern`s (e.g. `ActionType::shutdown` applied to `ResourcePattern` `{ db: 'test', collection: 'my.awesome.collection' }`), however the system will generally not prohibit declaring these combinations.

### User and Role Management

`User Management Commands`, sometimes referred to as `UMCs` provide an
abstraction for mutating the contents of the local authentication database
in the `admin.system.users` and `admin.system.roles` collections.
These commands are implemented primarily for config and standalone nodes in
[user\_management\_commands.cpp](https://github.com/mongodb/mongo/blob/92cc84b0171942375ccbd2312a052bc7e9f159dd/src/mongo/db/commands/user_management_commands.cpp),
and as passthrough proxies for mongos in
[cluster\_user\_management\_commands.cpp](https://github.com/mongodb/mongo/blob/92cc84b0171942375ccbd2312a052bc7e9f159dd/src/mongo/s/commands/cluster_user_management_commands.cpp).
All command payloads and responses are defined via IDL in
[user\_management\_commands.idl](https://github.com/mongodb/mongo/blob/92cc84b0171942375ccbd2312a052bc7e9f159dd/src/mongo/db/commands/user_management_commands.idl)

#### UMC Transactions

Most command implementations issue a single `CRUD` op against the
relevant underlying collection using `DBDirectClient` after
validating that the command's arguments refer to extant roles, actions,
and other user-defined values.

The `dropRole` and `dropAllRolesFromDatabase` commands can not be
expressed as a single CRUD op.  Instead, they must issue all three of the following ops:

1. `Update` the users collection to strip the role(s) from all users possessing it directly.
1. `Update` the roles collection to strip the role(s) from all other roles possessing it as a subordinate.
1. `Remove` the role(s) from the roles collection entirely.

In order to maintain consistency during replication and possible conflicts,
these `UMC` commands leverage transactions through the `applyOps` command
allowing a rollback.
The [UMCTransaction](https://github.com/mongodb/mongo/blob/92cc84b0171942375ccbd2312a052bc7e9f159dd/src/mongo/db/commands/user_management_commands.cpp#L756)
class provides an abstraction around this mechanism.

#### Multitenancy

When acting in multitenancy mode, each tenant uses distinct storage for their users and roles.
For example, given a `TenantId` of `"012345678ABCDEF01234567"`, all users for that tenant will
be found in the `012345678ABCDEF01234567_admin.system.users` collection, and all roles will be
found in the `012345678ABCDEF01234567_admin.system.roles` collection.

### Command Execution

When a client attempts to execute a command, the service entry point calls
[`checkAuthorization`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/service_entry_point_common.cpp#L1026),
which calls `doCheckAuthorization`, a virtual function implemented by individual commands. For
example, if a user is attempting to call find, the
[`FindCmd`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/find_cmd.cpp#L136)
overrides the `CommandInvocation` class with its own implementation of
[`Invocation`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/find_cmd.cpp#L182).
That class implements its own version of
[`doCheckAuthorization`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/find_cmd.cpp#L218).
`doCheckAuthorization` gets the `AuthorizationSession` for the `Client` that is executing the command
and checks all the privileges of the `Client` and either throws if there is an issue or returns
if all the authorization checks are complete.

### Authorization Caching

Logged in users are cached using a structure called a
[`ReadThroughCache`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/util/read_through_cache.h).
In the `AuthorizationSession` there is a structure called the `UserSet` that takes ownership of
`UserHandle` objects which determine what Users are authenticated on a Client object. Because the
`AuthorizationManager` implements a
[`ReadThroughCache`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/util/read_through_cache.h),
all requests for getting user information flow through a
[`_lookup`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authorization_manager_impl.cpp#L692)
function which calls `getUserDescription()`.

There are a few different ways that the `UserCache` can get invalidated. Some user management
commands such as
[`UpdateUser`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/user_management_commands.cpp#L887)
will invalidate any cache entry for a single user. Some User management commands such as
[`UpdateRole`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/user_management_commands.cpp#L1499)
will invalidate the entire cache. Unfortunately, when a user management command is run and the cache
is invalidated, mongos' are not notified of the changes. For this reason, there is a value stored in
the admin database that represents the cache generation. When a mongod invalidates any item in the
cache, it also updates the cache generation. Mongos' have a periodicJob on the
[`PeriodicRunner`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/util/periodic_runner.h)
known as the
[`UserCacheInvalidator`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/user_cache_invalidator_job.cpp)
that checks the cache generation every n seconds (defaulted to 30 seconds) that sweeps and
invalidates the cache for the authorization manager in the mongos. Direct writes to the
`admin.system.users` and `admin.system.roles` collections will also result in invalidation via the
`OpObserver` hook. Writing to these collections directly is strongly discouraged.

### Authorization Manager External State

Implementations of the `AuthorizationManagerExternalState` interface define a way to get state
information from external systems. For example, when the Authorization Manager needs to get
information regarding the schema version of the Authorization System, information that is stored in
the database that needs to be queried, the Authorization Manager will call
[`getStoredAuthorizationVersion`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authz_manager_external_state.h#L83).
Because mongod and mongos processes have different ways of interacting with the data stored in the
database, there is an
[`authz_manager_external_state_d`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authz_manager_external_state_d.h)
and an
[`authz_manager_external_state_s`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authz_manager_external_state_s.h)
version of the external state implementation, with the former referencing local values in the
storage subsystem, while the latter delegates to remove cluster config servers.

### Types of Authorization

#### Local Authorization

Local authorization stores users and their respective roles in the database. Users in a database are
generally created using the
[`CreateUser`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/user_management_commands.cpp#L765)
command. A user is authenticated to a specific database. The newly created user document is saved to
the collection
[`admin.system.users`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authorization_manager.cpp#L56).
The user must supply roles when running the `createUser` command. Roles are stored in
[`admin.system.roles`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authorization_manager.cpp#L52).

#### LDAP Authorization

LDAP authorization is an external method of getting roles. When a user authenticates using LDAP,
there are roles stored in the User document specified by the LDAP system. The LDAP system relies on
the
[`AuthzManagerExternalStateLDAP`](https://github.com/10gen/mongo-enterprise-modules/blob/r4.4.0/src/ldap/authz_manager_external_state_ldap.h)
to make external requests to the LDAP server. The `AuthzManagerExternalStateLDAP` wraps the
`AuthzManagerExternalStateLocal` for the current process, initially attempting to route all
Authorization requests to LDAP and falling back on Local Authorization. LDAP queries are generated
from
[`UserRequest`](https://github.com/10gen/mongo-enterprise-modules/blob/r4.4.0/src/ldap/authz_manager_external_state_ldap.cpp#L75-L113)
objects, passing just the username into the query. If a user has specified the `userToDNMapping`
server parameter, the `AuthorizationManager` calls the LDAPManager to transform the usernames into
names that the LDAP server can understand. The LDAP subsystem relies on a complicated string
escaping sequence, which is handled by the LDAPQuery class. After LDAP has returned the `User`
document, it resolves role names into privileges by dispatching a call to
[`Local::getUserObject`](https://github.com/10gen/mongo-enterprise-modules/blob/r4.7.0/src/ldap/authz_manager_external_state_ldap.cpp#L110-L123)
with a `UserRequest` struct containing a set of roles to be resolved.

Connections to LDAP servers are made by the `LDAPManager` through the
[`LDAPRunner`](https://github.com/10gen/mongo-enterprise-modules/blob/r4.4.0/src/ldap/ldap_runner.h)
by calling `bindAsUser()`. `BindAsUser()` attempts to set up a connection to the LDAP server using
connection parameters specified through the command line when starting the process.The
[`LDAPConnectionFactory`](https://github.com/10gen/mongo-enterprise-modules/blob/r4.4.0/src/ldap/connections/ldap_connection_factory.h)
is the class that is actually tasked with establishing a connection and sending raw bytes over the
wire to the LDAP server, all other classes decompose the information to send and use the factory to
actually send the information. The `LDAPConnectionFactory` has its own thread pool and executor to
drive throughput for authorization. LDAP has an
[`LDAPUserCacheInvalidator`](https://github.com/10gen/mongo-enterprise-modules/blob/r4.4.0/src/ldap/ldap_user_cache_invalidator_job.h)
that periodically sweeps the `AuthorizationManager` and deletes user entries that have `$external` as
their authentication database.

There are a few thread safety concerns when making connections to the LDAP server. MongoDB uses
LibLDAP to make connections to the LDAP server. LibLDAP comes without any thread safety guarantees,
so all the calls to libLDAP are wrapped with mutexes to ensure thread safety when connecting to LDAP
servers on certain distros. The logic to see whether libLDAP is thread-safe lives
[here](https://github.com/10gen/mongo-enterprise-modules/blob/r4.4.0/src/ldap/connections/openldap_connection.cpp#L348-L378).

#### <a name="x509azn"></a>X.509 Authorization

In user acquisition in the
[`AuthorizationManager`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authorization_manager_impl.cpp#L454-L465),
roles can come either from an extension in the client X509 certificate or from the local
authorization database. If a client connects using TLS and authenticates using an X509 certificate,
the server may use the X509 certificate to derive the roles for the user through a custom X509
extension. The roles extension uses the OID described
[here](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/util/net/ssl_manager.h#L163-L165). The
roles are DER encoded in the certificate. They are read by the SSL manager and stored in the SSL
Peer Info struct, which is eventually used by
[`AcquireUser`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authorization_manager_impl.cpp#L454-L465)
if X509 authorization is used. A tunable parameter in X509 Authorization is tlsCATrusts. TLSCATrusts
is a setParameter that allows a user to specify a mapping of CAs that are trusted to use X509
Authorization to a set of roles that are allowed to be specified by the CA.

### Cursors and Operations

When running a query, the database batches results in groups, allowing clients to get the remainder
of the results by calling
[`getMore`](https://github.com/mongodb/mongo/blob/r4.7.0/src/mongo/db/commands/getmore_cmd.cpp). In
order to ensure correct authorization rights to run the `getMore` command, there are some extra
authorization checks that need to be run using cursors and operations. When a CRUD operation is run,
a cursor is created with client information and registered with the `CursorManager`. When `getMore` is
called, the command uses the cursor to gather the next batch of results for the request. When a
cursor is created, a list of the client's authenticated users and privileges required to run the
command are added to the cursor. When the getMore command is issued, before continuing the CRUD
operation to return the next batch of data, a method called
[`isCoauthorizedWith`](https://github.com/mongodb/mongo/blob/r4.7.0/src/mongo/db/auth/authorization_session.h#L343-L346)
is run by the [`AuthorizationSession`](#authorization) against the list of identities copied to the
cursor and the Client object. `isCoauthorizedWith` returns true if the `AuthorizationSession` and
the authenticated users on the cursor share an authenticated user, or if both have no authenticated
users. If `isCoauthorizedWith` is true, the function `isAuthorizedForPrivileges` is called on the
`AuthorizationSession` to see if the current session has all the privileges to run the initial
command that created the cursor, using the privileges stored on the cursor from earlier. If both of
these checks pass, the getMore command is allowed to proceed, else it errors stating that the cursor
was not created by the same user.

Every operation that is performed by a client generates an
[`OperationContext`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/operation_context.h#L77).
If a client has `ActionType::killop` on the cluster resource, then the client is able to kill its
operations if it has the same users (impersonated or otherwise) as the client that owns the
`OperationContext` by issuing a `{killOp: 1}` command. When the command is issued, it calls
[`isCoauthorizedWithClient`](https://github.com/mongodb/mongo/blob/r4.7.0/src/mongo/db/auth/authorization_session.h#L332-L341)
and checks the current client's authorized users and authorized impersonated users.

### Contracts

[Authorization
Contracts](https://github.com/mongodb/mongo/blob/r4.9.0-rc0/src/mongo/db/auth/authorization_contract.h)
were added in v5.0 to support API Version compatibility testing. Authorization contracts consist of
three pieces:
1. A list of privileges and checks a command makes against `AuthorizationSession` to check if a user
   is permitted to run the command. These privileges and checks are declared in an IDL file in the
   `access_check` section. The contract is compiled into the command definition and is available via
   [`Command::getAuthorizationContract`](https://github.com/mongodb/mongo/blob/r4.9.0-rc0/src/mongo/db/commands.h#L582-L588).
2. During command execution, `AuthorizationSessionImpl` records all privilege and access checks that
   are performed against it into a contract.
3. After a command completes, the server verifies that the command's recorded checks are a subset of
   its compile-time contract. This verification only occurs if the server has enabled testing
   diagnostics and does not occur in normal production use of MongoDB.

All commands in API Version 1 are required to have an `access_check` section in IDL. The two
exceptions are `getMore` and `explain` since they inherit their checks from other commands.

## External References

Refer to the following links for definitions of the Classes referenced in this document:

| Class | File | Description |
| --- | --- | --- |
| `ActionType` | [mongo/db/auth/action\_type.h](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/action_type.h) | High level categories of actions which may be performed against a given resource (e.g. `find`, `insert`, `update`, etc...) |
| `AuthenticationSession` | [mongo/db/auth/authentication\_session.h](https://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authentication_session.h) | Session object to persist Authentication state |
| `AuthorizationContract` | [mongo/db/auth/authorization_contract.h](https://github.com/mongodb/mongo/blob/r4.9.0-rc0/src/mongo/db/auth/authorization_contract.h) | Contract generated by IDL|
| `AuthorizationManager` | [mongo/db/auth/authorization\_manager.h](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authorization_manager.h) | Interface to external state providers |
| `AuthorizationSession` | [mongo/db/auth/authorization\_session.h](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authorization_session.h) | Representation of currently authenticated and authorized users on the `Client` connection |
| `AuthzManagerExternalStateLocal` | [.../authz\_manager\_external\_state\_local.h](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authz_manager_external_state_local.h) | `Local` implementation of user/role provider |
| `AuthzManagerExternalStateLDAP` | [.../authz\_manager\_external\_state\_ldap.h](https://github.com/10gen/mongo-enterprise-modules/blob/r4.4.0/src/ldap/authz_manager_external_state_ldap.h) | `LDAP` implementation of users/role provider |
| `Client` | [mongo/db/client.h](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/client.h) | An active client session, typically representing a remote driver or shell |
| `Privilege` | [mongo/db/auth/privilege.h](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/privilege.h) | A set of `ActionType`s permitted on a particular `resource' |
| `ResourcePattern` | [mongo/db/auth/resource\_pattern.h](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/resource_pattern.h) | A reference to a namespace, db, collection, or cluster to apply a set of `ActionType` privileges to |
| `RoleName` | [mongo/db/auth/role\_name.h](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/role_name.h) | A typed tuple containing a named role on a particular database |
| `User` | [mongo/db/auth/user.h](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/user.h) | A representation of a authorization user, including all direct and subordinte roles and their privileges and authentication restrictions |
| `UserName` | [mongo/db/auth/user\_name.h](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/user_name.h) | A typed tuple containing a named user on a particular database |
