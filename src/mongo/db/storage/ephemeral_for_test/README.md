# Ephemeral For Test Storage Engine

The primary goal of this [storage engine](#ephemeral-storage-engine-glossary) is to provide a simple
and efficient in-memory storage engine implementation for use in testing. The lack of I/O,
background threads, complex concurrency primitives or third-party libraries, allows for easy
integration in unit tests, compatibility with advanced sanitizers, such as
[TSAN](https://clang.llvm.org/docs/ThreadSanitizer.html). A secondary goal is to strengthen the
Storage API, its modularity and our understanding of it, by having an alternate implementation that
can be used as testbed for new ideas.

## Architecture

The architecture of the storage engine has parallels to and is in part inspired by the `git` version
control system and borrows some of its terminology.

## Radix Store

The core of this storage engine is a [radix tree](https://en.wikipedia.org/wiki/Radix_tree). Our
`RadixStore` implementation uses [copy on write](https://en.wikipedia.org/wiki/Copy-on-write) to
make the data structure [persistent](https://en.wikipedia.org/wiki/Persistent_data_structure). This
is implemented using reference-counted pointers.

### Transactions on the Radix Store

The most recent committed state of the store is called the _master_. Starting a transaction involves
making a [shallow copy](https://en.wikipedia.org/wiki/Object_copying#Shallow_copy) of the master,
where we copy the head node in full, but share the children with the original. This copy is known as
the _base_ of the transaction. Another copy of this base is referred to as the _current_ state. Each
modification of the tree updates this _current_ state. A transaction abort consists of simply
discarding these copies.

A transaction commit is trivial if the master still compares equal to the base: just swap the
current state into master. In other cases, it is necessary to do a [three-way
merge](https://en.wikipedia.org/wiki/Merge_(version_control)#Three-way_merge) to advance the base to
master. Any merge conflicts result in a `WriteConflictException` and transaction abort and requires
the operation to execute again. As long as there is no merge conflict, the current tree has
incorporated all changes that occurred in the master tree and the
[compare-and-swap](https://en.wikipedia.org/wiki/Compare-and-swap) loop can repeat with an updated
base to reflect this. This scheme ensures forward progress and makes the commit protocol [lock
free](https://en.wikipedia.org/wiki/Non-blocking_algorithm#Lock-freedom). Because merging compares
values using pointer comparison, and both old and new values of any modified key-value pairs still
exist, there is no [ABA problem](https://en.wikipedia.org/wiki/Compare-and-swap#ABA_problem).

## Changing Nodes

Modification of a node N requires making a copy N' of that node, as well as all its parents up
to the root R of the tree, as they need to change to point to N'. The tree referenced by R is
immutable, and removing the last reference to R causes that root node to be deallocated, as well
as recursively any nodes unique to that tree. The root node of the tree has some extra information,
namely a `_count` and `_datasize`, which contain the total number of key-value pairs in the tree as
well as the total size in bytes of all key-value pairs.

### Iterators

The radix tree is an ordered data structure and traversal of the tree is done with depth-first
search. A `radix_iterator` has a shared reference to the root of the tree as well as a pointer
to the current node. Currently, on successful commit, the new root of the tree automatically adds
itself to the `_nextVersion` link of the previous master thus establishing a linked list of
committed versions. Iterators use this list to move themselves to a newer version when available.
These versions are only versions of the tree made by the same client. This is needed so the client's
iterators can see their own write.


## KVEngine implementation

### _idents_ and prefixed keys

The storage engine API assumes there are many logical tables containing key-value pairs. These
tables are called [_idents_](#ephemeral-storage-engine-glossary). However, to support transactions
with changes spanning multiple idents, this storage engine uses a single radix store and prefixes
each key with the ident, so that in effect each ident has its own branch in the radix tree.

### `RecoveryUnit`

The RecoveryUnit is the abstraction of how transactions are implemented on the storage engine as
described [_above_](#transactions-on-the-radix-store). It is responsible for creating a local fork
of the radix tree and merge in any changes to the master tree on commit. When a transaction is
rolled back the RecoveryUnit can simply release its reference to the forked radix store. This
guarantees
[_atomicity_](https://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/README.md#atomicity)
on the storage engine. 

### `RecordStore`

The RecordStore is the abstraction on how to read or write data with the storage engine. It is also
responsible for implementing the oplog collection with the correct semantics, this is handled in the
[VisibilityManager](#visibilitymanager). The RecordStore accesses the radix store via the
[_RecoveryUnit_](#recoveryunit), this ensures
[_isolation_](https://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/README.md#isolation)
on the storage engine.

### `VisibilityManager`

The visibility manager is a system internal to the storage engine to keep track of uncommitted
inserts and reserved timestamps to implement [_oplog
visibility_](https://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/README.md#oplog-visibility).

### `SortedDataInterface`

The SortedDataInterface is the implementation for indexes. Indexes are implemented on top of the
radix store like any other [_idents_](#ephemeral-storage-engine-glossary) where the data contains
the necessary information to be able to find a record belonging to another ident without searching
for it. There are two types of indexes in the implementation, unique and non-unique. Both index
types may contain duplicate entries but are optimized for their regular use-case.

# Ephemeral Storage Engine Glossary

**ident**: Name uniquely identifying a table containing key-value pairs. Idents are not reused.

**sanitizer**: Testing tool for executing code with extra checks, often through the use of special
run time libraries and/or compiled code generation.

**storage engine**: Low-level database component providing transactional storage primitives
including create, read, update and delete (CRUD) operations on key-value pairs.

**master**: The master radix store contains the last committed transaction.

**base**: The radix store containing the master at the start of the transaction.

**path compression**: The technique to ensure that each node without value in a radix tree has at
least two children.

**storage transaction**: A series of CRUD operations performed on behalf of a client that remain
invisible to other clients (_isolation property_), until they are made visible(_committed_) or
undone (_aborted_), all at once (atomicity property).  Pre-commit checks ensure the operations do
not violate logical constraints (_consistency property_). Once committed, the changes will not go
away unless the node fails (_durability property_). Together these four transaction properties are
known as the ACID properties.
