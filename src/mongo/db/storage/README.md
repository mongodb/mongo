Storage Engine API
==================

The purpose of the Storage Engine API is to allow for pluggable storage engines in MongoDB, see
also the [Storage FAQ][].  This document gives a brief overview of the API, and provides pointers
to places with more detailed documentation. Where referencing code, links are to the version that
was current at the time when the reference was made. Always compare with the latest version for
changes not yet reflected here.  For questions on the API that are not addressed by this material,
use the [mongodb-dev][] Google group. Everybody involved in the Storage Engine API will read your
post.

Third-party storage engines are integrated through self-contained modules that can be dropped into
an existing MongoDB source tree, and will be automatically configured and included. A typical
module would at least have the following files:

    src/             Directory with the actual source files
    README.md        Information specific to the storage engine
    SConscript       Scons build rules
    build.py         Module configuration script

See <https://github.com/mongodb-partners/mongo-rocks> for a good example of the structure.


Concepts
--------

### Record Stores
A database contains one or more collections, each with a number of indexes, and a catalog listing
them. All MongoDB collections are implemented with record stores: one for the documents themselves,
and one for each index. By using the KVEngine class, you only have to deal with the abstraction, as
the KVStorageEngine implements the StorageEngine interface, using record stores for catalogs and
indexes.

#### Record Identities
A RecordId is a unique identifier, assigned by the storage engine, for a specific document or entry
in a record store at a given time. For storage engines based in the KVEngine the record identity is
fixed, but other storage engines, such as MMAPv1, may change it when updating a document. Note that
this can be very expensive, as indexes map to the RecordId. A single document with a large array
may have thousands of index entries, resulting in very expensive updates.

#### Cloning and bulk operations
Currently all cloning, [initial sync][] and other operations are done in terms of operating on
individual documents, though there is a BulkBuilder class for more efficiently building indexes.

### Locking and Concurrency
MongoDB uses multi-granular intent locking, see the [Concurrency FAQ][]. In all cases, this will
ensure that operations to meta-data, such as creation and deletion of record stores, are serialized
with respect to other accesses. Storage engines can choose to support document-level concurrency,
in which case the storage engine is responsible for any additional synchronization necessary. For
storage engines not supporting document-level concurrency, MongoDB will use shared/exclusive locks
at the collection level, so all record store accesses will be serialized.

MongoDB uses [two-phase locking][] (2PL) to guarantee serializability of accesses to resources it
manages. For storage engines that support document level concurrency, MongoDB will only use intent
locks for the most common operations, leaving synchronization at the record store layer up to the
storage engine.

### Transactions
Each operation creates an OperationContext with a new RecoveryUnit, implemented by the storage
engine, that lives until the  operation finishes. Currently, query operations that return a cursor
to the client live as long as that client cursor, with the operation context switching between its
own recovery unit and that of the client cursor. In a few other cases an internal command may use
an extra recovery unit as well. The recovery unit must implement transaction semantics as described
below.

#### Atomicity
Writes must only become visible when explicitly committed, and in that case all pending writes
become visible atomically. Writes that are not committed before the unit of work ends must be
rolled back. In addition to writes done directly through the Storage API, such as document updates
and creation of record stores, other custom changes can be registered with the recovery unit.

#### Consistency
Storage engines must ensure that atomicity and isolation guarantees span all record stores, as
otherwise the guarantee of atomic updates on a document and all its indexes would be violated.

#### Isolation
Storage engines must provide snapshot isolation, either through locking (as is the case for the
MMAPv1 engine), through multi-version concurrency control (MVCC) or otherwise. The first read
implicitly establishes the snapshot. Operations can always see all changes they make in the context
of a recovery unit, but other operations cannot until a successfull commit.

#### Durability
Once a transaction is committed, it is not necessarily durable: if, and only if the server fails,
as result of power loss or otherwise, the database may recover to an earlier point in time.
However, atomicity of transactions must remain preserved. Similarly, in a replica set, a primary
that becomes unavailable may need to rollback to an earlier state when rejoining the replicas et,
if its changes were not yet seen by a majority of nodes. The RecoveryUnit implements methods to
allow operations to wait for their committed transactions to become durable.

A transaction may become visible to other transactions as soon as it commits, and a storage engine
may use a group commit bundling a number of transactions to achieve durability. Alternatively, a
storage engine may wait for durability at commit time.

### Write Conflicts
Systems with optimistic concurrency control (OCC) or multi-version concurrency control (MVCC) may
find that a transaction conflicts with other transactions, that executing an operation would result
in deadlock or violate other resource constraints. In such cases the storage engine may throw a
WriteConflictException to signal the transient failure. MongoDB will handle the exception, abort
and restart the transaction.


Classes to implement
--------------------

A storage engine should generally implement the following classes. See their definition for more
details.

* [KVEngine](kv/kv_engine.h)
* [RecordStore](record_store.h)
* [RecoveryUnit](ecovery_unit.h)
* [SeekableRecordCursor](record_store.h)
* [SortedDataInterface](sorted_data_interface.h)
* [ServerStatusSection](../commands/server_status.h)
* [ServerParameter](../server_parameters.h)


[Concurrency FAQ]: http://docs.mongodb.org/manual/faq/concurrency/
[initial sync]: http://docs.mongodb.org/manual/core/replica-set-sync/#replica-set-initial-sync
[mongodb-dev]: https://groups.google.com/forum/#!forum/mongodb-dev
[replica set]: http://docs.mongodb.org/manual/replication/
[Storage FAQ]: http://docs.mongodb.org/manual/faq/storage
[two-phase locking]: http://en.wikipedia.org/wiki/Two-phase_locking
