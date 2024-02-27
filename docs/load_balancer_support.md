# Load balancer support

`mongos` has built-in support for connections made via L4 load balancers. However, placing `mongos`
endpoints behind load balancers requires proper configuration of the load balancers, `mongos`, and
any drivers or shells used to connect to the database. Three conditions must be fulfilled for
`mongos` to be used behind a load balancer:

-   `mongos` must be configured with the [MongoDB Server Parameter](https://docs.mongodb.com/manual/reference/parameters/) `loadBalancerPort` whose value can be specified at program start in any of the ways mentioned in the server parameter documentation.
    This option causes `mongos` to open a second port that expects _only_ load balanced connections. All connections made from load
    balancers _must_ be made over this port, and no regular connections may be made over this port.
-   The L4 load balancer _must_ be configured to emit a [proxy protocol][proxy-protocol-url] header
    at the [start of its connection stream](https://github.com/mongodb/mongo/commit/3a18d295d22b377cc7bc4c97bd3b6884d065bb85). `mongos` [supports](https://github.com/mongodb/mongo/commit/786482da93c3e5e58b1c690cb060f00c60864f69) both version 1 and version 2 of the proxy
    protocol standard.
-   The connection string used to establish the `mongos` connection must set the `loadBalanced` option,
    e.g., when connecting to a local `mongos` instance, if the `loadBalancerPort` server parameter was set to 20100, the
    connection string must be of the form `"mongodb://localhost:20100/?loadBalanced=true"`.

`mongos` will emit appropiate error messages on connection attempts if these requirements are not
met.

There are some subtle behavioral differences that these configuration options enable, chief of
which is how `mongos` deals with open cursors on client disconnection. Over a normal connection,
`mongos` will keep open cursors alive for a short while after client disconnection in case the
client reconnects and continues to request more from the given cursor. Since client reconnections
aren't expected behind a load balancer (as the load balancer will likely redirect a given client
to a different `mongos` instance upon reconnection), we eagerly [close cursors](https://github.com/mongodb/mongo/commit/b429d5dda98bbe18ab0851ffd1729d3b57fc8a4e) on load balanced
client disconnects. We also [abort any in-progress transactions](https://github.com/mongodb/mongo/commit/74628ed4e314dfe0fd69d3fbae1411981a869f6b) that were initiated by the load balanced client.

[proxy-protocol-url]: https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt
