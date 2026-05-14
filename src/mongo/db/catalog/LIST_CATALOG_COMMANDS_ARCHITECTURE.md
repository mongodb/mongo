# List-Catalog Commands Architecture

This document describes the architecture of the catalog-listing commands
exposed by `mongod` and `mongos`: `listDatabases`, `listCollections`,
`listIndexes`, and the aggregation stage `$listClusterCatalog`. It covers
what each command surfaces, where the data comes from, and which
read-concern / time-travel options each command supports.

## Commands at a glance

- **`listDatabases`** returns the set of databases visible on the target
  node. On a `mongod`, the source of truth is the local
  `CollectionCatalog`; on a `mongos`, results are gathered by routing to
  the appropriate shards via `ShardRegistry` and merging.
- **`listCollections`** returns the collections and views inside a single
  database. On a shard, the local `CollectionCatalog` is consulted; on a
  router, the command is dispatched to the database-primary shard, which
  owns the authoritative collection list for unsharded namespaces and
  consults the sharding catalog (`config.collections`) for sharded ones.
- **`listIndexes`** returns the index specs of a single collection. The
  authoritative source is the durable catalog (`_mdb_catalog`) reflected
  through the in-memory `CollectionCatalog`; on a router, the command is
  forwarded to one shard owning data for the namespace.
- **`$listClusterCatalog`** is an aggregation stage that streams a unified
  view of every collection in the cluster, joining shard-local catalog
  state with the global sharding catalog. It is used by tooling that
  needs a single pass over "every namespace in the deployment".

## Read-concern and time-travel support

| Command                | local | majority | snapshot | afterClusterTime |
| ---------------------- | :---: | :------: | :------: | :--------------: |
| `listDatabases`        |   Y   |    Y     |    N     |        N         |
| `listCollections`      |   Y   |    Y     |    N     |        N         |
| `listIndexes`          |   Y   |    Y     |    N     |        N         |
| `$listClusterCatalog`  |   Y   |    Y     |    N     |        N         |

All four commands read from the in-memory `CollectionCatalog`, which is
not multi-versioned across cluster time. As a consequence, none of the
commands accept `snapshot` read concern or `afterClusterTime`; requests
that specify either are rejected at command-parsing time. `local` and
`majority` read concern are both honored: with `majority`, the catalog
read is gated on the most recently majority-committed catalog mutation
having been applied locally before results are returned.

## State diagram

The flow below summarizes how a list-catalog request is dispatched and
where it terminates. `R` denotes a router (`mongos` or embedded router
role); `P` denotes the database-primary shard; `S` denotes any shard
hosting collection data.

```
                +-----------------------------+
                |  Client list-catalog request|
                +--------------+--------------+
                               |
                       parse + validate
                       (reject snapshot /
                        afterClusterTime)
                               |
                               v
                  +------------+------------+
                  |  Running on mongos?     |
                  +---+-----------------+---+
                      |yes              |no
                      v                 v
        +-------------+----+    +-------+-----------+
        | Dispatch by cmd  |    | Resolve namespace |
        |                  |    | locally           |
        | listDatabases:   |    +-------+-----------+
        |   fan-out -> S*  |            |
        | listCollections: |            v
        |   -> P           |    +-------+-----------+
        | listIndexes:     |    | Read in-memory    |
        |   -> S owning ns |    | CollectionCatalog |
        | $listClusterCat: |    | (durable catalog  |
        |   fan-out -> S*  |    |  for listIndexes) |
        +---------+--------+    +-------+-----------+
                  |                     |
                  v                     v
        +---------+--------+    +-------+-----------+
        | Merge shard      |    | Apply read concern|
        | responses        |    | gate (local /     |
        | (dedupe DBs,     |    |  majority wait)   |
        |  union namespaces|    +-------+-----------+
        +---------+--------+            |
                  |                     |
                  +----------+----------+
                             |
                             v
                  +----------+-----------+
                  | Cursor batch to caller|
                  +----------------------+
```

## Implementation notes

- The `CollectionCatalog` is a snapshot-stable in-memory structure but is
  not versioned across replication time; this is why `afterClusterTime`
  and `snapshot` are unsupported. Adding either would require multi-
  version catalog state or a stash mechanism on top of WiredTiger's
  durable history, neither of which is in scope today.
- `listIndexes` and `listCollections` are cursor-producing commands and
  honor `batchSize`; `listDatabases` returns a single document.
- `$listClusterCatalog` is the only one of the four that crosses
  database boundaries in a single invocation and is the recommended path
  for `mongosync` and similar bulk-introspection tooling.

See also: `src/mongo/db/global_catalog/`, `src/mongo/db/shard_role/`,
`src/mongo/db/commands/list_*.cpp`.
