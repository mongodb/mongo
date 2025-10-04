# MongoDB Extensions Public API

## Implementing Polymorphism Across API Boundary

The API aims to provide flexibility to extension developers in choosing how an implementation
looks on the extension side of the API boundary. To this end, we provide an additional layer of
indirection when defining the data types that comprise this API, which allows us to hide data
members of API objects and implementation details entirely on the extension side of the API
boundary.

We achieve this by implementing polymorphism in the C API, such that the vast majority of the
data structures that cross the API boundary only hold a single member: a pointer to a
virtual table that represents the common interface for the polymorphic type.

For example, `APIStruct` below only has a single member, a pointer to `APIStructVTable`, which
requires that an extension implements the `foo` function, and assign it to the function pointer in
the `APIStructVTable` provided to the instantiation of an `APIStruct`.

```
// Note: APIStruct is not part of the API, just an example.
extern "C" {
    // public API:
    typedef APIStructVTable {
        void (*foo)();
    } APIStructVTable;

    typedef APIStruct {
        const APIStructVTable* vtable;
    } APIStruct;
}
```

## Memory Ownership of Extension-Allocated Objects

Function calls of this API will at times pass extension-allocated data structures across the API
boundary back to the host.

When passing data structures across the API boundary, it is imperative that memory is deallocated
using the same mechanism by which it was originally allocated. Memory allocated by the extension
must be deallocated by the extension, and memory allocated by the host must be deallocated by the
host.

For this reason, when allocated memory is passed across the API boundary with the intention of
transferring ownership to the caller, it must be done so via an interface that offers the
functionality required to delegate the deallocation back to the original allocation context.

This API adopts the convention that all data structures that intend to transfer ownership to
the caller must provide a `destroy()` function pointer in their interface, as shown in the example
below:

```
extern C {
   // Note: Destroyable is not part of the API, just an example.
   typedef struct DestroyableVTable {
        /**
         * destroy `ptr` and free any related resources.
        */
        void (*destroy)(Destroyable* ptr);
   } DestroyableVTable;

   typedef struct Destroyable {
        const DestroyableVTable* vtable;
   } Destroyable;
}
```

In the Extensions API, the presence of a `destroy()` function in an interface indicates that the
type is associated with long lived memory whose ownership can be transferred across the API
boundary (i.e. from the extension to the host). It is important to note that when a function intends
to transfer ownership across the boundary, it must be explicitly stated and made clear in the
function’s documentation.

## MongoExtensionStatus

Exceptions must never cross beyond the extension’s API boundary. This means that extension
developers must guarantee that no exceptions escape from the extension, and any such exceptions
must be converted to errors that can be passed across the boundary and interpreted by the host.

For the most part, this API adopts a convention that all function calls across the API boundary must
return a `MongoExtensionStatus` which will inform the caller whether the API call was successful or
not. A zero error code indicates success, while a non-zero error code indicates an error during
the function execution. `MongoExtensionStatus` is a long-lived allocated object, since it needs to
to provide additional error information in the failure case.

Note that when a `MongoExtensionStatus` is returned by a function call, ownership is always
transferred to the caller of the function. Once the error is no longer needed by the caller,
its deallocation must be delegated to the other side of the API boundary.

## MongoExtensionAggregationStageDescriptor

A `MongoExtensionAggregationStageDescriptor` describes features of an aggregation stage that are
not bound to the stage definition. This object functions as a factory to create a logical stage
through parsing. Note, that a `MongoExtensionAggregationStageDescriptor` is always fully owned by
the extension, and is expected to remain valid for the entire time an extension is loaded.

## MongoExtensionLogicalAggregationStage

A `MongoExtensionLogicalAggregationStage` describes a stage that has been parsed and bound to
instance specific context -- the stage definition and other context data from the pipeline.
These objects are suitable for pipeline optimization. Once optimization is complete they can
be used to generate objects for execution.

## Extension Initialization

All extensions must expose a `get_mongodb_extension` function symbol as the top-level extension
entrypoint for the host to facilitate extension loading and initialization. The `MongoExtension`
initialization function receives a pointer to the `MongoExtensionHostPortal`, which provides a
function `registerStageDescriptor`. Each aggregation stage you wish to load as part of an extension
must be registered during initialization.
