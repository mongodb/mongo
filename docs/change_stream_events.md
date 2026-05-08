# MongoDB Change Stream Event Types — Comprehensive Reference

## Configuration Flags

The following flags can be passed to `$changeStream` to control which events appear:

| Flag                     | Type   | Default   | Notes                                                                                                                        |
| ------------------------ | ------ | --------- | ---------------------------------------------------------------------------------------------------------------------------- |
| fullDocument             | string | "default" | "default", "updateLookup", "whenAvailable", "required"                                                                       |
| fullDocumentBeforeChange | string | "off"     | "off", "whenAvailable", "required"                                                                                           |
| showExpandedEvents       | bool   | false     | Enables DDL events and additional fields; not permitted with API strict version 1                                            |
| showSystemEvents         | bool   | false     | Enables events on system collections; not permitted with API strict version 1                                                |
| showMigrationEvents      | bool   | false     | Allows chunk migration writes to appear; only valid on direct shard connections, rejected by mongos (error 31123)            |
| showRawUpdateDescription | bool   | false     | (Internal) Returns raw oplog update description instead of parsed updateDescription; not permitted with API strict version 1 |
| showCommitTimestamp      | bool   | false     | (Internal) Adds commitTimestamp to CRUD events inside prepared transactions                                                  |

## Event Type Reference

### Group 1 — Classic CRUD Events (always visible)

These are always returned regardless of configuration flags.

#### `insert`

- Triggered by: Document insertion (direct insert, or implicit collection creation + insert).
- Topologies: All (standalone not supported by change streams; replica set, sharded cluster).

Fields:

| Field           | Type              | Always Present                                                             | Notes                                                    |
| --------------- | ----------------- | -------------------------------------------------------------------------- | -------------------------------------------------------- |
| \_id            | Object            | Yes                                                                        | Resume token                                             |
| operationType   | String            | Yes                                                                        | "insert"                                                 |
| clusterTime     | Timestamp         | Yes                                                                        | Logical cluster time                                     |
| wallTime        | Date              | Yes                                                                        | Wall clock time                                          |
| ns              | Object {db, coll} | Yes                                                                        | Namespace                                                |
| documentKey     | Object            | Yes                                                                        | {\_id, ...shardKey} — identifying fields of the document |
| fullDocument    | Object            | Yes                                                                        | The inserted document                                    |
| collectionUUID  | UUID              | Only if showExpandedEvents=true                                            | UUID of the collection                                   |
| commitTimestamp | Timestamp         | Only if showCommitTimestamp=true and in a prepared transaction             | Commit timestamp                                         |
| fromMigrate     | Boolean (true)    | Only if showMigrationEvents=true and event originated from chunk migration | Migration marker                                         |
| lsid            | Object            | Only inside a transaction                                                  | Logical session ID                                       |
| txnNumber       | Long              | Only inside a transaction                                                  | Transaction number                                       |

#### `update`

- Triggered by: Document update using $set, $unset, delta update operators, etc. (non-replacement).
  Detected by the absence of \_id in the oplog o field combined with oplog version $v: 2 (delta
  format).

Fields:

| Field                    | Type              | Always Present                                               | Notes                                                                                                           |
| ------------------------ | ----------------- | ------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------- |
| \_id                     | Object            | Yes                                                          | Resume token                                                                                                    |
| operationType            | String            | Yes                                                          | "update"                                                                                                        |
| clusterTime              | Timestamp         | Yes                                                          |                                                                                                                 |
| wallTime                 | Date              | Yes                                                          |                                                                                                                 |
| ns                       | Object {db, coll} | Yes                                                          |                                                                                                                 |
| documentKey              | Object            | Yes                                                          | {\_id, ...shardKey}                                                                                             |
| updateDescription        | Object            | Yes (unless showRawUpdateDescription=true)                   | {updatedFields, removedFields, truncatedArrays}. When showExpandedEvents=true also includes disambiguatedPaths. |
| rawUpdateDescription     | Object            | Only if showRawUpdateDescription=true                        | Raw oplog delta; replaces updateDescription                                                                     |
| fullDocument             | Object            | Only if fullDocument=updateLookup/whenAvailable/required     | Current state of document (looked up post-update)                                                               |
| fullDocumentBeforeChange | Object            | Only if fullDocumentBeforeChange=whenAvailable/required      | Pre-image (requires pre-image collection enabled on the collection)                                             |
| collectionUUID           | UUID              | Only if showExpandedEvents=true                              |                                                                                                                 |
| commitTimestamp          | Timestamp         | Only if showCommitTimestamp=true and in prepared transaction |                                                                                                                 |
| fromMigrate              | Boolean           | Only if showMigrationEvents=true and from migration          |                                                                                                                 |
| lsid                     | Object            | Only inside transaction                                      |                                                                                                                 |
| txnNumber                | Long              | Only inside transaction                                      |                                                                                                                 |

#### `replace`

- Triggered by: Full document replacement (e.g., replaceOne, or update with a replacement document
  containing \_id). Detected by the presence of \_id in the oplog o field for an update oplog entry.

Fields:

| Field                    | Type              | Always Present                                            | Notes                 |
| ------------------------ | ----------------- | --------------------------------------------------------- | --------------------- |
| \_id                     | Object            | Yes                                                       | Resume token          |
| operationType            | String            | Yes                                                       | "replace"             |
| clusterTime              | Timestamp         | Yes                                                       |                       |
| wallTime                 | Date              | Yes                                                       |                       |
| ns                       | Object {db, coll} | Yes                                                       |                       |
| documentKey              | Object            | Yes                                                       | {\_id, ...shardKey}   |
| fullDocument             | Object            | Yes                                                       | The new full document |
| fullDocumentBeforeChange | Object            | Only if fullDocumentBeforeChange=whenAvailable/required   | Pre-image             |
| collectionUUID           | UUID              | Only if showExpandedEvents=true                           |                       |
| commitTimestamp          | Timestamp         | Only if showCommitTimestamp=true and prepared transaction |                       |
| fromMigrate              | Boolean           | Only if showMigrationEvents=true and from migration       |                       |
| lsid                     | Object            | Only inside transaction                                   |                       |
| txnNumber                | Long              | Only inside transaction                                   |                       |

#### `delete`

- Triggered by: Document deletion.

Fields:

| Field                    | Type              | Always Present                                            | Notes                                           |
| ------------------------ | ----------------- | --------------------------------------------------------- | ----------------------------------------------- |
| \_id                     | Object            | Yes                                                       | Resume token                                    |
| operationType            | String            | Yes                                                       | "delete"                                        |
| clusterTime              | Timestamp         | Yes                                                       |                                                 |
| wallTime                 | Date              | Yes                                                       |                                                 |
| ns                       | Object {db, coll} | Yes                                                       |                                                 |
| documentKey              | Object            | Yes                                                       | {\_id, ...shardKey} (document no longer exists) |
| fullDocumentBeforeChange | Object            | Only if fullDocumentBeforeChange=whenAvailable/required   | Pre-image                                       |
| collectionUUID           | UUID              | Only if showExpandedEvents=true                           |                                                 |
| commitTimestamp          | Timestamp         | Only if showCommitTimestamp=true and prepared transaction |                                                 |
| fromMigrate              | Boolean           | Only if showMigrationEvents=true and from migration       |                                                 |
| lsid                     | Object            | Only inside transaction                                   |                                                 |
| txnNumber                | Long              | Only inside transaction                                   |                                                 |

### Group 2 — Classic DDL Events (always visible)

#### `drop`

- Triggered by: Collection drop (db.coll.drop() or drop command). Also generated (as a view drop
  event) for deletion from system.views when showExpandedEvents=true on a whole-db or whole-cluster
  stream.
- Invalidation effect: Invalidates single-collection streams watching the dropped collection.

Fields:

