# Transport Internals
## Ingress Networking

Ingress networking refers to when a MongoDB process receives incoming requests from a client.

When a client wants to interact with a server, they must first establish a connection. The server listens on a single port for incoming client connections. Once a client connection is received over an ephermeral port, a client session is created out of this connection and commands begin to be processed on a new dedicated thread.

The session takes in commands received from the client and hands them off to a [ServiceEntryPointImpl]. This entry point will spawn threads to perform specific tasks via a [ServiceExecutor], which is then used to initialize a [ServiceStateMachine]. The ServiceStateMachine is a state machine that manages the life cycle of the client connection.

These are the valid state transitions:
* *Source -> SourceWait -> {ProcessStandard,ProcessExhaust,ProcessMoreToCome}*
* *ProcessStandard -> SinkStandard -> SinkWait -> Source (standard RPC)*
* *ProcessExhaust -> SinkMoreToCome-> SinkWait -> ProcessExhaust*
* *ProcessExhaust -> SinkFinal -> SinkWait -> Source*
* *ProcessMoreToCome -> Source*

*Source* - When the server is in the state it requests a new message from the network to handle.

*SourceWait* - This simply the waiting state for that requested message to arrive. The message that is received will also dictate which mode the *Process* state will be transitioned into.

*Process* - The message enters through the [ServiceEntryPoint] and will be run through the database in this state. This state may operate in different modes (*Standard*, *Exhaust*, or *MoreToCome*) which dictate the next state transition. The *MoreToCome* mode denotes that another message should be received before any other action, and so the server returns to the *Source* state when *MoreToCome* mode is seen by the *Process* state on the request message. Meanwhile the *Exhaust* mode indicates that multiple messages must be sent back, and so it will continue to send more response messages until this state stops setting *MoreToCome* mode.

*SinkWait* - Marks the period when the server waits for the database result to be sent over the network This state is usually preceded by a *Sink* state that follows three modes similar to *Process* (*Standard*, *MoreToCome*, *Final*). *Standard* and *Final* will simply proceed to the *SinkWait* stage to await the database result and then return to *Source*. If the *MoreToCome* mode is enabled, however, the server will go on to *ProcessExhaust* following the standard *SinkWait* state. When this happens the server will not transition back to *Source* until a message is received with *MoreToCome* disabled.

*EndSession* - Denotes the end of a session.

Should an error occur throughout the lifecycle, the state will transition to the *EndSession* state.

In order to return the results to the user whether it be a document or a response code, MongoDB uses the [ReplyBuilderInterface]. This interface helps to build message bodies for replying to commands or even returning documents.

This builder interface includes a standard BodyBuilder that builds reply messages in serialized-BSON format.

A Document body builder ([DocSequenceBuilder]) is also defined to help build a reply that can be used to build a response centered around a document.

The various builders supplied in the ReplyBuilderInterface can be appended together to generate responses containing document bodies, error codes, and other appropriate response types.

This interface acts as a cursor to build a response message to be sent out back to the client.

## See Also
For details on egress networking, see [this document][egress_networking].

[ServiceExecutor]: service_executor.h
[ServiceStateMachine]: service_state_machine.h
[ServiceEntryPoint]: service_entry_point.h
[ServiceEntryPointImpl]: service_entry_point_impl.h
[ReplyBuilderInterface]: ../rpc/reply_builder_interface.h
[DocSequenceBuilder]: ../rpc/op_msg.h
[egress_networking]: ../../../docs/egress_networking.md
