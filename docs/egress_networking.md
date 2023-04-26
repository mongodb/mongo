# Egress Networking

Egress networking entails outbound communication (i.e. requests) from a client process to a server process (e.g. *mongod*), as well as inbound communication (i.e. responses) from such a server process back to a client process.

## Remote Commands

Remote commands represent the "packages" in which data is transmitted via egress networking. There are two types of remote commands: requests and responses. The [request object][remote_command_request_h] is in essence a wrapper for a command in BSON format, that is to be delivered to and executed by a remote MongoDB node against a database specified by a member in the object. The [response object][remote_command_response_h], in turn, contains data that describes the response to a previously sent request, also in BSON format. Besides the actual response data, the response object also stores useful information such as the duration of running the command specified in the corresponding request, as well as a `Status` member that indicates whether the operation was a success, and the cause of error if not. 

There are two variants of both the request and response classes that are used in egress networking. The distinction between the `RemoteCommandRequest` and `RemoteCommandRequestOnAny` classes is that the former specifies a particular host/server to connect to, whereas the latter houses a vector of hosts, for when a command may be run on multiple nodes in a replica set. The distinction between `RemoteCommandResponse` and `RemoteCommandOnAnyResponse` is that the latter includes additional information as to what host the originating request was ultimately run on. It should be noted that the distinctions between the request and response classes are characteristically different; that is to say, whereas the *OnAny* variant of the request object is a augmented version of the other, the response classes should be understood as being different return types altogether.

## Connection Pooling

[Connection pooling][connection_pool] is largely taken care of by the [executor::connection_pool][connection_pool_h] class. This class houses a collection of `ConnectionPool::SpecificPool` objects, each of which shares a one-to-one mapping with a unique host. This lends itself to a parent-child relationship between a "parent" ConnectionPool and its constituent "children" SpecificPool members. The `ConnectionPool::ControllerInterface` subclass is used to direct the behavior of the SpecificPools that belong to it. The main operations associated with the ControllerInterface are the addition, removal, and updating of hosts (and thereby corresponding SpecificPools) to/from/in the parent pool. SpecificPools are created when a connection to a new host is requested, and expire when `hostTimeout` has passed without there having been any new requests or checked-out connections (i.e. connections in use). A pool can have its expiration status lifted whenever a connection is requested, but once a pool is shutdown, the pool becomes unusable. The `hostTimeout` field is one of many parameters belonging to the `ConnectionPool::Options` struct that determines how pools operate. 

The `ConnectionPool::ConnectionInterface` is responsible for handling the connections *within* a pool. The ConnectionInterface's operations include, but are not limited to, connection setup (establishing a connection, authenticating, etc.), refreshing connections, and managing a timer. This interface also maintains the notion of a pool/connection **generation**, which is used to identify whether some particular connection's generation is older than that of the pool it belongs to (i.e. the connection is out-of-date), in which case it is dropped. The ConnectionPool uses a global mutex for access to SpecificPools as well as generation counters. Another component of the ConnectionPool is its `EgressTagCloserManager`. The manager consists of multiple `EgressTagClosers`, which are used to determine whether hosts should be dropped based on their tags [(see transport/session.h)][session_h]. In the context of the ConnectionPool, the manager's purpose is to drop *connections* to hosts based on whether their tags do or do not match those of the manager.

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
