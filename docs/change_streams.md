# Change Streams Overview

Disclaimer: the following documentation is based on how change streams are implemented in the
current version of master, if not explicitly stated otherwise. Implementation details for previous
versions may vary slightly.

Change streams are a convenient way for an application to monitor changes made to the data in a
deployment. The events produced by change streams are called "change events". The event data is
produced from the oplog(s) of the deployment. The events that are emitted by change streams include

- DML events: emitted for operations that insert, update, replace, or delete individual documents.
- DDL events: emitted for operations that create, drop, or modify collections, databases, or views.
- Data placement events: emitted for operations that define or modify the placement of data inside a
  sharded cluster.
- Cluster topology events: emitted for operations that add or remove shards in a sharded cluster.

Which exact event types are emitted by a change stream depends on the change stream configuration
and the deployment type.

Change streams are mainly used by customer applications and tools to keep track of changes to the
data in a deployment, in order to relay these updates to external systems. Some of MongoDB's own
tools and components are also based on change streams, e.g. _mongosync_ (C2C), Atlas Search, Atlas
Stream Processing, and the resharding process. The component that opens a change stream and pulls
events from it is called the "consumer".

## Change Stream Guarantees

Change Streams provide various guarantees:

- Ordering: change streams deliver events in the order they originally occurred within the target
  namespace (e.g., collection, database, or entire cluster). The order is based on the sequence in
  which the operations were applied to the oplog. In a sharded cluster, the events from multiple
  oplogs will be merged deterministically into a single, ordered stream of change events.
- Durability and reproducability: change streams are based on the internal oplog, which is part of
  the deployment's replication mechanism. Change streams only deliver events after they have been
  committed to a majority of nodes and durably persisted, ensuring they will not be rolled back.
- Exactly-once delivery: every event in a change stream is emitted exactly once, and no event that
  matches the change stream filter is skipped.
- Resumability: change stream consumption can be interrupted due to transient errors (e.g. network
  issues, node failures, application errors), but it can be resumed from the exact point where the
  consumption stopped. This is made possible by the resume token (`_id` field) that accompanies
  every change event, which acts as a bookmark. This allows to the consumer to continue processing
  changes from the last known position without missing events.

### Change Stream Namespace Scopes

Change streams can be opened on different levels that control which events are emitted:

- collection-level change streams: these change streams only include events related to a specific
  collection namespace (e.g. `testDB.testCollection`).
- database-level change streams: such change streams include events related to all collections in a
  specific database (e.g. `testDB`).
- all-cluster change streams: these change streams include events for all databases/collections in
  replica set or sharded cluster deployment.

Consumers can use additional filters to a change stream by adding `$match` stages to the change
stream pipeline as needed.

Change streams cannot be opened on views. This includes timeseries collections if they use views.

Change streams respect the read permissions of the consumer. A consumer can only open change streams
on collections or databases that they have access to.

## Opening a Change Stream

Change streams can be opened against a replica sets and sharded cluster deployments. They cannot be
opened against standalone _mongod_ instances, as there is no oplog to generate the events from in
standalone mode.

In replica set deployments, the change stream can be opened directly on any replica set member of
the deployment. In sharded cluster deployments, the change stream must be opened against any of the
deployment's _mongos_ processes.

A change stream is opened by executing an `aggregate` command with a pipeline that contains at least
the `$changeStream` pipeline stage.

_mongosh_, the "jstest shell" (_mongo_) and many drivers provide simpler "watch" wrappers for this.

### Opening a Collection-Level Change Stream

To open a collection-level change stream on a specific collection (e.g., `testDB.testCollection`),
the following _mongosh_ command can be used:

```js
db.getSiblingDB("testDB").runCommand({
  aggregate: "testCollection",
  pipeline: [
    {
      $changeStream: {},
    },
  ],
  cursor: {},
});
```

### Opening a Database-Level Change Stream

To open a database-level change stream on a specific database (e.g., `testDB`), use the following
command in _mongosh_:

```js
db.getSiblingDB("testDB").runCommand({
  aggregate: 1,
  pipeline: [
    {
      $changeStream: {},
    },
  ],
  cursor: {},
});
```

The `aggregate` parameter must be set to `1` for database-level change streams, and the command must
be executed inside the desired database. The internal namespace that is used by database-level
change streams is `<dbName>.$cmd.aggregate` (where `<dbName>` is the actual name of the database).

### Opening an All-Cluster Change Stream

All-cluster change streams can only be opened on the `admin` database and also need the
`allChangesForCluster` flag to be set to `true` in order to work. The following _mongosh_ command
can be used to open an all-cluster change stream:

```js
db.adminCommand({
  aggregate: 1,
  pipeline: [
    {
      $changeStream: {
        allChangesForCluster: true,
      },
    },
  ],
  cursor: {},
});
```

The internal namespace that is used by all-cluster change streams is always `admin.$cmd.aggregate`.

### Adding more Pipeline Stages

As a `$changeStream` is a pipeline, additional pipeline stages can be added to it for filtering and
transforming results, e.g.

- `$addFields`
- `$match`
- `$project`
- `$replaceRoot`
- `$replaceWith`
- `$redact`
- `$set`
- `$unset`

There is also a change streams-specific stage `$changeStreamSplitLargeEvent` to split large events
into smaller fragments, in order to avoid running into `BSONObjectTooLarge` errors.