| Field          | Type              | Always Present                  | Notes        |
| -------------- | ----------------- | ------------------------------- | ------------ |
| \_id           | Object            | Yes                             | Resume token |
| operationType  | String            | Yes                             | "drop"       |
| clusterTime    | Timestamp         | Yes                             |              |
| wallTime       | Date              | Yes                             |              |
| ns             | Object {db, coll} | Yes                             |              |
| collectionUUID | UUID              | Only if showExpandedEvents=true |              |

#### `rename`

- Triggered by: Collection rename (renameCollection command). Also emitted for internal timeseries
  collection upgrades/downgrades (upgradeDowngradeViewlessTimeseries oplog command).
- Invalidation effect: Invalidates single-collection streams watching either the source or target
  namespace.

Fields:

| Field                | Type              | Always Present                  | Notes                                                                                                       |
| -------------------- | ----------------- | ------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| \_id                 | Object            | Yes                             | Resume token                                                                                                |
| operationType        | String            | Yes                             | "rename"                                                                                                    |
| clusterTime          | Timestamp         | Yes                             |                                                                                                             |
| wallTime             | Date              | Yes                             |                                                                                                             |
| ns                   | Object {db, coll} | Yes                             | Source namespace                                                                                            |
| to                   | Object {db, coll} | Yes                             | Target namespace (kept for backwards compatibility)                                                         |
| operationDescription | Object            | Only if showExpandedEvents=true | {to: {db, coll}, dropTarget: UUID} — dropTarget is present when the rename overwrote an existing collection |
| collectionUUID       | UUID              | Only if showExpandedEvents=true | UUID of the source collection                                                                               |

#### `dropDatabase`

- Triggered by: Database drop command. Appears on whole-db and whole-cluster streams. Excluded from
  single-collection streams' oplog filter (a single-collection stream is invalidated before reaching
  this event; dropDatabase is omitted from the filter to avoid showing it after startAfter
  resumption from the invalidate).
- Invalidation effect: Invalidates whole-database streams.
- Topologies: Whole-db and whole-cluster change streams only.

Fields:

| Field         | Type        | Always Present | Notes            |
| ------------- | ----------- | -------------- | ---------------- |
| \_id          | Object      | Yes            | Resume token     |
| operationType | String      | Yes            | "dropDatabase"   |
| clusterTime   | Timestamp   | Yes            |                  |
| wallTime      | Date        | Yes            |                  |
| ns            | Object {db} | Yes            | Only db, no coll |

#### `invalidate`

- Triggered by: An invalidating operation — specifically:
  - For single-collection streams: drop, rename (both as source and target), or
    upgradeDowngradeViewlessTimeseries affecting the watched collection.
  - For whole-database streams: dropDatabase.
  - Whole-cluster streams are never invalidated.

The invalidate event is generated by the $\_internalChangeStreamCheckInvalidate stage after the
invalidating event is emitted. After an invalidate event the stream closes.

Fields:

| Field         | Type      | Always Present | Notes                                     |
| ------------- | --------- | -------------- | ----------------------------------------- |
| \_id          | Object    | Yes            | Resume token (has fromInvalidate bit set) |
| operationType | String    | Yes            | "invalidate"                              |
| clusterTime   | Timestamp | Yes            |                                           |
| wallTime      | Date      | Yes            |                                           |

No ns, documentKey, or other fields.

### Group 3 — Classic Internal Sharding Events (always visible, sharded cluster only)

These events appear on sharded clusters regardless of configuration flags. They are generated from
noop oplog entries.

#### `reshardBegin`

- Triggered by: Start of a resharding operation (noop oplog entry written on donor shards). Contains
  o2.reshardBegin.
- Topologies: Sharded cluster only.

Fields:

| Field          | Type              | Always Present                                             | Notes                            |
| -------------- | ----------------- | ---------------------------------------------------------- | -------------------------------- |
| \_id           | Object            | Yes                                                        | Resume token                     |
| operationType  | String            | Yes                                                        | "reshardBegin"                   |
| clusterTime    | Timestamp         | Yes                                                        |                                  |
| wallTime       | Date              | Yes                                                        |                                  |
| ns             | Object {db, coll} | Yes                                                        | The collection being resharded   |
| reshardingUUID | UUID              | Yes                                                        | UUID of the resharding operation |
| fromMigrate    | Boolean           | Only if showMigrationEvents=true and marked as fromMigrate |                                  |

