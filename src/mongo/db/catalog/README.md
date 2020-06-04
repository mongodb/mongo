# Execution Internals
_intro to execution goes here_

# The Catalog

## In-Memory Catalog

### Collection Catalog
include discussion of RecordStore interface

### Index Catalog
include discussion of SortedDataInferface interface

### Versioning
in memory versioning (or lack thereof) is separate from on disk

#### The Minimum Visible Snapshot

## On-Disk Catalog
Discuss what the catalog looks like on disk -- e.g. just another WT data table we structure specially

### Catalog Data Formats
What do the catalog documents look like? Are there catalog concepts constructed only in-memory and not on disk?

#### Collection Data Format

#### Index Data Format

### Versioning
e.g. data changes in tables are versioned, dropping/creating tables is not versioned

## Catalog Changes
How are updates to the catalog done in-memory and on disk?

## Two-Phase Collection and Index Drop
First phase removes access to the collection/index in the in-memory catalog, second phase drops the WT table.
Explain this is necessary because WT versions document writes but not table drops.
Necessary to support ongoing queries, repl ops like rollback, and our own startup recovery.

# Storage Transactions
Clarify transaction refers to storage engine transactions, not repl or sharding, throughout this document.

Include a discussion on how RecoveryUnit implements isolation and transactional behaviors, including ‘read source’ and how those implement read concern levels.
Maybe include a discussion of how MongoDB read concerns translate into particular read sources and data views.

## WiredTiger Snapshot

## MongoDB Point-in-Time Read

# Read Operations
How does a read work?

## Collection Read
how it works, what tables

## Index Read
_could pull out index reads and writes into its own section, if preferable_

how it works, goes from index table to collection table -- two lookups

# Write Operations
an overview of how writes (insert, update, delete) are processed

## Index Writes
_could pull out index reads and writes into its own section, if preferable_

how index tables also get updated when a write happens, (numIndexes + 1) writes total

## Vectored Insert

# Concurrency Control
We have the catalog described above; now how do we protect it?

## Lock Modes

## Lock Granularity
Different storage engines can support different levels of granularity.

### Lock Acquisition Order
discuss lock acquisition order

mention risk of deadlocks motivation

### Replication State Transition Lock (RSTL)

### Parallel Batch Writer Mode Lock (PBWM)

### Global Lock

### Database Lock

### Collection Lock

### Document Level Concurrency Control
Explain WT's optimistic concurrency control, and why we do not need document locks in the MongoDB layer.

### Mutexes

### FCV Lock

## Two-Phase Locking
We use this for transactions? Explain.

## Replica Set Transaction Locking
TBD: title of this section -- there is some confusion over what terminology will be best understood
Stashing and unstashing locks for replica set level transactions across multiple statements.
Read's IS locks are converted to IX locks in replica set transactions.

## Locking Best Practices

### Network Calls
i.e., never hold a lock across a network call unless absolutely necessary

### Long Running I/O
i.e., don't hold a lock across journal flushing

### FCV Lock Usage

# Index Builds
How do indexs work?

Read collection table, sort in-memory, write to index table.

## Hybrid Index Build
KeyString has its own section below. Index builds discussion may need to reference it

### Temporary Side Table For New Writes

### Temporary Table For Transient Conflicts

## Index Locks

## Multikey Indexes

## Cross-Replica Set Index Builds

# KeyString
describe how KeyString values are used for comparing and indexing BSON elements

# The External Sorter
describe how it works; indexing and query sort stages use it

# Repair

Data corruption has a variety of causes, but can usually be attributed to misconfigured or unreliable I/O subsystems that do not make data durable when called upon, often in the event of power outages.

MongoDB provides a command-line `--repair` utility that attempts to recover as much data as possible from an installation that fails to start up due to data corruption.