### Change Stream Start Time

When opening a change stream without specifying an explicit point in time, the change stream will be
opened using the current time, and will report only change events that happened after that point in
time. The current time here is

- the time of the latest majority-committed operation for replica set change streams, or
- the value of the cluster's vector clock for sharded cluster change streams.

To open a change stream at a specific point in time instead of using the current time, the parameter
`startAtOperationTime` can be set in the initial change stream request. The `startAtOperationTime`
parameter is specified as a logical timestamp.

### Resuming Change Streams

Change streams allow the consumer to resume the change stream after an error occurred. To support
resumability, change streams report a "resume token" inside the `_id` field of every emitted event.
To resume a change stream after an error occurred, the resume token of a previously consumed event
can be passed in one of the parameters `resumeAfter` or `startAfter` when opening a change stream.

The `resumeAfter` parameter cannot be used with resume tokens that were emitted by an "invalidate"
event. The `startAfter` parameter can be used even with invalidate events.

When specifying an explicit start point for a change stream, only one of the parameters
`resumeAfter`, `startAfter` and `startAtOperationTime` can be used. Using more than one of them when
opening a change stream will return an error.

#### Resume Tokens

Resume tokens are not "portable" in the sense that they can only be used to resume a change stream
that is opened with the same settings and pipeline stages as the change stream that produced the
original resume token.

For example, changing the `$match` expression of a change stream when resuming from a change stream
with a different `$match` expression may lead to different events being returned, which may lead to
the event with the original resume token not being found in the new change stream.

The resume tokens that are emitted by change streams are string values that contain a hexadecimal
encoding of the internal resume token data. The internal resume token data contains

- the cluster time of an event.
- the version of the resume token format.
- the type of the token (event token or high watermark token).
- the internal position inside the transaction, if the event was part of a transaction.
- a flag stating if the resume token is for an "invalidate" event.
- the collection UUID (optional).
- an event identifier / event description (optional).

Resume tokens are versioned. Currently only version 2 is supported.

Future versions may introduce new resume token versions. Client applications should treat resume
tokens as opaque identifiers and should not make any assumptions about the format or internals or
resume tokens, nor should they rely on the internal implementation details of resume tokens.

