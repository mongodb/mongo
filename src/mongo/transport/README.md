# Transport Internals
## Ingress Networking

Ingress networking refers to when a MongoDB process receives incoming requests 
from a client.

When a client wants to interact with a server, they must first establish a 
connection. The server listens on a single port for incoming client connections. 
Once a client connection is received, a client session is created out of this
connection and commands begin to be processed on a dedicated thread, which
can be either new or reused from a previous ingress connection. Each ingress
connection will be granted temporary but exclusive lease of a worker thread.

The session accepts commands from the client and hands them off to a 
[ServiceEntryPointImpl]. This entry point will spawn threads to perform specific 
tasks via a [ServiceExecutor], which is then used to initialize a 
[SessionWorkflow]. The `SessionWorkflow` manages the life cycle of the client
connection, organizing its traffic to simple request and response sequences
represented internally as `WorkItem` objects. Only one `WorkItem` can be in
progress at a time.

A `WorkItem` starts with a request message, and may be associated with 0 or
more response messages.

Normally, a `WorkItem` is created by an incoming request
and is resolved by the sending of a response.

A request that produces multiple responses is an "exhaust" command, and
after each response is sent, a new `WorkItem` is synthesized from the
completed one.

A "more to come" request is similar to an exhaust command but requires the user to
issue `getMore` commands to pull responses, as opposed to "exhaust" which generates
responses spontaneously.

A request that produces no responses is a "fire and forget" command.

In order to return the results to the user whether it be a document or a response 
code, MongoDB uses the [ReplyBuilderInterface]. This interface helps to build 
message bodies for replying to commands or even returning documents.

This builder interface includes a standard BodyBuilder that builds reply 
messages in serialized-BSON format.

A Document body builder ([DocSequenceBuilder]) is also defined to help build a 
reply that can be used to build a response centered around a document.

The various builders supplied in the ReplyBuilderInterface can be appended 
together to generate responses containing document bodies, error codes, and 
other appropriate response types.

This interface acts as a cursor to build a response message to be sent out back 
to the client.

## See Also
For details on egress networking, see [Egress Networking][egress_networking]. For 
details on command dispatch, see [Command Dispatch][command_dispatch]. For details 
on *NetworkingBaton* and *AsioNetworkingBaton*, see [Baton][baton].

For more detail about `SessionWorkflow`, see the [design][session_workflow_design].

[ServiceExecutor]: service_executor.h
[SessionWorkflow]: session_workflow.h
[ServiceEntryPoint]: service_entry_point.h
[ServiceEntryPointImpl]: service_entry_point_impl.h
[ReplyBuilderInterface]: ../rpc/reply_builder_interface.h
[DocSequenceBuilder]: ../rpc/op_msg.h
[egress_networking]: ../../../docs/egress_networking.md
[command_dispatch]: ../../../docs/command_dispatch.md
[baton]: ../../../docs/baton.md
[session_workflow_design]: https://docs.google.com/document/d/1CKna4BEFyOj_NAM7i2dL7xCDcTs3RQzsu4KN6p5GVuM
