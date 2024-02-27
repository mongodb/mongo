# Transport Internals

## Ingress Networking

Ingress networking refers to a server accepting incoming connections
from MongoDB protocol clients. A client establishes a connection to a
server, issues commands, and receives responses. A server can be
configured to accept connections on several network endpoints.

### Session and ServiceEntryPoint

Once a client connection is accepted, a `Session` object is created to manage
it. This in turn is given to the `ServiceEntryPoint` singleton for the server.
The `ServiceEntryPoint` creates a `SessionWorkflow` for each `Session`, and
maintains a collection of these. The `Session` represents the client side of
that conversation, and the `ServiceEntryPoint` represents the server side.

### SessionWorkflow

While `Session` manages only the transfer of `Messages`, the `SessionWorkflow`
organizes these into a higher layer: the MongoDB protocol. It organizes `Session`
messages into simple request and response sequences represented internally as
`WorkItem` objects. A `SessionWorkflow` can only have one `WorkItem` in
progress at a time.

In the most straightforward case, a `WorkItem` is created by an incoming
message from the `Session`, which is parsed by `SessionWorkflow` and sent to
the `ServiceEntryPoint::handleRequest` function. The `WorkItem` is resolved
by the sending of a response message, and destroyed.

That's the basic case. A few more exotic message transfer styles exist, and
these are also managed by the `SessionWorkflow`. See the section on message
[flag bits][wire_protocol_flag_bits] for details.

A request that can produce multiple responses is an "exhaust" command.
Internally, after each response is sent out, the `SessionWorkflow` synthesizes
a new `WorkItem` from the completed one, and submits this synthetic request to
`ServiceEntryPoint::handleRequest` as it would with a client-initiated request.
In this way, the `SessionWorkflow` keeps the exhaust command going until one
of the responses indicates that it is the last one, or the operation is
interrupted (e.g., due to an error).

A request may also have the "more to come" flag set, so that it
produces no responses. This is known as a "fire and forget" command. This
behavior is also managed by the `SessionWorkflow`.

### Builders

In order to return the results to the user whether it be a document or a response
code, MongoDB uses the [ReplyBuilderInterface]. This interface helps to build
message bodies for replying to commands or even returning documents.

This builder interface includes a standard BodyBuilder that builds reply
messages in serialized-BSON format.

A Document body builder ([DocSequenceBuilder]) is also defined to help build a
reply that can be used to build a response centered around a document.

The various builders supplied in the `ReplyBuilderInterface` can be appended
together to generate responses containing document bodies, error codes, and
other appropriate response types.

This interface acts as a cursor to build a response message to be sent out back
to the client.

## See Also

-   For details on egress networking, see [Egress Networking][egress_networking].
-   For details on command dispatch, see [Command Dispatch][command_dispatch].
-   For details on _NetworkingBaton_ and _AsioNetworkingBaton_, see [Baton][baton].
-   For more detail about `SessionWorkflow`, see WRITING-10398 (internal).

[ServiceExecutor]: service_executor.h
[SessionWorkflow]: session_workflow.h
[ServiceEntryPoint]: service_entry_point.h
[ServiceEntryPointImpl]: service_entry_point_impl.h
[ReplyBuilderInterface]: ../rpc/reply_builder_interface.h
[DocSequenceBuilder]: ../rpc/op_msg.h
[egress_networking]: ../../../docs/egress_networking.md
[command_dispatch]: ../../../docs/command_dispatch.md
[baton]: ../../../docs/baton.md
[wire_protocol_flag_bits]: https://www.mongodb.com/docs/manual/reference/mongodb-wire-protocol/#flag-bits
