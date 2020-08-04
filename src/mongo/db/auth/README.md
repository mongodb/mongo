# Identity And Access Management

## Table of Contents

- [High Level Overview](#high-level-overview)
  - [Cursors and Operations](#cursors-and-operations)
- [Authentication](#authentication)
  - [SASL](#sasl)
- [Authorization](#authorization)
  - [Authorization Caching](#authorization-caching)
  - [Authorization Manager External State](#authorization-manager-external-state)
  - [Types of Authorization](#types-of-authorization)
    - [Local Authorization](#local-authorization)
    - [LDAP Authorization](#ldap-authorization)
    - [X.509 Authorization](#x509-authorization)

## High Level Overview

### Cursors and Operations

## Authentication

### SASL

## Authorization

Auth**orization** describes the set of access controls conferred by the system on a connected
client. It naturally flows from Auth**entication**, as authenticated clients may be trusted with
addition access, thus upon completing [authentication](#Authentication), a client's authorization
tends to expand. Similarly, upon logout a client's authorization tends to shrink. It is important to
pay attention to the distinction between these two similar, closely related words.

An auth enabled server establishes an `AuthorizationSession`, attached to the `Client` object as a
decoration. The Authorization Session's job is to handle all the authorization information for a
single `Client` object. Note that if auth is not enabled, this decoration remains uninitialized, and
all further steps are skipepd. Until the client chooses to authenticate, this AuthorizationSession
contains an empty AuthorizedUsers set (and by extension an empty AuthorizedRoles set), and is this
"unauthorized", also known as "pre-auth".

When a client connects to a database and authorization is enabled, authentication sends a request
to get the authorization information of a specific user by calling addAndAuthorizeUser() on the
AuthorizationSession and passing in the UserName as an identifier.  The `AuthorizationSession` calls
functions defined in the `AuthorizationManager` (described in the next paragraph) to both get the
correct `User` object (defined below) from the database and to check that the users attributed to a
specific Client have the correct permissions to execute commands.
[Here](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/authorization_session_impl.cpp#L126)
is the authorization session calling into the authorization manager to acquire a user.

`User` objects contain authorization information with regards to a specific user in a database. The
`AuthorizationManager` has control over creation, management, and deletion of a `UserHandle` object,
which is a cache value object from the ReadThroughCache (described in [Authorization
Caching](#authorization-caching)). There can be multiple authenticated users for a single `Client`
object. The most important elements of a `User` document are the username and the roles set that the
user has. A single `User` can have multiple `Roles`. `Roles` can contain a set of `Privileges`,
which is a `Resource` and a set of `ActionTypes` performable on that `Resource`, and other Roles.
The ultimate privilege set of a user is the union of all the privileges across all roles contained
by a `User`. A `Role` is represented as a `RoleName`, which the `AuthorizationManager` uses to look
up the details of the `Role`. A `Resource` is either a collection, a database, or a cluster. A
"normal" `Resource` is any database and non-system collection. `ActionTypes` are used when checking
whether a user is able to perform an action. For example, if a user wants to drop a database, they
would need `ActionType::dropDatabase` on the specific database resource (see
[here](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/dbcommands.cpp#L119-L120)).
The `User` objects are stored in the `AuthorizationSession` in a structure called the UserSet. Below
are links to all the types referred to in this paragraph.

- [`Client`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/client.h)
- [`User`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/user.h)
- [`RoleName`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/role_name.h)
- [`Privilege`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/privilege.h)
- [`Resource`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/resource_pattern.h)
- [`ActionTypes`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/action_type.h)

When a client attempts to execute a command, the service entry point calls
[`checkAuthorization`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/service_entry_point_common.cpp#L1026),
which calls `doCheckAuthorization`, a virtual function implemented by individual commands. For
example, if a user is attempting to call find, the
[`FindCmd`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/find_cmd.cpp#L136)
overrides the `CommandInvocation` class with its own implementation of
[`Invocation`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/find_cmd.cpp#L182).
That class implements its own version of
[`doCheckAuthorization`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands/find_cmd.cpp#L218).
`doCheckAuthorization` gets the authorizationSession for the Client that is executing the command
and checks all the permissioning of the `Client` and either throws if there is an issue or returns
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
Below are the schema documents that these collections use; you can see how these documents are
parsed
[here](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/auth/user_document_parser.cpp).

`admin.system.users` schema:

```C++
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

`admin.system.roles` schema:

```C++
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
        { resource: { db: "test" }, actions: ['dropDatabase'] },
        // Collection name on any database
        { resource: { collection: "system.views" }, actions: ['insert'] },
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

#### LDAP Authorization

LDAP authorization is an external method of getting roles. When a user authenticates using LDAP,
there are roles stored in the User document specified by the LDAP system. The LDAP system relies on
the
[`AuthzManagerExternalStateLDAP`](https://github.com/10gen/mongo-enterprise-modules/blob/r4.4.0/src/ldap/authz_manager_external_state_ldap.h)
to make external requests to the LDAP server. The `AuthzManagerExternalStateLDAP` wraps the
`AuthzManagerExternalStateLocal` for the current process, initally attempting to route all
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
that periodically sweeps the AuthorizationManager and deletes user entries that have `$external` as
their authentication database.

There are a few thread safety concerns when making connections to the LDAP server. MongoDB uses
LibLDAP to make connections to the LDAP server. LibLDAP comes without any thread safety guarantees,
so all the calls to libLDAP are wrapped with mutexes to ensure thread safety when connecting to LDAP
servers on certain distros. The logic to see whether libLDAP is thread-safe lives
[here](https://github.com/10gen/mongo-enterprise-modules/blob/r4.4.0/src/ldap/connections/openldap_connection.cpp#L348-L378).

#### X.509 Authorization

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
