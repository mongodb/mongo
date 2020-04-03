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
_put terms here that can either be briefly explained in order to be simply referenced by above sections; or terms with links to sections for complete explanation so topics can be found quickly when not obviously covered by a section_