Resume tokens are serialized and deserialized by the
[ResumeToken](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/resume_token.h#L148)
class. The resume token internal data is stored in
[ResumeTokenData](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/resume_token.h#L51).

#### Resume Token Types

There are two types of resume tokens:

- event resume tokens
- high watermark resume tokens

The former stem from actual change events. High watermark token are a special kind of change stream
resume token that represent a logical position in the global change stream ordered only by cluster
time, not a specific event.

High watermark tokens sort strictly before any real event token at the same cluster time. That is, a
high‑watermark token for time T sorts ahead of all events whose cluster time >= T.

#### Decoding Resume Tokens

An example encoded resume token looks as follows:

```js
{ "_data" : "8269B03187000000022B0429296E1404" }
```

To destructure a resume token into its internal constituent parts, there is the function
`decodeResumeToken()` available in the _mongo_ shell.

Invoking it on the example resume token above, it will produce:

```js
{
  clusterTime: Timestamp(1773154695, 2),
  tokenData: 0,
  version: 2,
  txnOpIndex: NumberLong(0),
  tokenType: 0,
  fromInvalidate: false
}
```

This can be very helpful to extract the cluster time (i.e. the resume timepoint) from a resume
token.

### Change Stream Cursors

When opening a change stream on a replica set, a cursor will be established on the targeted replica
set node. The change stream cursor is "tailable" and will remain open until it is explicitly closed
by the consumer or the change stream runs into an error. Also, unused cursors are eventually
garbage-collected after a period of inactivity.

When opening a change stream on a sharded cluster, the targeted `mongos` instance will open the
required cursors on the relevant shards of the cluster and also the config server. Here, the
`mongos` instance will also automatically open additional cursors in case new shards are added to
the cluster. All this is abstracted from the consumer of the change stream. The consumer of the
change stream will only see a single cursor and interact with _mongos_, which handles the complexity
of managing the underlying shard cursors.

If a change stream cursor can be successfully established, the cursor id is returned to the
consumer. The consumer can then use the cursor id to pull change events from the change stream by
issuing follow-up `getMore` commands to this cursor.

If a change stream cursor cannot be successfully opened, the initial `aggregate` command will return
an error, and the returned cursor id will be `0`. In this case, no events can be consumed from the
change stream, and the consumer needs to resolve the error.

### Change Stream errors

When a change stream is opened at a specific point in time, it is validated that the oplog of all
participating nodes actually contains data for this point in time. If the oplog does not contain any
data for the exact point in time or before, it would be possible that the requested data has already
fallen off the oplog. In case no oplog entry can be found that is at least as old as the specified
timetamp, opening the change stream will fail with error code `OplogQueryMinTsMissing`. This
validation happens for all change streams, regardless if the start timestamp is specified via the
`resumeAfter`, `startAfter` or `startAtOperationTime` parameters, or if the start time is implied
from the current time. An exception in which opening a change stream at a later point in time than
the timestamp of the first present oplog entry is permitted is for new shard primaries. New shard
primary can be added to an existing cluster at any point in time. When a new shard primary is added,
its first oplog entry will be a no-op entry with `msg` == `initiating set` (on ASC) or `msg` ==
`new primary` (on DSC).

The code for this can be found
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/classic/collection_scan.cpp#L195-L227).

Another common error is `ChangeStreamHistoryLost`. This error is raised when a change stream is
opened with a resume token that cannot be found (anymore) in any of the participating nodes' oplogs.
This can either happen when the resume event has actually fallen off the oplog, or, when a change
stream is resumed with the resume token from another change stream with a different `$match`
expression. In this case, the new change stream may filter out the resume event due to the different
`$match` expression, so it cannot be found anymore.

Resuming a change stream using a resume token from a change stream with a different `$match`
expression is thus not guaranteed to work.

## Consuming Events from a Change Stream

To fetch events from a previously opened change stream, the consumer can send a `getMore` request
using the cursor id that was established by the initial `aggregate` command, e.g.

```js
// For a collection-level change stream on "testDB.testCollection"
db.getSiblingDB("testDB").runCommand({
  getMore: cursorId,
  collection: "testCollection",
});

// For a database-level change stream on "testDB"
db.getSiblingDB("testDB").runCommand({
  getMore: cursorId,
  collection: "$cmd.aggregate",
});

// For an all-cluster change stream:
db.adminCommand({
  getMore: cursorId,
  collection: "$cmd.aggregate",
});
```

Responses can be further controlled by using the following optional parameters in the `getMore`
request:

- `batchSize`: maximum number of change events to return in the response.
- `maxTimeMS`: maximum server-side waiting time for producing events.

The `getMore` command will fill the response with up to `batchSize` results if that many events are
available. A response can also contain less events than the specified `batchSize`. Regardless of the
specified batch size, the maximum response size limit of 16MB will be honored, in order to prevent
responses from getting too large.

A change stream response is returned to the consumer when

- `batchSize` events have been accumulated in the response, or
- at least one event has been accumulated in the response, but adding the next event to the response
  would make it exceed the 16MB size limit.

In case the change stream cursor has reached the end of the oplog and there are currently no events
to return, the response will be returned immediately if it already contains at least one event. If
the response is empty, the change stream will wait for at most `maxTimeMS` for new oplog entries to
arrive. If no new oplog entries arrive within `maxTimeMS`, an empty response will be returned. If
new oplog entries arrive within `maxTimeMS` and at least one of them matches the change stream's
filter, the matching event will be returned immediately. If oplog entries arrive but do not match
the change stream's filter, the change stream will wait for matching oplog entries until `maxTimeMS`
is fully expired.

### Generic Event layout

In general, the returned change stream events have the following fields:

- `_id`: resume token for the event. This is **not** the same as the document id. The resume token
  can be used to open a new change stream starting at the very same event.
- `operationType`: type of the change stream event.
- `clusterTime`: logical timestamp of when the event originally occurred.
- `wallTime`: wall-clock date/time of when the event originally occurred.
- `ns`: namespace inside which the event occurred.

The following generic fields are added for change streams that were opened with the
`showExpandedEvents` flag:

- `collectionUUID`: UUID of the collection for which the event occurred, if applicable.
- `operationDescription`: populated for DDL events.

Most other fields are event type-specific, so they are only present for specific events. A few such
fields include:

- `documentKey`: the `_id` value of the affected document, populated for DML events. May contain the
  shard key values for sharded collections.
- `fullDocument`: the full document for "insert" and "replace" events. Will also be populated for
  "update" events if the change stream is opened with the `fullDocument` parameter set to any other
  value than `default`.
- `updateDescription` / `rawUpdateDescription`: contains details for "update" events.
- `fromMigrate`: a boolean field set to `true` for events that originate from a data migration
  (e.g., chunk migrations, `moveCollection`, `unshardCollection`, `movePrimary`). This field is only
  present when both the change stream was opened with `showMigrationEvents: true` **and** the server
  parameter `changeStreamsEmitFromMigrate` is enabled (see below). Regular non-migration events
  never carry this field.

The majority of change stream event fields are emitted by the
`ChangeStreamDefaultEventTransformation` object
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/change_stream_event_transform.cpp#L321).
This object is called by the `ChangeStreamEventTransform` stage
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_transform_stage.cpp#L75).

A custom `$project` stage in the change stream pipeline can be used to suppress certain fields.

### Large Events

Emitted change events can get large, especially if they contain pre- or post-images. In this case
the events can exceed the maximum BSON object size of 16MB, which can lead to `BSONObjectTooLarge`
errors when trying to process these change stream events.

To split large change stream events into multiple smaller chunks, change stream consumers can add a
`$changeStreamSplitLargeEvent` stage as the last step of their change stream pipeline, e.g.

```js
db.getSiblingDB("testDB").runCommand({
  aggregate: "testCollection",
  pipeline: [
    {
      $changeStream: {},
    },
    {
      $changeStreamSplitLargeEvent: {},
    },
  ],
  cursor: {},
});
```

The splitting is performed by the `ChangeStreamSplitLargeEventStage` stage
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_split_large_event_stage.cpp#L72),
using
[this helper function](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/change_stream_split_event_helpers.cpp#L63).
The change stream consumer is responsible for assembling the split event fragments into a single
event later.

#### Change Stream Invalidate Events

Collection-level and database-level change streams can return so-called "invalidate" events that
close the change stream cursor in specific situations:

- in collection-level change streams, the change stream is invalidated in the following situations:
  - the target collection is dropped
  - the target collection is renamed
  - the parent database of the target collection is dropped
- in database-level change streams, the change stream is invalidated if the target database is
  dropped. In case a change stream gets invalidated by any of the above situations, it will emit a
  special "invalidate" event to inform the consumer that further processing is not possible. There
  are no "invalidate" events in all-cluster change streams.

Issuing of change stream invalidate events is implemented in the `ChangeStreamCheckInvalidateStage`
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_check_invalidate_stage.cpp#L106-L157).

## Change Stream Parameters

The behavior of change streams can be controlled via various parameters that can be passed with the
initial `aggregate` command used to open the change stream. The parameters are defined in an
[IDL file](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream.idl#L84).

The parameters that are provided when opening the change stream are automatically validated using
mechanisms provided by the IDL framework. Additional validation of the change stream parameters is
performed
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream.cpp#L391).
Invalid change stream parameters are immediately rejected with appropriate errors.

### `fullDocument`

The `fullDocument` change stream parameter controls what value should be returned inside the
`fullDocument` field for change stream DML "update" events.

The following values are possible:

- `default`: the `fullDocument` field will only be populated for "insert" and "replace" events.
- `updateLookup`: the `fullDocument` field will be populated with the _current_ version of the
  document, identified by the document's `_id` value. Note that the current version of the document
  may not be the same version of the document that was present when the "update" change event was
  originally recorded. If no document can be found by the lookup, the `fullDocument` field will
  contain `null`.
- `whenAvailable`: the `fullDocument` field will be populated with the post-image for the event. The
  post-image is generated on the fly from a stored pre-image and applying a delta update from the
  event on top of it. If no post-image is available, the `fullDocument` field will contain `null`.
- `required`: populates the `fullDocument` field with the post-image for the event. Post-images are
  generated in the same way as in `whenAvailable`. If no post-image can be generated, this will
  abort the change stream with a `NoMatchingDocument` error.

The latter two options rely on pre-images to be enabled for the target collection(s). When
pre-images are enabled, they are written synchronously with the regular "update" oplog entry, and
change stream events aren’t returned until both have been majority-committed.

Post-images for "update" events are added to change events by the `ChangeStreamAddPostImage` stage
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_add_post_image_stage.cpp#L84).

### `fullDocumentBeforeChange`

The `fullDocumentBeforeChange` change stream parameter controls what value should be returned inside
the `fullDocumentBeforeChange` field for change stream DML events ("update", "replace", "delete").
The following values are possible:

- `off` (default): the `fullDocumentBeforeChange` field will always be omitted.
- `whenAvailable`: the field will be populated with the pre-image of the document modified by the
  current change event, if available. If no pre-image is available, the `fullDocumentBeforeChange`
  field will contain `null`.
- `required`: populates the `fullDocumentBeforeChange` field with the stored pre-image for the event
  if it exists. If no pre-image is available, aborts the change stream with a `NoMatchingDocument`
  error.

  Pre-images are added to change events by the `ChangeStreamAddPreImage` stage
  [here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_add_pre_image_stage.cpp#L67).

### Change Stream Flags

There are also numerous flags that control the behavior of change streams. The most important flag
parameters are:

#### `showExpandedEvents` (public)

The `showExpandedEvents` flag can be used to make a change stream return both additional event types
and additional fields. The flag defaults to `false`. In this mode, change streams will only return
DML events and no DDL events. When setting `showExpandedEvents` to `true`, change streams will also
emit events for various DDL operations. In addition, setting `showExpandedEvents` will make change
streams return the additional fields `collectionUUID` (for various change stream event types) and
`updateDescription.disambiguatedPaths` (for update events).

#### `ignoreRemovedShards` (public)

The `ignoreRemovedShards` parameter is only meaningful when a sharded cluster change stream uses the
v2 reader with precise shard targeting. The parameter controls the behavior when the change stream
encounters a time range for which a now-removed shard held relevant data.

The possible values for this parameter are:

- `false` (default, "Strict mode"): The change stream fails if it cannot retrieve events from a
  required shard that has since been removed from the cluster. This guarantees that no events are
  silently lost due to shard removal.
- `true` ("Ignore-Removed-Shards mode" or "IRS mode"): The change stream continues reading even when
  a required shard has been removed. Events that would have originated from the removed shard during
  the affected time range (a _degraded segment_) are skipped. Normal event delivery resumes once the
  stream advances past the degraded time range.

#### `allChangesForCluster` (public)

This flag must be set when opening an all-cluster change stream. Will normally be set by drivers
automatically when opening an all-cluster change stream.

#### `matchCollectionUUIDForUpdateLookup` (internal)

The `matchCollectionUUIDForUpdateLookup` field can be used to ensure that "updateLookup" operations
are performed on the correct collection in case multiple collections with the same name have existed
over time. This is relevant, because change streams can be opened retroactively on collections that
were already dropped and may have been recreated with the same name but different contents
afterwards.

The flag defaults to `false`. In this case, "updateLookup" operations will not verify that the
looked-up document is actually from the same collection "generation" as the change event the
document was looked up for. If set to `true`, "updateLookup" operations will compare the collection
UUID of the change event with the UUID of the collection. If there is a UUID mismatch, the returned
`fullDocument` field of the event will be set to `null`.

#### `version` (internal)

The `version` parameter selects the change stream reader version, which controls the shard-targeting
behavior in sharded cluster deployments. On replica sets, this parameter is accepted but has no
effect. The supported values are:

- `"v1"`: Opens cursors on all shards in the cluster, including the config server, regardless of
  whether they can hold data for the monitored namespace. This is the legacy behavior from earlier
  versions before v9.0.
- `"v2"` (default when shard placement history is available): Uses _precise shard targeting_ to open
  cursors only on the shards that actually hold data for the monitored namespace at any given
  cluster time. See [Precise Shard Targeting in Sharded Cluster Change Streams] below for details.

If the `version` parameter is omitted, the server automatically selects `"v2"` when shard placement
history is available, and falls back to `"v1"` otherwise. Explicitly specifying `"v2"` when
placement history is not available silently falls back to `"v1"`.

This parameter is used for testing and is not intended to be set by change stream consumers. The
server selects the appropriate version automatically.

#### `showSystemEvents` (internal)

The `showSystemEvents` flag can be used to make change streams return events for collections inside
the `system` namespace. These are not emitted by default. Setting `showSystemEvents` to `true` will
also include events related to system collections in the change stream. The flag defaults to `false`
and is internal.

#### `showMigrationEvents` (internal)

The `showMigrationEvents` flag can be used to make change streams return DML events that are
happening during chunk migrations. If set to `true`, insert and delete events related to chunk
migrations will be reported as if they were regular events. The flag defaults to `false` and is
internal.

When `showMigrationEvents` is `true` and the `changeStreamsEmitFromMigrate` server parameter is also
enabled (the default), emitted migration events include a `fromMigrate: true` field. This allows
consumers to distinguish migration-originated events from user-initiated writes.

The `fromMigrate` field is emitted for:

- **DML events** (insert, delete) caused by chunk migration, `moveCollection`, `unshardCollection`,
  or `movePrimary` data cloning. TODO SERVER-107688: add 'fromMigrate' for migration events stemming
  from cross-database renames.
- **DDL events** (create, createIndexes, rename, reshardDoneCatchUp) generated by the resharding
  machinery or `movePrimary` on the recipient shard.

The `fromMigrate` field is intentionally **not** emitted for inserts into temporary resharding
system collections (e.g. `system.resharding.*`), because those documents are owned by the shard they
are written to — they are not orphans.

#### `changeStreamsEmitFromMigrate` server parameter

This is a server parameter (not a change stream flag), configurable at startup or runtime via
`setParameter`. When set to `true` (the default), change streams opened with
`showMigrationEvents: true` will include the `fromMigrate: true` field on events that originate from
a migration. When set to `false`, migration events still appear in the stream (assuming
`showMigrationEvents: true`) but the `fromMigrate` field is omitted.

To disable the `fromMigrate` field at runtime, run the following on each relevant shard:

```js
db.adminCommand({setParameter: 1, changeStreamsEmitFromMigrate: false});
```

#### `showCommitTimestamp` (internal)

The `showCommitTimestamp` flag can be used to include the transaction commit timestamp inside DML
events that were part of a prepared transaction. The flag defaults to `true` and is internal. It is
used by the resharding.

#### `showRawUpdateDescription` (internal)

The `showRawUpdateDescription` flag can be used to make change streams emit the raw, internal format
used for "update" oplog entries. If set to `true`, emitted change stream "update" events will
contain a `rawUpdateDescription` field. The default is `false`. In this case, emitted change stream
"update" events will contain the regular `updateDescription` field.

#### `allowToRunOnConfigDB` (internal)

The `allowToRunOnConfigDB` flag is an internal flag that can be used to open a change stream on the
config server in a sharded cluster. It is used internally by `mongos` to open a cursor on the config
server to keep track of shard additions and removals in the deployment.

#### `$_passthroughToShard` (internal)

In sharded cluster deployments, all change streams are supposed to be opened on _mongos_. _mongos_
will open the required cursors to the data shards and the config server on the consumer's behalf. If
the consumer only wants to target a specific shard of the cluster, they can use the
`$_passthroughToShard` aggregation parameter to limit the change stream to a single shard.

For example, to open a collection-level change stream targeting only one of the cluster's shards
(identified by the value in `shardId`), the following example code can be used:

```js
db.getSiblingDB("testDB").runCommand({
  aggregate: "testCollection",
  pipeline: [
    {
      $changeStream: {},
    },
  ],
  $_passthroughToShard: {shard: shardId},
  cursor: {},
});
```

Using `$_passthroughToShard` will bypass the regular cluster shard targeting for change streams and
open a replica set change stream pipeline (only) on the targeted shard. The change events that
mongos retrieves from the single shard will be returned as is, without using a merge pipeline on
_mongos_.

## Differences Between Replica Set and Sharded Cluster Change Streams

When a change stream is opened against a replica set, the consumer opens the change stream directly
on a replica set node, which can then return matching events immediately from the node's own oplog.
The events are already correctly ordered, and the latency is defined by the node's replication lag
and how close the change stream has advanced towards the end of the node's oplog.

Opening a change stream on a sharded cluster works differently. Here, the consumer opens the change
stream against a _mongos_ instance. The _mongos_ instance will then use the cluster's topology
information to open the cursors on the appropriate data shards on behalf of the consumer. Because of
the ordering guarantee provided by change streams, _mongos_ must wait until all cursors have either
responded with events, or ran into a timeout and reported that currently no more events are
available for them. The latter is why change streams in a sharded cluster can have higher latency
than change streams in replica sets.

For sharded cluster change streams, the merging of the multiple streams of change events from the
different cursors is performed by the
[`AsyncResultsMerger`](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/s/query/exec/async_results_merger.h#L100).

### Precise Shard Targeting in Sharded Cluster Change Streams

By default, sharded cluster change streams use _precise shard targeting_ (also called "v2"
internally). Instead of opening cursors on all shards in the cluster, the change stream queries the
config server's placement history to determine which shards actually hold data for the monitored
namespace, and opens cursors only on those shards. This eliminates unnecessary polling of shards
that have no relevant data and reduces end-to-end latency.

#### Initial shard set

When a v2 change stream is opened, _mongos_ queries the config server's placement history for the
monitored namespace at the requested start time. The resulting set of shards is used to establish
the initial set of remote cursors. If the start time precedes the point at which the placement
history was first recorded (e.g., immediately after an FCV upgrade that introduced placement history
tracking), the change stream may fall back to targeting all shards until the historical placement
information becomes available.

The initial set of targeted shards differs by scope:

- **Collection-level change streams**: Cursors are opened only on shards than can hold chunks of the
  target collection. If the collection's database does not exist yet at the requested start time, a
  cursor is instead opened on the config server to detect when the database and collection are
  created.
- **Database-level change streams**: Cursors are opened on shards that can hold any collection
  belonging to the target database. If the database does not yet exist at the requested start time,
  a cursor is opened on the config server to detect database creation.
- **Cluster-level change streams**: Cursors are always opened on all data-bearing shards plus the
  config server.

#### Dynamic cursor adjustment via control events

As data placement changes (e.g., due to chunk migrations, `reshardCollection`, `movePrimary`, or
`dropCollection`), the change stream must adjust its cursor targeting accordingly. This is
accomplished through _control events_, which are internal, consumer-invisible events that carry
placement change information.

When a DDL operation modifies data placement, the config server writes a `NamespacePlacementChanged`
entry to its oplog and notifies the affected data shards. Each affected shard writes a corresponding
no-op oplog entry. The `$_internalChangeStreamInjectControlEvents` stage on each shard converts
these no-op entries into control events (tagged with `$changeStreamControlEvent` metadata), which
are then forwarded to the `$_internalChangeStreamHandleTopologyChangeV2` stage on _mongos_. This
stage processes the control events and opens or closes shard cursors as appropriate. Control events
are never returned to the consumer.

#### Read modes in v2

v2 change streams can operate in two modes, controlled by the `ignoreRemovedShards` parameter:

- **Strict mode** (default, `ignoreRemovedShards: false`): If the change stream encounters a time
  range for which a now-removed shard held relevant data, the stream fails. This guarantees
  exactly-once delivery even across shard removal events.
- **Ignore-Removed-Shards (IRS) mode** (`ignoreRemovedShards: true`): The change stream continues
  reading even when a required shard has been removed. Events from the removed shard during
  _degraded segments_ (bounded time ranges where the shard was required but is gone) are skipped.
  Once the stream advances past a degraded segment, normal event delivery resumes. This mode is
  appropriate for consumers that can tolerate event loss during shard decommissioning.

#### FCV compatibility and fallback

v2 change streams require placement history to be recorded in the config server. This recording is
automatically performed in FCV 9.0 and higher. If the cluster's FCV is downgraded to a version that
does not maintain placement history (i.e. any FCV before 9.0), an active v2 change stream
transparently falls back to v1 mode without losing events (by internally reopening its cursors).
Resume tokens produced by v2 change streams are valid in v1 streams and vice versa.

An existing v1 stream does not automatically upgrade to v2 when the FCV is upgraded. A new v2 stream
must be explicitly opened to take advantage of precise shard targeting.

## Change Stream Pipeline Building

A change stream pipeline issued by a consumer contains the `$changeStream` meta stage. This stage is
expanded internally into multiple `DocumentSource`s
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/change_stream_pipeline_helpers.cpp#L171).

The change stream `DocumentSource`s are located in the `src/mongo/db/pipeline` directory
[here](https://github.com/mongodb/mongo/tree/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline),
among other `DocumentSource`s that are not related to change streams. The `DocumentSource`s are only
used for pipeline building and optimization, but they are converted into execution `Stage`s later
when the change stream is executed. These `Stage`s are located in the `src/mongo/db/exec/agg`
directory
[here](https://github.com/mongodb/mongo/tree/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg).

### Replica Set Pipelines

On a replica set, the `$changeStream` stage is expanded into the following internal stages:

- `$_internalChangeStreamOplogMatch`
- `$_internalChangeStreamUnwindTransaction`
- `$_internalChangeStreamTransform`
- `$_internalChangeStreamCheckInvalidate` (only present for collection-level and database-level
  change streams)
- `$_internalChangeStreamCheckResumability`
- `$_internalChangeStreamAddPreImage` (only present if `fullDocumentBeforeChange` is not set to
  `off`)
- `$_internalChangeStreamAddPostImage` (only present if `fullDocument` is not set to `default`)
- `$_internalChangeStreamEnsureResumeTokenPresent` (only present if the change stream resume token
  is not a high water mark token)
- user-defined `$match` expression (only present if the user's change stream pipeline contains a
  `$match` stage)
- user-defined `$project` expression (only present if the user's change stream pipeline contains a
  `$project` stage)
- `$_internalChangeStreamSplitLargeEvent` (only present if the change stream is opened with the
  `$changeStreamSplitLargeEvent` pipeline step)

The change stream pipeline on replica sets will also contain a `$match` stage to filter out all
non-DML change events in case `showExpandedEvents` is not set.

### Sharded Cluster Pipelines

For sharded cluster change streams, _mongos_ will expand the `$changeStream` stage into multiple
internal stages split across a _shard pipeline_ (executed on each data shard) and a _merge pipeline_
(executed on _mongos_). The exact stages depend on the change stream reader version.

#### Data Shard Pipeline

The shard pipeline will look like this:

- `$_internalChangeStreamOplogMatch`
- `$_internalChangeStreamUnwindTransaction`
- `$_internalChangeStreamTransform`
- `$_internalChangeStreamCheckInvalidate` (only present for collection-level and database-level
  change streams)
- `$_internalChangeStreamCheckResumability`
- `$_internalChangeStreamInjectControlEvents` (only present in v2 change streams; converts data
  placement change events into internal control events)
- `$_internalChangeStreamAddPreImage` (only present if `fullDocumentBeforeChange` is not set to
  `off`)
- `$_internalChangeStreamAddPostImage` (only present if `fullDocument` is not set to `default`)
- user-defined `$match` expression (only present if the user's change stream pipeline contains a
  `$match` stage)
- user-defined `$project` expression (only present if the change stream pipeline contains a
  `$project` stage)
- `$_internalChangeStreamSplitLargeEvent` (only present if the change stream is opened with the
  `$changeStreamSplitLargeEvent` pipeline step)

#### mongos Merge Pipeline

The merge pipeline on _mongos_ will look like this:

- `$mergeCursors`
- `$_internalChangeStreamHandleTopologyChangeV2` (v2, default) _or_
  `$_internalChangeStreamHandleTopologyChange` (v1, legacy)
- `$_internalChangeStreamEnsureResumeTokenPresent` (only present if the change stream resume token
  is not a high water mark token)

The pipeline split point is above the topology-change handler stage. _mongos_ also adds a
`$mergeCursors` stage that aggregates the responses from different shards (and in some cases the
config server) into a single, sorted stream.

Additionally, the change stream pipeline on a sharded cluster will contain a `$match` stage to
filter out all non-DML change events in case `showExpandedEvents` is not set.

### Details of individual Pipeline Stages

#### `$_internalChangeStreamOplogMatch`

This stage is responsible for reading data from the oplog and filtering out irrelevant events. The
`DocumentSourceChangeStreamOplogMatch` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_oplog_match.h#L61).
The oplog filter for the stage is built
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_oplog_match.cpp#L79).

There is no `Stage` equivalent for `DocumentSourceChangeStreamOplogMatch`, as it will be turned into
a `$cursor` stage for execution.

#### `$_internalChangeStreamUnwindTransaction`

This stage is responsible for "unwinding" (expanding) multiple operations that are contained in an
"applyOps" oplog entry into individual events. The `DocumentSourceChangeStreamUnwindTransaction`
code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_unwind_transaction.h#L71).
The `ChangeStreamUnwindTransactionStage` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_unwind_transaction.cpp#L83).

#### `$_internalChangeStreamTransform`

This stage is responsible for converting oplog entries into change events. It will build a change
event document for every oplog entry that enters this stage. Event fields are added based on the
change stream configuration. The `DocumentSourceChangeStreamTransform` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_transform.h#L60).
The `ChangeStreamTransformStage` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_transform_stage.cpp#L75).
The actual event transformation happens inside `ChangeStreamDefaultEventTransformation`
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/change_stream_event_transform.cpp#L321).

#### `$_internalChangeStreamCheckInvalidate`

This stage is responsible for creating change stream "invalidate" events and is only added for
collection-level and database-level change streams. The `DocumentSourceChangeStreamCheckInvalidate`
code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_check_invalidate.h#L65).
The `ChangeStreamCheckInvalidate` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_check_invalidate_stage.cpp#L106).

When an invalidate event is encountered, the stage will first emit an "invalidate" event, and then
throws a `ChangeStreamInvalidated` exception on the next call. The
[`ChangeStreamInvalidatedInfo`](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/change_stream_invalidation_info.h#L47).
exception type contains the error code `ChangeStreamInvalidated`.

#### `$_internalChangeStreamCheckResumability`

This stage checks if the oplog has enough history to resume the change stream, and consumes all
events up to the given resume point. If no data for the resume point can be found in the oplog
anymore, it will throw a `ChangeStreamHistoryLost` error.

The `DocumentSourceChangeStreamCheckResumability` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_check_resumability.h#L79).
The `ChangeStreamCheckResumabilityStage` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_check_resumability_stage.cpp#L68).

#### `$_internalChangeStreamAddPreImage`

This stage is responsible for adding pre-image data to "update", "replace" and "delete" events. It
is only added to change stream pipelines if the `fullDocumentBeforeChange` parameter is not set to
`off`. If enabled, the stage relies on the pre-images stored in the system's pre-image system
collection.

The `DocumentSourceChangeStreamAddPreImage` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_add_pre_image.h#L67).
The `ChangeStreamAddPreImageStage` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_add_pre_image_stage.cpp#L67).

#### `$_internalChangeStreamAddPostImage`

This stage is responsible for adding post-image data to "update" events. It is only added to change
stream pipelines if the `fullDocument` parameter is not set to `default`.

If `fullDocument` is set to `updateLookup`, the stage will perform a lookup for the current version
of a document that was updated by an "update" event, and store it in the `fullDocument` field of the
"update" event if present. The lookup is performed using the `_id` value of the document from the
change event. As the lookup is executed at a different point in time than when the change event was
recorded, it is possible that the lookup finds a different version of the document than the one that
was active when the change event was recorded. This can happen if the document was updated again
between the change event and the lookup. The lookup may also find no document at all if the document
was deleted after the "update" event, but before the lookup. In case the lookup cannot find a
document with the requested `_id`, it will populate the `fullDocument` field with a value of `null`.

If `fullDocument` is set to `whenAvailable` or `required`, the stage will make use of the stored
pre-image of the document in the system's pre-image system collection. It will fetch the pre-image
and then apply the delta that is stored in the "update" change event on top of it, and store the
result in the `fullDocument` field.

The `DocumentSourceChangeStreamAddPostImage` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_add_post_image.h#L63).
The `ChangeStreamAddPostImageStage` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_add_post_image_stage.cpp#L84).

#### `$_internalChangeStreamEnsureResumeTokenPresent`

This stage is used by change streams to ensure that the resume token that was specified as part of
the change stream parameters is actually in the stream. The stage is only present if the change
stream resume token is not a high water mark token. If the resume token cannot be found in the
stream, it will throw a `ChangeStreamFatalError`.

The `DocumentSourceChangeStreamEnsureResumeTokenPresent` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h#L51).
The `ChangeStreamEnsureResumeTokenPresent` code is
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_ensure_resume_token_present_stage.cpp#L67).

#### `$_internalChangeStreamHandleTopologyChange`

This stage is only present in v1 sharded cluster change streams and is always part of the _mongos_
merge pipeline. The stage is responsible for opening additional cursors to shards that have been
added to the cluster. It handles "insert" events into the `config.shards` collection that were
observed from the config server. In v2 change streams, this stage is replaced by
`$_internalChangeStreamHandleTopologyChangeV2`.

The `DocumentSourceChangeStreamHandleTopologyChange` code can be found
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/pipeline/document_source_change_stream_handle_topology_change.h#L63).
The `ChangeStreamHandleTopologyChangeStage` code can be found
[here](https://github.com/mongodb/mongo/blob/eb4c6148f6a25c444be39a0e330506834526d935/src/mongo/db/exec/agg/change_stream_handle_topology_change_stage.cpp#L121).

#### `$_internalChangeStreamHandleTopologyChangeV2`

This stage is only present in v2 sharded cluster change streams and is always part of the _mongos_
merge pipeline. It replaces the `$_internalChangeStreamHandleTopologyChange` stage used by v1 change
streams. Rather than tracking additions to `config.shards`, this stage processes _control events_,
and dynamically opens or closes remote shard cursors as data placement changes are detected. This
enables the precise shard targeting that distinguishes v2 from v1 change streams.

The `DocumentSourceChangeStreamHandleTopologyChangeV2` code can be found
[here](https://github.com/mongodb/mongo/blob/329eda6d27129c10a4c0aedb261fd83c5e291516/src/mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2.h#L55).
The `ChangeStreamHandleTopologyChangeV2Stage` code can be found
[here](https://github.com/mongodb/mongo/blob/329eda6d27129c10a4c0aedb261fd83c5e291516/src/mongo/db/exec/agg/change_stream_handle_topology_change_v2_stage.h#L63).

#### `$_internalChangeStreamInjectControlEvents`

This stage is only present in v2 sharded cluster change streams and runs on each data shard as part
of the shard pipeline (i.e., it appears between `$_internalChangeStreamCheckResumability` and
`$_internalChangeStreamAddPreImage`). Its role is to intercept specific no-op oplog entries that
carry placement change notifications and covert them into internal _control events_. These oplog
entries are written on the config server or participating shards when a DDL operation modifies data
placement.

The following oplog entry types are handled on the config server:

- `insert` entries for the `config.databases` collection (also called 'DatabaseCreated' internally)
- `namespacePlacementChanged` entries

On data-bearing shards, the following oplog entry types are handled:

- `moveChunk`
- `movePrimary`
- `namespacePlacementChanged`

Control events are not visible to change stream consumers. They are forwarded from the data shards
to the `$_internalChangeStreamHandleTopologyChangeV2` stage on _mongos_, which uses the placement
information they carry to adjust cursor targeting. After processing a control event, _mongos_ either
opens new shard cursors (for shards that have gained relevant data) or closes existing shard cursors
(for shards that have lost all relevant data).

The `DocumentSourceChangeStreamInjectControlEvents` code can be found
[here](https://github.com/mongodb/mongo/blob/329eda6d27129c10a4c0aedb261fd83c5e291516/src/mongo/db/pipeline/document_source_change_stream_inject_control_events.h#L61).

## Missing documentation (to be completed)

- How are user-defined match expressions are handled, rewritten and pushed down.
