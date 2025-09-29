# Extension Host

This directory holds all extension logic that hooks extensions into the rest of the server.

## DocumentSourceExtension

Aggregation stages are modelled and implemented within the server as `DocumentSource`
specializations. The Host API introduces `DocumentSourceExtension`, a specialization of the
`DocumentSource` abstraction, which acts as a façade and delegates its business logic to the
Extensions API.

All the methods offered by the `DocumentSource` interface are implemented by modelling a
comprehensive set of interfaces in the API which the extension must service.

`DocumentSourceExtension` delegates calls to its interface to the following Public API components:

- `MongoExtensionAggregationStageDescriptor` (Static Descriptor)
- `MongoExtensionLogicalAggregationStage` (Logical Stage)

## Loading Extensions

`loadExtensions()` is the entrypoint for loading extensions during startup (called from
`mongod_main.cpp` and `mongos_main.cpp`). Extensions are only loaded during startup, after the
`parserMap` is initialized with built-in aggregation stages. The core loading logic relies on
`SharedLibrary`, which uses the `dlopen()`/`dlsym()` library functions under the hood to open the
.so file and find the `get_mongodb_extension` symbol.

Once the host has access to the top-level extension logic exposed through `get_mongodb_extension`,
the extension and host perform a version negotiation to agree upon an Extensions API version that
is supported by both modules. Last, the host calls `initialize()` for the extension to register its
aggregation stages through the host portal.

If there are any issues while loading an extension, an error will be logged and startup will fail.
If a node successfully starts up, that means all extensions requested through the `loadExtensions`
startup option were successfully loaded, and we will log a success message.

## Extension Configuration Files

Each extension loaded at startup must have:

1. A `SharedLibrary` file (`*.so`) - the compiled extension
2. A configuration file (`<extensionName>.conf`) - located under `/etc/mongo/extensions`.

Configuration files use YAML syntax and must define:

1. `sharedLibraryPath`: The path to the extensions `SharedLibrary` (`.so`) file.
2. `extensionOptions`: Key-value pairs of extension-specific options, passed to the extension during
   initialization via the `MongoExtensionHostPortal`.

For example:

```yml
# /etc/mongo/extensions/toaster.conf
sharedLibraryPath: /path/to/toaster.so
extensionOptions:
  maxHeat: 5
  allowBagels: true
```

At startup, the host will first load a `.conf` file, then use its `sharedLibraryPath` to access and
load the `SharedLibrary` file that represents the extension and pass the corresponding
`extensionOptions` to its initialization function.
