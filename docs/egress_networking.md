# Egress Networking

Egress networking entails outbound communication (i.e. requests) from a client process to a server process (e.g. _mongod_), as well as inbound communication (i.e. responses) from such a server process back to a client process.

## Remote Commands

A remote command represents an exchange of data between a client and a server. A remote command consists of two steps: a request, which the clients sends to the server, and a response, which the client receives from the server. These elements are represented by the [request][remote_command_request_h] and [response][remote_command_response_h] objects; each wraps the BSON that represents the on-wire transacted data and metadata that describes the context of the command, such as the host that the command targets. Each object also contains metadata that corresponds to its half of the command lifecycle. For example, the request object notes the timeout of the command and the operation's unique identifier, among other fields, and the response object notes the final disposition of the command's data exchange as a `Status` object (which takes no position on the success of the command's semantics at the remote) and the time that the command actually took to execute, among other fields. In the case of an exhaust command, there may be multiple responses for a single request.

## Connection Pooling

The [executor::ConnectionPool][connection_pool_h] class is responsible for pooling connections to any number of hosts. It contains zero or more `ConnectionPool::SpecificPool` objects, each of which pools connections for a unique host, and exactly one `ConnectionPool::ControllerInterface` object, which is responsible for the addition, removal, and updating of `SpecificPool`s to, from, and in its owning `ConnectionPool`. When a caller requests a connection to a host from the `ConnectionPool`, the `ConnectionPool` creates a new `SpecificPool` to pool connections for that host if one does not exist already, and then the `ConnectionPool` forwards the request to the `SpecificPool`. A `SpecificPool` expires when its `hostTimeout` has passed without any connection requests, after which time it becomes unusable; further requests for connections to that host will trigger the creation of a fresh `SpecificPool`.

The final result of a successful connection request made through `ConnectionPool::getConnection` is a `ConnectionPool::ConnectionInterface`, which represents a connection ready for use. Externally, the `ConnectionInterface` is primarily used by the caller to exchange data with its remote host. Callers return `ConnectionInterface`s to the pool by allowing them to destruct and callers must signal to the pool the final disposition of the connection beforehand through the `indicate*` family of methods. `ConnectionInterface`s also support setting timers to schedule future activities. Internally, the `ConnectionInterface` is used to prepare the connection for data exchange before transferring ownership to the caller and refreshing the health of a connection when the caller returns the connection to the pool. `ConnectionInterface` also maintains a notion of generation, which is implemented as a monotonically-incrementing counter. When a caller returns a `ConnectionInterface` to a `ConnectionPool` from a generation prior to the current generation of the corresponding `SpecificPool`, the connection is dropped. The current generation of a `SpecificPool` is incremented when the pool experiences certain failures (e.g., when to establish a new connection). `ConnectionPool` also drops a connection if the caller called `indicateFailure` on the connection before returning it. `ConnectionPool` uses a global mutex for access to `SpecificPool`s as well as generation counters.

`ConnectionPool` uses its single instance of `EgressConnectionCloserManager` to determine when hosts should be dropped. The manager consists of multiple `EgressConnectionClosers`, which are used to determine whether hosts should be dropped. In the context of the ConnectionPool, the manager's purpose is to drop _connections_ to hosts based on whether they have been marked as keep open or not.

## Internal Network Clients

Client-side outbound communication in egress networking is primarily handled by the [AsyncDBClient class][async_client_h]. The async client is responsible for initializing a connection to a particular host as well as initializing the [wire protocol][wire_protocol] for client-server communication, after which remote requests can be sent by the client and corresponding remote responses from a database can subsequently be received. In setting up the wire protocol, the async client sends an [isMaster][is_master] request to the server and parses the server's isMaster response to ensure that the status of the connection is OK. An initial isMaster request is constructed in the legacy OP_QUERY protocol, so that clients can still communicate with servers that may not support other protocols. The async client also supports client authentication functionality (i.e. authenticating a user's credentials, client host, remote host, etc.).

The scheduling of requests is managed by the [task executor][task_executor_h], which maintains the notion of **events** and **callbacks**. Callbacks represent work (e.g. remote requests) that is to be executed by the executor, and are scheduled by client threads as well as other callbacks. There are several variations of work scheduling methods, which include: immediate scheduling, scheduling no earlier than a specified time, and scheduling iff a specified event has been signalled. These methods return a handle that can be used while the executor is still in scope for either waiting on or cancelling the scheduled callback in question. If a scheduled callback is cancelled, it remains on the work queue and is technically still run, but is labeled as having been 'cancelled' beforehand. Once a given callback/request is scheduled, the task executor is then able to execute such requests via a [network interface][network_interface_h]. The network interface, connected to a particular host/server, begins the asynchronous execution of commands specified via a request bundled in the aforementioned callback handle. The interface is capable of blocking threads until its associated task executor has work that needs to be performed, and is likewise able to return from an idle state when it receives a signal that the executor has new work to process.

Client-side legacy networking draws upon the `DBClientBase` class, of which there are multiple subclasses residing in the `src/mongo/client` folder. The [replica set DBClient][dbclient_rs_h] discerns which one of multiple servers in a replica set is the primary at construction time, and establishes a connection (using the `DBClientConnection` wrapper class, also extended from `DBClientBase`) with the replica set via the primary. In cases where the primary server is unresponsive within a specified time range, the RS DBClient will automatically attempt to establish a secondary server as the new primary (see [automatic failover][automatic_failover]).

## See Also

For details on transport internals, including ingress networking, see [this
document][transport_internals].

[remote_command_request_h]: ../src/mongo/executor/remote_command_request.h
[remote_command_response_h]: ../src/mongo/executor/remote_command_response.h
[connection_pool]: https://en.wikipedia.org/wiki/Connection_pool
[connection_pool_h]: ../src/mongo/executor/connection_pool.h
[session_h]: ../src/mongo/transport/session.h
[async_client_h]: ../src/mongo/client/async_client.h
[is_master]: https://docs.mongodb.com/manual/reference/command/isMaster/
[wire_protocol]: https://docs.mongodb.com/manual/reference/mongodb-wire-protocol/
[task_executor_h]: ../src/mongo/executor/task_executor.h
[network_interface_h]: ../src/mongo/executor/network_interface.h
[dbclient_rs_h]: ../src/mongo/client/dbclient_rs.h
[automatic_failover]: https://docs.mongodb.com/manual/replication/#automatic-failover
[transport_internals]: ../src/mongo/transport/README.md