#### `reshardDoneCatchUp`

- Triggered by: Completion of the catch-up phase of resharding on the recipient shard. The ns refers
  to the temporary system.resharding.\* collection.
- Topologies: Sharded cluster only.

Fields:

| Field          | Type              | Always Present                   | Notes                                         |
| -------------- | ----------------- | -------------------------------- | --------------------------------------------- |
| \_id           | Object            | Yes                              | Resume token                                  |
| operationType  | String            | Yes                              | "reshardDoneCatchUp"                          |
| clusterTime    | Timestamp         | Yes                              |                                               |
| wallTime       | Date              | Yes                              |                                               |
| ns             | Object {db, coll} | Yes                              | system.resharding.<uuid> collection namespace |
| reshardingUUID | UUID              | Yes                              | UUID of the resharding operation              |
| fromMigrate    | Boolean           | Only if showMigrationEvents=true |                                               |

#### `migrateChunkToNewShard`

- Triggered by: An internal noop oplog entry written when a chunk is migrated to a new shard, to
  signal mongos to open change stream cursors on that shard. Contains o2.migrateChunkToNewShard.
- Topologies: Sharded cluster, mongos-level streams only. This is an internal control event not
  meaningful to end users and may be removed in a future version (see TODO SERVER-112325).

Fields:

| Field         | Type              | Always Present | Notes                    |
| ------------- | ----------------- | -------------- | ------------------------ |
| \_id          | Object            | Yes            | Resume token             |
| operationType | String            | Yes            | "migrateChunkToNewShard" |
| clusterTime   | Timestamp         | Yes            |                          |
| wallTime      | Date              | Yes            |                          |
| ns            | Object {db, coll} | Yes            |                          |

### Group 4 — Expanded DDL Events (showExpandedEvents=true required)

These events require showExpandedEvents: true. Without it, the pipeline adds a post-transform filter
that excludes all non-classic operation types.

Additionally, when showExpandedEvents=true:

- All events gain the collectionUUID field (if a UUID is available).
- update events gain the disambiguatedPaths sub-field in updateDescription.

#### `create`

- Triggered by:
  - create command (explicit collection creation).
  - Implicit collection creation triggered by a first insert.
  - Insert into system.views (view/timeseries creation) — only on whole-db or whole-cluster streams.

Fields:

| Field                | Type              | Always Present                     | Notes                                                                                                                     |
| -------------------- | ----------------- | ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| \_id                 | Object            | Yes                                | Resume token                                                                                                              |
| operationType        | String            | Yes                                | "create"                                                                                                                  |
| clusterTime          | Timestamp         | Yes                                |                                                                                                                           |
| wallTime             | Date              | Yes                                |                                                                                                                           |
| ns                   | Object {db, coll} | Yes                                | The created collection/view namespace                                                                                     |
| operationDescription | Object            | Yes                                | Collection options (e.g. idIndex, capped, size, timeseries, viewOn, pipeline, etc.) — the create field itself is excluded |
| nsType               | String            | Yes                                | "collection", "timeseries", "view"                                                                                        |
| collectionUUID       | UUID              | Yes (when showExpandedEvents=true) | UUID of the created collection                                                                                            |

Notes on nsType: Regular collection creates yield "collection" or "timeseries". View creates (from
system.views inserts) yield "view" or "timeseries".

#### `createIndexes`

- Triggered by:
  - createIndex / createIndexes command on an empty collection (one event per index).
  - createIndexes on a non-empty collection with data (single event covering all indexes, from
    commitIndexBuild).
  - commitIndexBuild oplog entry (same event type as createIndexes).

Fields:

