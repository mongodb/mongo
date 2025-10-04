# MongoDB Internals

This document aims to provide a high-level specification for the MongoDB's
infrastructure to support client/server interaction and process globals.
Examples for such components are `ServiceContext` and `OperationContext`.
This is a work in progress and more sections will be added gradually.

## Server-Internal Baton Pattern

For details on the server-internal _Baton_ pattern, see [this document][baton].

[baton]: ../../../docs/baton.md
