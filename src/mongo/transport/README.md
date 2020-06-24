# Transport Layer
The transport layer assists in ingress traffic to the MongoDB server through the use of Generic Sockets.

## Internal Ingress Networking  
Ingress networking refers to when a running MongoDB  instance acts as a server and receives an incoming request from an internal client.

When a client wants to interact with a MongoDB database, they must first establish a connection, typically over TCP. The Transport Layer runs a Listener on a port (27107 by default) to listen for incoming client connections. Once a client connection is received, the Listener creates a session with that client and a thread is spawned for the lifetime of that connection.

The connection lifecycle of a single client connection is handled by a state machine. The session takes in the command received from the client and hands it off to a ServiceExecutor. This ServiceExecutor is then passed in to initialize a ServiceStateMachine, which is a state machine that manages the life cycle of the client connection. Eventually, this ServiceExecutor will spawn a thread to handle the command itself or hand it off to either a ClientOutOfLineExecutor or a TaskExecutor to handle instead. 

In order to return the results to the user whether it be a query or a response code, MongoDB uses the ReplyBuilderInterface. This interface acts as a cursor to build a response message to be sent out back to the client.

#### Code References
* [**ServiceExecutor class**](https://github.com/mongodb/mongo/blob/master/src/mongo/transport/service_executor.h)
* [**ServiceStateMachine class**](https://github.com/mongodb/mongo/blob/master/src/mongo/transport/service_state_machine.h)
* [**ServiceEntryPointImpl class**](https://github.com/mongodb/mongo/blob/master/src/mongo/transport/service_entry_point_impl.h)
* [**ReplyBuilderInterface class**](https://github.com/mongodb/mongo/blob/master/src/mongo/rpc/reply_builder_interface.h)

