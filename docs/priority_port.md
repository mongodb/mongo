# Priority port support

`mongod` and `mongos` support a dedicated **priority port** intended for **internal, high-priority
operations** such as automation monitoring, MongoTune, and critical intra-cluster replication
traffic.

With a priority port configured:

- The database listens on a second TCP port in addition to the main port.
- Connections accepted on the priority port are exempt from connection limits, connection
  establishment rate limiting, and ingress request rate limiting.
- gRPC is not supported.

The feature is **disabled by default**.

---

# Configuring `mongod` and `mongos`

You can configure the priority port via command line or config file:

**Command line:**

```sh
mongod --port <mainPort> --priorityPort <priorityPort> ...
mongos --port <mainPort> --priorityPort <priorityPort> ...
```

**YAML config file:**

```yaml
net:
  port: <mainPort>
  bindIp: localhost,<hostnames-or-ip-addresses>
  priorityPort: <priorityPort>
```

When the transport layer starts:

- A **separate listener thread** is created for the priority port in the ASIO transport layer.
- Sessions created from the priority port are tagged so downstream code can distinguish them from
  main-port sessions (similar to the load balancer port implementation).

---

# Behavior of priority port connections

Priority-port connections differ from normal connections in several ways.

## Connection limits

When a new connection is accepted:

- Connections from the priority port are treated as **limit-exempt** in the session manager, reusing
  the existing exemption machinery used for CIDR-based exemptions.
- These connections can continue to be created even when the normal connection limit is reached.

Metrics:

- `serverStatus.connections.priority` counts current connections on the priority port only.
- These connections are also included in `connections.limitExempt` (along with CIDR-based
  exemptions).

## Rate limiters

Two ingress-side rate limiters recognize priority-port exemptions:

- [**SessionEstablishmentRateLimiter**](../src/mongo/db/admission/README.md#session-establishment-rate-limiter)
  (connection establishment)
- [**IngressRequestRateLimiter**](../src/mongo/db/admission/README.md#ingress-request-rate-limiting)
  (request rate limiting)

## Logging and profiling

For observability and debugging, the server records whether an operation came through the priority
port:

- `CurOp` / currentOp output includes a flag indicating the connection is from the priority port.
- Slow query log and profiler entries include whether the operation was executed via a priority-port
  connection.
- Client summary reports also distinguish clients on the main vs priority port.

---

# Connecting clients to the priority port

## Replica set connections

To connect to a replica set via the priority port, a user must:

- Use a connection string that points directly at a specific host and priority port.
- Set `directConnection=true` to disable SDAM and prevent the driver from using hello-based host
  discovery, which currently does not advertise the priority port.

Example:

```text
mongodb://hostA:27018/?directConnection=true
```

## Sharded cluster connections via `mongos`

For `mongos`:

- You may connect directly to the `mongos` priority port.
- `directConnection=true` is **not required** for `mongos` connections, since SDAM is not used in
  the same way.

Important limitation:

- **Priority does not automatically propagate**:
  - If a client connects to a `mongos` via the priority port and `mongos` forwards a command to
    shards, those shard-side connections still use the main ports and do **not** inherit
    priority-port behavior in the current implementation.

---
