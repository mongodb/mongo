# VersionContext in MongoDB

This document details the `VersionContext`, a crucial component for managing Feature Compatibility
Version (FCV) during DDL operations in the MongoDB codebase.

## What is VersionContext?

The Operation Feature Compatibility Version (OFCV) is an immutable snapshot of the FCV associated
with a given operation. The `VersionContext` serves as an indirection to store and access this OFCV,
making the code more flexible for future needs. It is defined as a decoration of the
`OperationContext` class.

> Please Note: The VersionContext is only available for use for DDL operations. SPM-4227 plans to
> extend this concept to more operations (see [warnings section](#warnings-and-future-outlook)).

## How to Use VersionContext

The `VersionContext` provides the following methods:

- `VersionContext::setOperationFCV(FCV fcv)`: Takes a snapshot of the given FCV and attaches it to
  the `VersionContext`.

- `VersionContext::setOperationFCV(FCVSnapshot fcv)`: Attaches a pre-existing `FCVSnapshot` to the
  `VersionContext`.

- `optional<FCVSnapshot> VersionContext::getOperationFCV()`: Returns the snapshot corresponding to
  the attached OFCV. It returns `boost::none` if no OFCV value is associated. Access to this method is
  restricted to the `FCVGatedFeatureFlag` class only.

### Initialization, Persistence, and Recovery

The OFCV for a DDL operation is created within the
`ShardingDDLCoordinatorService::getOrCreateInstance` function, where the node’s local FCV is
captured and set on a `VersionContext` instance. This value is then passed to a spawned DDL
coordinator through a specific field within the coordinator state document, extending the
`ForwardableOperationMetadata` definition to ensure propagation between subsequent coordinator
phases. This mechanism ensures persistence across various stages and allows for recovery in case of
crashes and failovers.

### Propagation

Coordinators retrieve the `VersionContext` value from the state document upon startup. To maintain a
consistent `VersionContext` across all phases of a coordinator, it is made part of the
`ForwardableOperationMetadata`. The `ForwardableOperationMetadata::setOn(opCtx)` method is extended
to attach the `VersionContext` to the operation context.

The `VersionContext` is implicitly propagated across the network to DDL participants by extending
the `GenericArguments` with an optional `versionContext` field. Helper functions `setVersionContext`
(on the coordinator) and `getVersionContext` (on the participants) are introduced in
`generic_argument_util` to manage this propagation. An `rpc::EgressMetadataHook` is used to transmit
the `VersionContext` as a generic command argument.

### Replication

The `VersionContext` is replicated to all `kCommand` oplog entries to ensure a consistent FCV is
used by primaries and secondaries when applying related oplog entries.

### Feature Flag Checks

The OFCV value in the `VersionContext` serves as the baseline for all feature flag checks during the
execution of DDL coordinators and participants. FCV-gated feature flags (represented by
`FCVGatedFeatureFlag`) will use an `isEnabled` method that incorporates a `VersionContext` parameter
to accommodate OFCV-based checks. Non-FCV-gated feature flags (represented by
`BinaryCompatibleFeatureFlag`) will use a parameterless `isEnabled()` method.

## Warnings and Future Outlook

Currently, the OFCV is primarily applied to DDL operations. Non-DDL operations can run across FCV
change boundaries, which requires careful handling. A transitional API is in place for FCV-gated
feature flag checks, relying on `FCVSnapshot` if no OFCV is available.

Future work aims to enable non-DDL operations to:

1. Store an FCV snapshot (OFCV) on the `VersionContext`.
2. Check that the FCV has not transitioned when acquiring a write lock or `FixedFCVRegion`.
3. If the FCV has transitioned, kill the operation or allow callers to decide the resolution.

Once these capabilities are introduced for non-DDL operations, the feature flag API can be
streamlined to directly use `VersionContext::getDecoration(opCtx)` for
`FCVGatedFeatureFlag::isEnabled` calls.

## Multiversion Considerations

Changes related to `VersionContext` are controlled by a `SnapshotFCVInDDLCoordinators` feature flag,
enabled with FCV \>= 9.0. This flag determines if the OFCV is set upon DDL coordinator startup. DDL
coordinators and participants will then distinguish between old and new behaviors based on the OFCV
value itself:

- **OFCV == None**: Fall back on the current FCV behavior.
- **OFCV \!= None**: Behave according to the OFCV.

This approach ensures consistent behavior during FCV 8.0 / FCV 9.0 transitions and when mixed
binaries are involved.

## Diagnosis/Debuggability

Information about the associated OFCV will be introduced to `config.changelog` logs for each DDL
operation and to general sharding logs for DDL startup, participant requests, and other FCV-related
cases. The OFCV for a given operation will also be added to the output of the `currentOp` command.
