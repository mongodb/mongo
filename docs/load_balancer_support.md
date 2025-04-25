# Proxy protocol support

`mongod` and `mongos` have built-in support for connections made via L4 load balancers using
the [proxy protocol][proxy-protocol-url] header. Placing `mongos` or `mongod` behind load balancers
requires proper configuration of the load balancers, `mongos`, and `mongod`.

# Configuring mongod

To use `mongod` with a L4 load balancer (or reverse proxy) it _must_ be configured with the
`proxyPort` config option whose value can be specified at program start in any of the ways
mentioned in the server config documentation. This config option opens a new port to which the
L4 load balancer _must_ connect.

The L4 load balancer (or reverse proxy) _must_ emit a [proxy protocol][proxy-protocol-url] header
at the start of its connection stream. `mongod` supports both version 1 and version 2 of the proxy
standard.

# Reverse proxy vs load balancer

Sharded clusters might be configured to work with either a L4 load balancer or a reverse proxy. In
both cases the proxy or load balancer _must_ connect to the `mongos`'s load-balancer port.

Placing `mongos` behind a reverse proxy does not hide the list of `mongos`. The driver will choose
a specific `mongos` to connect to via the reverse proxy.

Placing `mongos` behind an L4 load balancer hides the list of `mongos`. The driver only sees the
load balancer and, the connections it makes are routed by the load balancer to a `mongos`. There is
no guarantee that all connections from a driver target the same `mongos` : generally we can expect
that connections from a driver are distributed among multiple `mongos`.

# Configuring mongos with a reverse proxy

When a sharded cluster is deployed with a reverse proxy, there are two conditions that must be
fulfilled :

- `mongos` must be configured with the [MongoDB Server Parameter](https://docs.mongodb.com/manual/reference/parameters/) `loadBalancerPort` whose value can be specified at program start in any of the ways mentioned in the server parameter documentation.
  This option causes `mongos` to open a second port. All connections made from reverse proxy _must_ be made over this port, and no regular connections (without HAProxy protocol header) may be made over this port.
- The reverse proxy _must_ be configured to emit a [proxy protocol][proxy-protocol-url] header
  at the [start of its connection stream](https://github.com/mongodb/mongo/commit/3a18d295d22b377cc7bc4c97bd3b6884d065bb85). `mongos` [supports](https://github.com/mongodb/mongo/commit/786482da93c3e5e58b1c690cb060f00c60864f69) both version 1 and version 2 of the proxy
  protocol standard.

The driver does not require any configuration change compared to a cluster without a reverse proxy.

# Configuring mongos with a load balancer

When a sharded cluster is deployed with an L4 load balancer there are three conditions that must be
fulfilled :

- `mongos` must be configured with the [MongoDB Server Parameter](https://docs.mongodb.com/manual/reference/parameters/) `loadBalancerPort` whose value can be specified at program start in any of the ways mentioned in the server parameter documentation.
  This option causes `mongos` to open a second port. All connections made from load
  balancers _must_ be made over this port, and no regular connections (without HAProxy protocol header) may be made over this port.
- The L4 load balancer _must_ be configured to emit a [proxy protocol][proxy-protocol-url] header
  at the [start of its connection stream](https://github.com/mongodb/mongo/commit/3a18d295d22b377cc7bc4c97bd3b6884d065bb85). `mongos` [supports](https://github.com/mongodb/mongo/commit/786482da93c3e5e58b1c690cb060f00c60864f69) both version 1 and version 2 of the proxy
  protocol standard.
- Clients (drivers or shells) connecting to a `mongos` through the load balancer must set the `loadBalanced` option,
  e.g., when connecting to a local `mongos` instance through the load balancer, if the `loadBalancerPort` server parameter was set to 20100, the
  connection string must be of the form `"mongodb://localhost:20100/?loadBalanced=true"`.

There are some subtle behavioral differences that the load balancer options enable, chief of
which is how `mongos` deals with open cursors on client disconnection. Over a normal connection,
`mongos` will keep open cursors alive for a short while after client disconnection in case the
client reconnects and continues to request more from the given cursor. Since client reconnections
aren't expected behind a load balancer (as the load balancer will likely redirect a given client
to a different `mongos` instance upon reconnection), we eagerly [close cursors](https://github.com/mongodb/mongo/commit/b429d5dda98bbe18ab0851ffd1729d3b57fc8a4e) on load balanced
client disconnects. We also [abort any in-progress transactions](https://github.com/mongodb/mongo/commit/74628ed4e314dfe0fd69d3fbae1411981a869f6b) that were initiated by the load balanced client.

[proxy-protocol-url]: https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt
