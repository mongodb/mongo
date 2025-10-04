# Intent Registration Architecture Guide

## Motivation Behind Intent Registration

Intent registration aims to reduce deadlocks during state transitions and simplify the management of concurrency between state transitions and other ongoing operations. It achieves this by using non-blocking calls to grant or reject read or write operation intents and eliminates the need for engineers to follow complex rules to acquire locks in a certain order. This approach also simplifies and modularizes the codebase, allowing higher layers to directly determine their read/write capabilities without needing to check the node's state for permissible actions.

## Intent Types and Their Use Cases

- `Read`: Provides read-only access to database contents and requires interruption in case of rollback or shutdown.
- `Write`: Provides write access to the database and requires interruption in case of stepdown, rollback, or shutdown.
- `LocalWrite`: Provides write access exclusively to the unreplicated subset of a database and requires interruption in case of rollback or shutdown.
- `BlockingWrite`: Intended solely for prepared transactions and index coordinators, this operation blocks state transitions until completion. This is achieved by invoking [`waitForDrain` for BlockingWrite](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/repl/intent_registry.cpp#L196) in `killConflictingOperations` to ensure these operations are not interrupted during state transitions such as step-up.

## Registering and Deregistering Intents

Intent registration typically occurs implicitly when the `GlobalLock` or `DBLock` is acquired. The `GlobalLock` determines the appropriate intent type by evaluating the lock type requested (within [`_declareIntent`](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/concurrency/d_concurrency.cpp#L154-L171)). Alternatively, an explicitIntent can be specified within `GlobalLockOptions`. However, it's recommended to allow the intent registry to determine the implicit intent; the section [Preferring implicit intent over explicit intent](#preferring-implicit-intent-over-explicit-intent) provides further details on this recommendation. Once the intentType is established, an `IntentGuard` is created, which then invokes [`IntentRegistry::registerIntent`](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/concurrency/d_concurrency.cpp#L154-L171). This invocation first conducts compatibility checks:

- No intent is compatible with shutdown. `LocalWrite` is allowed during rollback. `Read`, `LocalWrite`, and `BlockingWrite` are permitted during stepdown.
- If no interruptions are occurring, all intent types are allowed.
- For `Write` intents, the intent registry verifies that the node is primary.
- For `BlockingWrite`, the intent registry waits for the current state transition to finish killing operations.

If any check fails, an `uassert` is triggered, requiring you to handle the retry.

Upon successful checks, the intent registry grants the intent and adds it to the [`tokenMap`](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/repl/intent_registry.h#L192-L196), which maps the `tokenId` to `opCtx`.

Intents can also be registered using `IntentGuard` or `WriteIntentGuard`, with the latter being a wrapper for the `Write` intent type (both defined [here](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/repl/intent_guard.h)). Both internally [call](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/repl/intent_guard.cpp#L35) the `registerIntent` function. This approach is primarily used by the index coordinator and for prepared transactions, where a `BlockingWrite` intent ensures the operations are not interrupted by state transitions. It's also used when an operation declares a `WriteIntentGuard` so that it can maintain the node's primary state and make sure it will be interrupted if a state transition occurs.

Like the `GlobalLock`, `IntentGuard` is also RAII type, and upon destructing it will call `IntentRegistry::deregisterIntent`. This simply involves [removing](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/repl/intent_registry.cpp#L151-L159) the intent's intentId from the tokenMap.

## State Transitions

When a state transition occurs, the state transition thread calls [`killConflictingOperations`](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/repl/intent_registry.cpp#L187) with an `interruptType` (e.g., StepUp, StepDown, Rollback, Shutdown, or None). Before proceeding with killing operations, intent registry ensures that all operations that have a `BlockingWrite` intent have completed, as these operations are designed to prevent state transitions until they are finished. It also waits for any previous state transitions (`_interruptionCtx`) to conclude. For each intent type incompatible with the current interrupt, the system waits for all registered intents of that type to drain, with a defined timeout for this waiting period.

## Using the Intent Registration API

### Preferring Implicit Intent over Explicit Intent

When declaring intents, the system first attempts to implicitly determine the intent type based on the requested lock type and the database. If the database is a local database or the operation does not generate an oplog entry (e.g. the write is not replicated), then the implicit intent will be a `LocalWrite` if the requested global lock type is `IX` or `X` (see [here](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/concurrency/d_concurrency.cpp#L252-L254)). If the write is not an unreplicated write, intent registration determines the implicit intent based on just the requested lock type. If the lock type is `IS` or `S`, the implicit intent is `Read`. If it's `IX` or `X`, the implicit intent is `Write` (see [\_declareIntent](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/concurrency/d_concurrency.cpp#L154-L171)).

It's important to note the following:

- Using `Write` when `LocalWrite` is the correct intent type will result in a loud failure, with a `WritablePrimary` uassert error.
- Using `LocalWrite` when `Write` is the correct intent type will result in a silent failure, which is a problematic outcome.

Because of that, it is better to allow intent registry to use the implicit intent type first, and if the implicit intent type is not suitable, an explicit intent can be provided through `GlobalLockOptions`.

### Checking or Maintaining Primary State

To check if a node is the primary, use [`canDeclareIntent`](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/repl/intent_registry.cpp#L161-L185). However, be aware that the node's state might change immediately after this call, as `canDeclareIntent` acts as a wrapper for `canAcceptWritesFor_UNSAFE`.

For operations that require the node to remain primary, declare a `Write` intent.

Additionally, to ensure an operation is interrupted if the node steps down, declare a `Write` intent. This is preferred over using `setAlwaysInterruptAtStepDownOrUp_UNSAFE`. Note that interrupting `Write` intents on stepup is currently not supported because secondaries cannot declare `Write` intents (it will fail with a `WritablePrimary` error) so no operations on a secondary will be interrupted on stepup.

### Handling Rejected Intents

Acquiring the RSTL was a blocking operation; however, declaring an intent is non-blocking. This means an intent is either granted or rejected, and engineers must handle both outcomes.

If an intent is rejected, a uassert is triggered, which needs to be handled to prevent it from propagating back to the user.

- For user commands, no special handling is required as the driver will automatically retry.
- Internal threads, however, need additional consideration.
- Declaring intents around state transitions can be tricky, since the intents could be rejected due to the state transition happening. A retry loop and logAndBackoff is one way to handle it (an example of that is [here](https://github.com/mongodb/mongo/blob/v8.2/src/mongo/db/repl/oplog_applier_batcher.cpp#L394-L401)).
