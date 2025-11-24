# Catalog Terminology

- **Collection incarnation (aka namespace incarnation):** the lifetime of collection's namespace
  across drop/recreate/rename boundaries. The concept of incarnation is implemented via collection
  UUIDs.
- **Collection generation:** a superset of the collection incarnation. A collection reincarnation
  changes the collection generation; a collection regeneration does not necessarily change its
  incarnation. This includes cases where the collection distribution metadata changes, but the
  underlying schema does not. The `refineShardKey` command is an example. The concept of generation
  is implemented via the [placement versioning protocol](https://github.com/mongodb/mongo/blob/8a79395deff895f18b8878ff4567c9fb309a7c64/src/mongo/db/s/README_versioning_protocols.md#shard-version).
- **Collection instance:** an immutable version of a [`Collection` object](https://github.com/mongodb/mongo/blob/5157bf67f0dc75f6d5ea63cac8654a0517fcf516/src/mongo/db/catalog/README.md#collection-objects).
  A `Collection` object is a shard-local concept and is generally unaware of the data distribution.
  A DDL operation installs a new Collection instance at commit. A collection reincarnaton changes
  the latest collection instance.