| Field                | Type              | Always Present                     | Notes                                                                 |
| -------------------- | ----------------- | ---------------------------------- | --------------------------------------------------------------------- |
| \_id                 | Object            | Yes                                | Resume token                                                          |
| operationType        | String            | Yes                                | "createIndexes"                                                       |
| clusterTime          | Timestamp         | Yes                                |                                                                       |
| wallTime             | Date              | Yes                                |                                                                       |
| ns                   | Object {db, coll} | Yes                                |                                                                       |
| operationDescription | Object            | Yes                                | {indexes: [{v, key, name, ...options}]} — full index specification(s) |
| collectionUUID       | UUID              | Yes (when showExpandedEvents=true) |                                                                       |

#### `dropIndexes`

- Triggered by: dropIndex / dropIndexes command.

Fields:

| Field                | Type              | Always Present                     | Notes                                                                           |
| -------------------- | ----------------- | ---------------------------------- | ------------------------------------------------------------------------------- |
| \_id                 | Object            | Yes                                | Resume token                                                                    |
| operationType        | String            | Yes                                | "dropIndexes"                                                                   |
| clusterTime          | Timestamp         | Yes                                |                                                                                 |
| wallTime             | Date              | Yes                                |                                                                                 |
| ns                   | Object {db, coll} | Yes                                |                                                                                 |
| operationDescription | Object            | Yes                                | {indexes: [{v, key, name, ...options}]} — specification(s) of the dropped index |
| collectionUUID       | UUID              | Yes (when showExpandedEvents=true) |                                                                                 |

#### `modify`

- Triggered by:
  - collMod command on a collection (modifying validator, TTL expiry, index visibility, etc.).
  - collMod on a view definition — only on whole-db or whole-cluster streams (via system.views
    update path).

Fields:

| Field                | Type              | Always Present                     | Notes                                                                                                                                                                           |
| -------------------- | ----------------- | ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| \_id                 | Object            | Yes                                | Resume token                                                                                                                                                                    |
| operationType        | String            | Yes                                | "modify"                                                                                                                                                                        |
| clusterTime          | Timestamp         | Yes                                |                                                                                                                                                                                 |
| wallTime             | Date              | Yes                                |                                                                                                                                                                                 |
| ns                   | Object {db, coll} | Yes                                |                                                                                                                                                                                 |
| operationDescription | Object            | Yes                                | The collMod options applied (e.g. {validator, validationLevel, index: {name, hidden, expireAfterSeconds}, viewOn, pipeline, ...}) — the collMod field itself is excluded        |
| stateBeforeChange    | Object            | Optional                           | {collectionOptions: {...}, indexOptions: {...}}. collectionOptions always present; indexOptions only if an index was modified. Contains the previous values of modified fields. |
| collectionUUID       | UUID              | Yes (when showExpandedEvents=true) |                                                                                                                                                                                 |

#### `shardCollection`

- Triggered by: shardCollection command. Generated from a noop oplog entry with o2.shardCollection.
- Topologies: Sharded cluster only.

Fields:

| Field                | Type              | Always Present                     | Notes                                                |
| -------------------- | ----------------- | ---------------------------------- | ---------------------------------------------------- |
| \_id                 | Object            | Yes                                | Resume token                                         |
| operationType        | String            | Yes                                | "shardCollection"                                    |
| clusterTime          | Timestamp         | Yes                                |                                                      |
| wallTime             | Date              | Yes                                |                                                      |
| ns                   | Object {db, coll} | Yes                                |                                                      |
| operationDescription | Object            | Yes                                | {shardKey, unique, presplitHashedZones, capped, ...} |
| collectionUUID       | UUID              | Yes (when showExpandedEvents=true) |                                                      |

#### `refineCollectionShardKey`

- Triggered by: refineCollectionShardKey command. Generated from a noop oplog entry with
  o2.refineCollectionShardKey.
- Topologies: Sharded cluster only.

Fields:

| Field                | Type              | Always Present                     | Notes                                      |
| -------------------- | ----------------- | ---------------------------------- | ------------------------------------------ |
| \_id                 | Object            | Yes                                | Resume token                               |
| operationType        | String            | Yes                                | "refineCollectionShardKey"                 |
| clusterTime          | Timestamp         | Yes                                |                                            |
| wallTime             | Date              | Yes                                |                                            |
| ns                   | Object {db, coll} | Yes                                |                                            |
| operationDescription | Object            | Yes                                | {shardKey: <new>, oldShardKey: <previous>} |
| collectionUUID       | UUID              | Yes (when showExpandedEvents=true) |                                            |