- [Types of Corruption](#types-of-corruption)
- [Repair Procedure](#repair-procedure)

## Types of Corruption

MongoDB repair attempts to address the following forms of corruption:

* Corrupt WiredTiger data files
  * Includes all collections, `_mdb_catalog`, and `sizeStorer`
* Missing WiredTiger data files
  * Includes all collections, `_mdb_catalog`, and `sizeStorer`
* Indexes
  * Prior to 4.4, all indexes were always rebuilt on all collections, even if not missing or corrupt.
  * Starting in 4.4, indexes are only rebuilt on collections that are salvaged or fail validation with inconsistencies. See [repairCollections](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database.cpp#L115).
* Unsalvageable collection data files
* Corrupt metadata
    * `WiredTiger.wt`, `WiredTiger.turtle`, and WT journal files
* “Orphaned” data files
    * Collection files missing from the `WiredTiger.wt` metadata
    * Collection files missing from the `_mdb_catalog` table
    * We cannot support restoring orphaned files that are missing from both metadata sources
* Missing `featureCompatibilityVersion` document

## Repair Procedure

1. Initialize the WiredTigerKVEngine. If a call to `wiredtiger_open` returns the `WT_TRY_SALVAGE` error code, this indicates there is some form of corruption in the WiredTiger metadata. Attempt to [salvage the metadata](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1046-L1071) by using the WiredTiger `salvage=true` configuration option.
2. Initialize the StorageEngine and [salvage the `_mdb_catalog` table, if needed](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L95).
3. Recover orphaned collections.
    * If an [ident](#ident) is known to WiredTiger but is not present in the `_mdb_catalog`, [create a new collection](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L145-L189) with the prefix `local.orphan.<ident-name>` that references this ident.
    * If an ident is present in the `_mdb_catalog` but not known to WiredTiger, [attempt to recover the ident](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L197-L229). This [procedure for orphan recovery](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1525-L1605) is a less reliable and more invasive. It involves moving the corrupt data file to a temporary file, creates a new table with the same name, replaces the original data file over the new one, and [salvages](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1525-L1605) the table in attempt to reconstruct the table.
4. [Verify collection data files](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1195-L1226), and salvage if necessary.
    *  If call to WiredTiger [verify()](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a0334da4c85fe8af4197c9a7de27467d3) fails, call [salvage()](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#ab3399430e474f7005bd5ea20e6ec7a8e), which recovers as much data from a WT data file as possible.
    * If a salvage is unsuccessful, rename the data file with a `.corrupt` suffix.
    * If a data file is missing or a salvage was unsuccessful, [drop the original table from the metadata, and create a new, empty table](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1262-L1274) under the original name. This allows MongoDB to continue to start up despite present corruption.
    * After any salvage operation, [all indexes are rebuilt](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database.cpp#L134-L149) for that collection.
5. Validate collection and index consistency.
    * [Collection validation](#collection-validation) checks for consistency between the collection and indexes. If any inconsistencies are found, [all indexes are rebuilt](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database.cpp#L167-L184).
6. [Invalidate the replica set configuration](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database_and_check_version.cpp#L460-L485) if data has been or could have been modified. This [prevents a repaired node from joining](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repl/replication_coordinator_impl.cpp#L486-L494) and threatening the consisency of its replica set.

Additionally:
* When repair starts, it creates a temporary file, `_repair_incomplete` that is only removed when repair completes. The server [will not start up normally](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_init.cpp#L82-L86) as long as this file is present.
* Repair [will restore a missing](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database_and_check_version.cpp#L434) `featureCompatibilityVersion` document in the `admin.system.version` to the lower FCV version available.

# Startup Recovery
How the different storage engines startup and recovery

Any differences for standalone mode?

## Recovery To A Stable Timestamp

## Table Ident Resolution

# File-System Backups

## How To Take a Backup

## How To Use Backed Up Datafiles
describe the different ways backed up datafiles can be used

explain how datafiles persist a machine’s identity which must be manipulated for some kinds of restores

## Replica Set Backup

## Sharding Backup

## Queryable Backup (Read-Only)

# Checkpoints

# Journaling
Durability can be guaranteed by simply flushing the journal to disk, not the writes themselves.
What we do when journaling is disabled -- take full data checkpoints.
Periodically flush to ensure minimal loss of data.

# Flow Control
What it does (motivation). How does it do it? Ticketing.

# Collection Validation
How we check that the data has not been corrupt, by ourselves or other parties involved in the system.

# Oplog Collection

## Purpose
‘an operations log’, entry for every write op, repl uses it, rollback, recovery, etc.

## Oplog Visibility

### Oplog ‘Holes’
because parallel writes

### Oplog Read Timestamp
only used for forward oplog cursors, backwards skips

## Oplog Truncation
capped collection or WT oplog stones

special timestamps we will not truncate with WT -- for which we delay truncation

new min oplog time retention, helps not fall off of the oplog

oplog durability considerations across nodes

# Glossary

## ident

And ident is a unique identifier given to a storage engine resource. Collections and indexes map application-layer names to storage engine idents. In WiredTiger, idents are implemented as tables. For example, collection idents have the form: `collection-<counter>-<random number>`.
