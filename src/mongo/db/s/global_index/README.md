# Global Index internals

Global indexes allow for enforcing uniqueness constraints on any key or combination of keys on a
sharded collection. Shards can only ever enforce uniqueness locally, but local uniqueness
enforcement gives rise to global uniqueness enforcement when the shard key pattern is a prefix of
the unique index's key pattern. One mental model of a global index is they are a second copy of the
sharded collection contents and sharded on the index key pattern. In consequence, just like in
regular sharded collections, local uniqueness enforcement within the global index gives rise to
global uniqueness enforcement.

# Catalog

Unlike local indexes, global indexes are maintained in the sharding catalog, and are known to the
entire cluster rather than individually by each shard. The sharding catalog is responsible for
mapping a [base collection](#glossary) to its global indexes and vice-versa, for storing the index
specifications, and for routing index key writes to the owning shards. TODO (SERVER-60626) expand
this section.

The global index keys are stored locally in a shard in an internal system collection referred
to as the [global index container](#glossary). Unlike local index tables, a global index container
has a UUID and a namespace. The namespace is `system.globalIndex.<uuid>`, where
`<uuid>` is the UUID of the global index. The same UUID is used as the UUID of the internal system
collection.

# Storage format

Sample index entry:

```
{_id: {shk1: 1 .. shkN: 1, _id: 1}, ik: BinData(KeyString({ik1: 1, .. ikN: 1})), tb: BinData(TypeBits({ik1: 1, .. ikN: 1}))}
```

* Top-level `_id` is the [document key](#glossary).
* `ik` is the index key. The key is stored in its [KeyString](https://github.com/mongodb/mongo/blob/dab0694cd327eb0f7e540de5dee97c69f84ea45d/src/mongo/db/catalog/README.md#keystring)
  form without [TypeBits](https://github.com/mongodb/mongo/blob/dab0694cd327eb0f7e540de5dee97c69f84ea45d/src/mongo/db/catalog/README.md#typebits), as BSON binary data with subtype 0 (Generic binary subtype).
* `tb` are the [TypeBits](https://github.com/mongodb/mongo/blob/dab0694cd327eb0f7e540de5dee97c69f84ea45d/src/mongo/db/catalog/README.md#typebits).
  This field is only present when not empty, and is stored as BSON binary data with subtype 0
  (Generic binary subtype).

The global index collection is [clustered](https://github.com/mongodb/mongo/blob/dab0694cd327eb0f7e540de5dee97c69f84ea45d/src/mongo/db/catalog/README.md#clustered-collections)
by `_id`, it has a local unique secondary index on `ik` and is planned to be sharded by `ik`.

# Storage execution

The storage layer provides an API for participant shards to [create](https://github.com/mongodb/mongo/blob/872b5054b7b434c22adcabfb990188eebb89090f/src/mongo/s/request_types/sharded_ddl_commands.idl#L283),
[drop](https://github.com/mongodb/mongo/blob/872b5054b7b434c22adcabfb990188eebb89090f/src/mongo/s/request_types/sharded_ddl_commands.idl#L293),
[insert](https://github.com/mongodb/mongo/blob/872b5054b7b434c22adcabfb990188eebb89090f/src/mongo/db/s/global_index_crud_commands.idl#L51)
and [delete](https://github.com/mongodb/mongo/blob/872b5054b7b434c22adcabfb990188eebb89090f/src/mongo/db/s/global_index_crud_commands.idl#L64)
keys to global index containers. It also allows for sending multiple key insert and delete
statements in [bulk](https://github.com/mongodb/mongo/blob/872b5054b7b434c22adcabfb990188eebb89090f/src/mongo/db/s/global_index_crud_commands.idl#L77)
to reduce the number of round trips.

DDL operations replicate as `createGlobalIndex` and `dropGlobalIndex` command types, and generate
`createIndex` and `dropIndex` change stream events like local indexes.

Key insert and delete operations replicate as `xi` and `xd` CRUD types and do not generate
change stream events. On a secondary, these entries are applied in parallel with other CRUD
operations, and serialized based on the container's UUID and the entry's document key.
# DDL operations
TODO (SERVER-65567)

# Index builds
TODO (SERVER-65618)
# Maintenance of a built index
TODO (SERVER-65513)

# Glossary
**Global index container**: the internal system collection that stores the range of keys owned by
the shard for a specific global index.

**Base collection**: the user collection the global index belongs to.

**Document key**: the key that uniquely identifies a document in the base collection. It is composed
  of the _id value of the base collections's document followed by the shard key value(s) of the
  base collection's document.