#### `reshardCollection`

- Triggered by: reshardCollection, moveCollection, unshardCollection, or rewriteCollection commands.
  Generated from a noop oplog entry with o2.reshardCollection. The operationDescription.provenance
  field distinguishes the source command.
- Topologies: Sharded cluster only.

Fields:

| Field                | Type              | Always Present                     | Notes                                                                                          |
| -------------------- | ----------------- | ---------------------------------- | ---------------------------------------------------------------------------------------------- |
| \_id                 | Object            | Yes                                | Resume token                                                                                   |
| operationType        | String            | Yes                                | "reshardCollection"                                                                            |
| clusterTime          | Timestamp         | Yes                                |                                                                                                |
| wallTime             | Date              | Yes                                |                                                                                                |
| ns                   | Object {db, coll} | Yes                                |                                                                                                |
| operationDescription | Object            | Yes                                | {reshardUUID, shardKey, oldShardKey, unique, numInitialChunks, provenance, collation?, zones?} |
| collectionUUID       | UUID              | Yes (when showExpandedEvents=true) | Pre-reshard UUID                                                                               |

provenance values: "reshardCollection", "moveCollection", "unshardCollection", "rewriteCollection".

#### `endOfTransaction`

- Triggered by: A noop oplog entry written at the commit of each multi-document transaction when the
  featureFlagEndOfTransactionChangeEvent feature flag is enabled. The event signals the end of a
  transaction's changes within the stream.
- Requirements: showExpandedEvents=true and the featureFlagEndOfTransactionChangeEvent feature flag
  must be enabled.
- Topologies: Replica set and sharded cluster.

Fields:

| Field                | Type      | Always Present | Notes                                                             |
| -------------------- | --------- | -------------- | ----------------------------------------------------------------- |
| \_id                 | Object    | Yes            | Resume token                                                      |
| operationType        | String    | Yes            | "endOfTransaction"                                                |
| clusterTime          | Timestamp | Yes            |                                                                   |
| wallTime             | Date      | Yes            |                                                                   |
| operationDescription | Object    | Yes            | {lsid: {...}, txnNumber: <Long>}                                  |
| lsid                 | Object    | Yes            | Logical session ID (top-level, mirrors operationDescription.lsid) |
| txnNumber            | Long      | Yes            | Transaction number (top-level)                                    |

No ns, no collectionUUID.

### View Definition Events (whole-db/whole-cluster only, showExpandedEvents=true)

When showExpandedEvents=true and the stream is opened at the database or cluster level (not
single-collection), CRUD operations on system.views are transformed into view DDL events. The
ChangeStreamViewDefinitionEventTransformation class handles this path:

- INSERT into `system.views` → `create` event with nsType: "view" or nsType: "timeseries"
- UPDATE in `system.views` → `modify` event
- DELETE from `system.views` → `drop` event

The fields match those of their regular DDL counterparts (see create, modify, drop above). The
operationDescription reflects the view definition (viewOn, pipeline). No collectionUUID is included
(view namespace has no associated UUID in these events).

### Group 5 — System Events (showSystemEvents=true required)

#### `reshardBlockingWrites`

- Triggered by: Noop oplog entry written when resharding reaches the blocking-writes phase. Contains
  o2.reshardBlockingWrites. Requires showSystemEvents=true to be matched from the oplog. Since it is
  in kClassicOperationTypes, showExpandedEvents is NOT additionally required.
- Topologies: Sharded cluster only.

Fields:

| Field                | Type              | Always Present | Notes                                    |
| -------------------- | ----------------- | -------------- | ---------------------------------------- |
| \_id                 | Object            | Yes            | Resume token                             |
| operationType        | String            | Yes            | "reshardBlockingWrites"                  |
| clusterTime          | Timestamp         | Yes            |                                          |
| wallTime             | Date              | Yes            |                                          |
| ns                   | Object {db, coll} | Yes            | The collection being resharded           |
| reshardingUUID       | UUID              | Yes            | UUID of the resharding operation         |
| operationDescription | Object            | Yes            | {reshardingUUID, type: "reshardFinalOp"} |

