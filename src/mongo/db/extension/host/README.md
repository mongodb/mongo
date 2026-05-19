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

- `MongoExtensionAggStageDescriptor` (Static Descriptor)
- `MongoExtensionLogicalAggStage` (Logical Stage)

## Loading Extensions

`loadExtensions()` is the entrypoint for loading extensions during startup (called from
`mongod_main.cpp` and `mongos_main.cpp`). Extensions are only loaded during startup, after the
`parserMap` is initialized with built-in aggregation stages. The core loading logic relies on
`SharedLibrary`, which uses the `dlopen()`/`dlsym()` library functions under the hood to open the
.so file and find the `get_mongodb_extension` symbol.

Once the host has access to the top-level extension logic exposed through `get_mongodb_extension`,
the extension and host perform a version negotiation to agree upon an Extensions API version that is
supported by both modules. Last, the host calls `initialize()` for the extension to register its
aggregation stages through the host portal.

If there are any issues while loading an extension, an error will be logged and startup will fail.
If a node successfully starts up, that means all extensions requested through the `loadExtensions`
startup option were successfully loaded, and we will log a success message.

## Extension Configuration Files

Each extension loaded at startup must have:

1. A `SharedLibrary` file (`*.so`) - the compiled extension
2. A configuration file (`<extensionName>.conf`) - located under `/etc/mongo/extensions`. In test
   environments (when test commands are enabled), the config directory is `/tmp/mongo/extensions`
   instead.

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

## Pipeline Rewrite Rules

Extensions can register pipeline rewrite rules to enable query optimizations. On the host side,
rules are stored in `_extensionRuleRegistry` — a static
`unordered_map<string, vector<PipelineRewriteRule>>` inside `DocumentSourceExtensionOptimizable`,
populated once at startup when `registerStageRules()` is called through the host portal.

### Registering rules

The extension calls `_registerStageRules<StageDescriptor>(portal, rules)` inside `initialize()`:

```cpp
void initialize(const sdk::HostPortalHandle& portal) override {
    _registerStage<MyStageDescriptor>(portal);

    std::vector<PipelineRewriteRule> rules{
        {"absorbMatch",   kPipelineRewriteRuleTagReordering},
        {"inlineProject", kPipelineRewriteRuleTagInPlace},
    };
    _registerStageRules<MyStageDescriptor>(portal, rules);
}
```

This call crosses the C API boundary through the host portal and arrives at `registerStageRules()`
on the host, which inserts the rules into `_extensionRuleRegistry` keyed by stage name. Each rule
has a **name** (used to dispatch to the correct extension override) and a **tag**:

- `kInPlace` — only modifies the stage's internal state; the host routes these through
  `extensionDispatcherInPlacePrecondition`
- `kReordering` — may erase or reorder adjacent pipeline stages; the host routes these through
  `extensionDispatcherReorderingPrecondition`

### Dispatch at optimization time

When a `DocumentSourceExtensionOptimizable` stage is constructed, `_buildOwnedRewriteRules()` wraps
the entries from `_extensionRuleRegistry` for that stage name into host-side
`rule_based_rewrites::pipeline::PipelineRewriteRule` objects (via `wrapExtensionRule()`), caching
them in `_ownedRewriteRules`. The rules must be owned per-stage instance because host-side RBR rules
hold function pointers that close over the specific extension stage object.

During the optimization pass the Rule-Based Rewriter calls one of the two host-side dispatcher
preconditions. Each precondition calls `dispatchExtensionRules(ctx, tagFilter)`, which iterates
`_ownedRewriteRules` and queues any rule whose tag matches the filter with `ctx.addRule()`.

### Precondition and transform callbacks

For each queued rule, the RBR engine calls the extension's `evaluateRulePrecondition`. The call
crosses the C API boundary through the `MongoExtensionLogicalAggStage` vtable, with
`PipelineRewriteContextAdapter` wrapping the host-side context so the extension can safely inspect
adjacent stages via its `PipelineRewriteContextHandle`.

```cpp
bool evaluateRulePrecondition(
    std::string_view ruleName,
    extension::ConstPipelineRewriteContextHandle ctx) const override {
    if (ruleName == "absorbMatch")
        return ctx->hasAtLeastNNextStages(1) &&
               ctx->getNthNextStage(1)->getName() == "$match";
    return false;
}
```

If the precondition returns `true`, the engine calls `evaluateRuleTransform` through the same vtable
path. Returning `true` from the transform signals a structural pipeline modification, causing the
rewriter to re-queue the stage for further optimization passes.

```cpp
bool evaluateRuleTransform(
    std::string_view ruleName,
    extension::PipelineRewriteContextHandle ctx) override {
    if (ruleName == "absorbMatch") {
        BSONObj filter = ctx->getNthNextStage(1)->getFilter();
        // fold filter into this stage's internal state …
        ctx->eraseNthNext(1);
        return true;
    }
    return false;
}
```

See `src/mongo/db/extension/test_examples/desugar/vector_search_optimization.cpp` for a complete
worked example.
