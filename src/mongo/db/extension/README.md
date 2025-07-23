# MongoDB Extensions API

This document aims to provide a high-level overview for the MongoDB Extensions API.
An extension is an ahead-of-time compiled object that is dynamically loaded into the server
to provide additional functionality. This object provides a handful of functions the server
may invoke to setup/teardown the extension and register new functionality. Each extension may be
updated independently from the server, meaning that functionality can be added or altered without
building and releasing a new version of the server.

This is a work in progress and more sections will be added gradually.

## Public API

The Extensions API’s primary goal is to provide a header which specifies the data structures and
functions that extension developers must use and implement in order to fully implement an
aggregation stage as an extension. This API header is referred to as the Public API, and can be
under `mongo/db/extensions/public/api.h`. The Public API establishes the contracts and protocol
through which the host can load an extension, make function calls into an extension, and likewise,
how the extension can expect to interface with the host. The Public API will be versioned, vendored
and distributed to extension developers. It is written in C to ensure we maintain a stable ABI.

## Host API

While the Public API defines the building blocks for communicating and interacting between the host
and the extension, its C interface makes it difficult and unsafe for the host code (i.e C++) to
interact with it directly.

The Host API is an adapter layer responsible for creating a safe interface for the C++ host code to
interact with the extension using the C Public API. The host does not need to be aware of any of the
C types that are introduced in the Public API. Instead, the Host API provides C++ classes and
functions which abstract away the complexity and memory ownership concerns of interfacing with the
C API.

The Host API can be found under the `mongo/db/extension/host` directory.

In general, every abstraction in the Public API has a respective C++ interface implemented in the
Host API which the host is expected to use. This allows us to encapsulate and control where
conversions across the API boundary between C and C++ take place, leading to more maintainable code
and minimizing the risk of programmer errors in the host code. The Host API code lives within the
C++ namespace mongo::extension::host.

## SDK API

The SDK API is an adapter layer that is responsible for creating a safe interface for an extension
developer to build an extension in their language of choice, and have it interact with the C Public
API.

The Extensions API initiative will only support Rust extensions in production. The Search team will
own the Rust SDK. However, the Query team develops and maintains a C++ SDK for the purpose of
writing internal unit and integration tests. The C++ SDK API can be found under
`mongo/db/extensions/sdk` directory.

In general, every abstraction in the Public API has a respective C++ interface implemented in the
C++ SDK API which extension developers are expected to use to build their extension. This includes
things like convenience methods, relevant base classes, etc. This allows us to encapsulate and
control where conversions across the API boundary between C and C++ take place, leading to more
maintainable code and minimizing the risk of programmer errors in extension code.

### Important Note: Avoid new SDK dependencies on mongo/base

TODO SERVER-107651 Remove SDK dependency on mongo/base library.

Currently, the SDK relies on the mongo/base library for BSON/BSONObj representation,
as well as DBException and other exception handling functionality. Ideally we should remove that
dependency since it's possible that linking mongo/base in an extension library could cause issues like
host<>extension symbol conflicts at load time or run time. SERVER-107651 tracks the work to remove
that dependency entirely. In the meantime, please avoid adding new usages of that library outside
of BSON representation and exception handling.