#### CRUD events on system collections

When showSystemEvents=true, the collection-matching regex is widened from

    (?!(\$|system\.))

to

    (?!(\$|system\.(?!(js$|resharding\.|buckets\.|views$))))

This allows the following system collections to appear in the stream as regular CRUD events:

| System collection pattern | Notes                                                                     |
| ------------------------- | ------------------------------------------------------------------------- |
| system.js                 | JavaScript stored functions                                               |
| system.resharding.<uuid>  | Temporary resharding collection                                           |
| system.buckets.<name>     | Internal timeseries bucket collection                                     |
| system.views              | View definitions (see also: view DDL events when showExpandedEvents=true) |

These produce `insert`, `update`, `replace`, `delete` events identical in structure to regular CRUD
events, with the system collection namespace in the ns field.

### Group 6 — Events Requiring Both showSystemEvents=true AND showExpandedEvents=true

#### `startIndexBuild`

- Triggered by: startIndexBuild oplog command (beginning of an index build on a non-empty
  collection). Requires showSystemEvents=true to be matched from the oplog, and
  showExpandedEvents=true to pass the post-transform classic-event filter.

Fields:

| Field                | Type              | Always Present                     | Notes                                   |
| -------------------- | ----------------- | ---------------------------------- | --------------------------------------- |
| \_id                 | Object            | Yes                                | Resume token                            |
| operationType        | String            | Yes                                | "startIndexBuild"                       |
| clusterTime          | Timestamp         | Yes                                |                                         |
| wallTime             | Date              | Yes                                |                                         |
| ns                   | Object {db, coll} | Yes                                |                                         |
| operationDescription | Object            | Yes                                | {indexes: [{v, key, name, ...options}]} |
| collectionUUID       | UUID              | Yes (when showExpandedEvents=true) |                                         |

#### `abortIndexBuild`

- Triggered by: abortIndexBuild oplog command (index build aborted). Same flag requirements as
  startIndexBuild.

Fields:

| Field                | Type              | Always Present                     | Notes                                   |
| -------------------- | ----------------- | ---------------------------------- | --------------------------------------- |
| \_id                 | Object            | Yes                                | Resume token                            |
| operationType        | String            | Yes                                | "abortIndexBuild"                       |
| clusterTime          | Timestamp         | Yes                                |                                         |
| wallTime             | Date              | Yes                                |                                         |
| ns                   | Object {db, coll} | Yes                                |                                         |
| operationDescription | Object            | Yes                                | {indexes: [{v, key, name, ...options}]} |
| collectionUUID       | UUID              | Yes (when showExpandedEvents=true) |                                         |

#### `migrateLastChunkFromShard`

- Triggered by: Noop oplog entry written when the last chunk is migrated off a shard. Requires both
  showSystemEvents=true (to be included in the noop event filter) and showExpandedEvents=true (since
  it is not in kClassicOperationTypes).
- Topologies: Sharded cluster only.

Fields:

| Field                | Type              | Always Present                     | Notes                       |
| -------------------- | ----------------- | ---------------------------------- | --------------------------- |
| \_id                 | Object            | Yes                                | Resume token                |
| operationType        | String            | Yes                                | "migrateLastChunkFromShard" |
| clusterTime          | Timestamp         | Yes                                |                             |
| wallTime             | Date              | Yes                                |                             |
| ns                   | Object {db, coll} | Yes                                |                             |
| operationDescription | Object            | Yes                                | {shardId: "<shard name>"}   |
| collectionUUID       | UUID              | Yes (when showExpandedEvents=true) |                             |

### Group 7 — Migration Events (showMigrationEvents=true, shard-level only)

showMigrationEvents: true is only accepted on direct shard connections — mongos rejects it with
error code 31123.

