# Split Horizon

## Overview

Split horizon enables a single replica set member to advertise different `host:port` addresses to
clients depending on which network zone the client connected from. This is the mechanism behind the
MongoDB
[`horizons`](https://www.mongodb.com/docs/manual/reference/replica-configuration/#mongodb-rsconf-rsconf.members-n-.horizons)
replica set member configuration option.

A typical use case is a deployment that spans internal and external networks. Internal clients reach
a member at `internal.example.com:27017` while external clients reach the same member through a
different DNS name such as `external.example.com:25000`. Without split horizon, the `hello` response
would advertise only one address, and clients in the other network zone would be unable to reach the
member using the returned host list.

## Key concepts

### Horizons

A **horizon** is a named network view. Every replica set member always has a `__default` horizon
corresponding to the member's `host` field in the replica set configuration. Additional horizons are
declared in the optional `horizons` sub-document of each member config:

```json
{
  "_id": 0,
  "host": "internal.example.com:27017",
  "horizons": {
    "external": "external.example.com:25000"
  }
}
```

The `__default` name is reserved and cannot be used explicitly.

### Horizon determination via SNI

When a TLS connection is established, the server captures the **SNI (Server Name Indication)**
hostname from the TLS handshake. This SNI name is stored on the `Client` object as
`SplitHorizon::Parameters`. When the client issues a `hello` (or legacy `isMaster`) command, the
replication coordinator calls `SplitHorizon::determineHorizon()` which looks up the SNI hostname in
the **reverse mapping** to find the matching horizon. If no match is found (or no SNI was provided),
the `__default` horizon is used.

The determined horizon is then used to select which set of addresses to include in the `hello`
response, so every member's address is returned from the perspective of the client's network zone.

## Data structures

The `SplitHorizon` class models a **single member's** address mappings across all horizons. It is
not a cluster-wide structure; each `MemberConfig` owns its own `SplitHorizon` instance.

### Forward mapping (`ForwardMapping`)

```
StringMap<HostAndPort>    horizon name  -->  host:port
```

Maps each horizon name (e.g. `"__default"`, `"external"`) to the `HostAndPort` at which this member
is reachable under that horizon. Always contains at least the `__default` entry.

### Reverse host mapping (`ReverseHostOnlyMapping`)

```
std::map<string, string>    hostname  -->  horizon name
```

Maps each **hostname** (without port) that this member can be reached at back to the horizon name
that uses it. This is used by `determineHorizon()` to match an incoming SNI name to a horizon.
Because the lookup is by hostname only (not host:port), each hostname must be unique across all
horizons for a given member.

## Construction and validation

A `SplitHorizon` can be constructed in two ways:

1. **From BSON** (`HostAndPort host`, `optional<BSONObj> horizonsObject`) -- used when parsing a
   replica set configuration (`replSetInitiate` / `replSetReconfig`). The `host` argument becomes
   the `__default` horizon entry and the optional `horizonsObject` supplies the additional horizons.

2. **From a `ForwardMapping` directly** -- used internally and in tests.

Both paths converge to build the reverse mapping from the forward mapping. The following invariants
are enforced during construction:

- The `__default` horizon must always be present.
- Horizon names must be non-empty and unique.
- The reserved name `__default` cannot appear in the `horizons` BSON object.
- Hostnames must be unique across all horizons (two horizons on the same member cannot share a
  hostname, even with different ports).
- The `horizons` BSON object, if present, cannot be empty.

Violations produce a `BadValue` error.

## Integration points

| Component                                           | How it uses split horizon                                                                                                                                                                                                                         |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **`MemberConfig`** (`member_config.h`)              | Owns a `SplitHorizon` instance. Delegates `getHostAndPort(horizon)` and `determineHorizon(params)` to it.                                                                                                                                         |
| **`replication_info.cpp`** (hello/isMaster handler) | Calls `SplitHorizon::setParameters()` during connection setup to capture the client's SNI name, then passes `SplitHorizon::getParameters()` to the replication coordinator so the `hello` response is built with the correct horizon's addresses. |
| **`ReplicationCoordinator`**                        | Uses the horizon parameter from the client to select which `HostAndPort` to return for each member in topology responses.                                                                                                                         |

## Serialization

`SplitHorizon::toBSON()` emits the `horizons` sub-document for a member config. If the member has
only the `__default` horizon, no `horizons` field is emitted (it would be redundant with the `host`
field). The `__default` entry is never serialized into the `horizons` object. Horizon entries are
sorted lexicographically by name before serialization for deterministic output.