Without this flag, all oplog entries with fromMigrate: true are excluded from the change stream.
With this flag, chunk-migration writes become visible as ordinary CRUD events (insert, delete):

- Donor shard: emits delete events for documents moved out.
- Recipient shard: emits insert events for documents moved in.

These events are structurally identical to regular insert/delete events. The only additional field
is:

| Field       | Type           | Condition                                                                                                 |
| ----------- | -------------- | --------------------------------------------------------------------------------------------------------- |
| fromMigrate | Boolean (true) | Only present if showMigrationEvents=true and the changeStreamsEmitFromMigrate server parameter is enabled |

Interaction with showSystemEvents: When showMigrationEvents=false but showSystemEvents=true, create
and createIndexes migration-marked events are selectively allowed through (used for
system.resharding.\* and other internal collection setups).

## Summary Table

| Event type                                              | Always              | showExpandedEvents   | showSystemEvents | Both flags | showMigrationEvents  | Topology              |
| ------------------------------------------------------- | ------------------- | -------------------- | ---------------- | ---------- | -------------------- | --------------------- |
| insert                                                  | ✓                   |                      |                  |            | (migration variant)  | All                   |
| update                                                  | ✓                   |                      |                  |            | (migration variant)  | All                   |
| replace                                                 | ✓                   |                      |                  |            | (migration variant)  | All                   |
| delete                                                  | ✓                   |                      |                  |            | (migration variant)  | All                   |
| drop                                                    | ✓                   |                      |                  |            |                      | All                   |
| rename                                                  | ✓                   |                      |                  |            |                      | All                   |
| dropDatabase                                            | ✓ (db/cluster only) |                      |                  |            |                      | All                   |
| invalidate                                              | ✓ (coll/db only)    |                      |                  |            |                      | All                   |
| reshardBegin                                            | ✓                   |                      |                  |            |                      | Sharded               |
| reshardDoneCatchUp                                      | ✓                   |                      |                  |            |                      | Sharded               |
| migrateChunkToNewShard                                  | ✓ (mongos internal) |                      |                  |            |                      | Sharded               |
| create                                                  |                     | ✓                    |                  |            |                      | All                   |
| createIndexes                                           |                     | ✓                    |                  |            |                      | All                   |
| dropIndexes                                             |                     | ✓                    |                  |            |                      | All                   |
| modify                                                  |                     | ✓                    |                  |            |                      | All                   |
| shardCollection                                         |                     | ✓                    |                  |            |                      | Sharded               |
| refineCollectionShardKey                                |                     | ✓                    |                  |            |                      | Sharded               |
| reshardCollection                                       |                     | ✓                    |                  |            |                      | Sharded               |
| endOfTransaction                                        |                     | ✓ + feature flag     |                  |            |                      | Replica set / Sharded |
| View DDL events (create, modify, drop via system.views) |                     | ✓ + db/cluster scope |                  |            |                      | All                   |
| reshardBlockingWrites                                   |                     |                      | ✓                |            |                      | Sharded               |
| System collection CRUD (system.js, etc.)                |                     |                      | ✓                |            |                      | All                   |
| startIndexBuild                                         |                     |                      |                  | ✓          |                      | All                   |
| abortIndexBuild                                         |                     |                      |                  | ✓          |                      | All                   |
| migrateLastChunkFromShard                               |                     |                      |                  | ✓          |                      | Sharded               |
| Migration CRUD (fromMigrate)                            |                     |                      |                  |            | ✓ (shard-level only) | Sharded               |

## Fields Common to All Events

Every change event includes:

| Field         | Notes                                                                                                                   |
| ------------- | ----------------------------------------------------------------------------------------------------------------------- |
| \_id          | Resume token object. Contains clusterTime, version, txnOpIndex, and event identity. Used with resumeAfter / startAfter. |
| operationType | String identifying the event type.                                                                                      |
| clusterTime   | Timestamp: the logical cluster time of the event.                                                                       |
| wallTime      | Date: wall clock time from the oplog entry.                                                                             |

Most events include ns. Exceptions: invalidate (no ns), endOfTransaction (no ns).
