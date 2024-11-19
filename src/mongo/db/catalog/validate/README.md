# Collection Validation

Collection validation is used to check both the validity and integrity of the data, which in turn
informs us whether there’s any data corruption present in the collection at the time of execution.

There are two forms of validation, foreground and background.

- Foreground validation requires exclusive access to the collection which prevents CRUD operations
  from running. The benefit of this is that we're not validating a potentially stale snapshot and that
  allows us to perform corrective operations such as fixing the collection's fast count.

- Background validation runs lock-free on the collection and reads using a timestamp in
  order to have a consistent view across the collection and its indexes. This mode allows CRUD
  operations to be performed without being blocked.

Additionally, users can specify that they'd like to perform a `full` validation.

- Storage engines run custom validation hooks on the
  [RecordStore](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/record_store.h#L445-L451)
  and
  [SortedDataInterface](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/sorted_data_interface.h#L130-L135)
  as part of the storage interface.
- These hooks enable storage engines to perform internal data structure checks that MongoDB would
  otherwise not be able to perform.
- More comprehensive and time-consuming checks will run to detect more types of non-conformant BSON
  documents with duplicate field names, invalid UTF-8 characters, and non-decompressible BSON
  Columns.
- Full validations are not compatible with background validation.

[Public docs on how to run validation and interpret the results.](https://docs.mongodb.com/manual/reference/command/validate/)

## Types of Validation

- Verifies the collection's durable catalog entry and in-memory state match.
- Indexes are marked as [multikey](#multikey-indexes) correctly.
- Index [multikey](#multikey-indexes) paths cover all of the records in the `RecordStore`.
- Indexes are not missing [multikey](#multikey-indexes) metadata information.
- Index entries are in increasing order if the sort order is ascending.
- Index entries are in decreasing order if the sort order is descending.
- Unique indexes do not have duplicate keys.
- Documents in the collection are valid and conformant `BSON`.
- Fast count matches the number of records in the `RecordStore`.
  - For foreground validation only.
- The number of \_id index entries always matches the number of records in the `RecordStore`.
- The number of index entries for each index is not greater than the number of records in the record
  store.
  - Not checked for indexed arrays and wildcard indexes.
- The number of index entries for each index is not less than the number of records in the record
  store.
  - Not checked for sparse and partial indexes.
- Time-series bucket collections are valid.

## Validation Procedure

- Instantiates the objects used throughout the validation procedure.
  - [ValidateState](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_state.h)
    maintains the state for the collection being validated, such as locking, cursor management
    for the collection and each index, data throttling (for background validation), and general
    information about the collection.
  - [IndexConsistency](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/index_consistency.h)
    descendents keep track of the number of keys detected in the record store and indexes. Detects when there
    are index inconsistencies and maintains the information about the inconsistencies for
    reporting.
  - [ValidateAdaptor](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_adaptor.h)
    used to traverse the record store and indexes. Validates that the records seen are valid
    `BSON` conformant to most [BSON specifications](https://bsonspec.org/spec.html). In `full`
    and `checkBSONConformance` validation modes, all `BSON` checks, including the time-consuming
    ones, will be enabled.
- If a `full` validation was requested, we run the storage engines validation hooks at this point to
  allow a more thorough check to be performed.
- Validates the [collection’s in-memory](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/collection.h)
  state with the [durable catalog](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/durable_catalog.h#L242-L243)
  entry information to ensure there are [no mismatches](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/collection_validation.cpp#L363-L425)
  between the two.
- [Initializes all the cursors](https://github.com/mongodb/mongo/blob/07765dda62d4709cddc9506ea378c0d711791b57/src/mongo/db/catalog/validate_state.cpp#L144-L205)
  on the `RecordStore` and `SortedDataInterface` of each index in the `ValidateState` object.
  - We choose a read timestamp (`ReadSource`) based on the validation mode: `kNoTimestamp`
    for foreground validation and `kProvided` for background validation.
- Traverses the `RecordStore` using the `ValidateAdaptor` object.
  - [Validates each record and adds the document's index key set to the IndexConsistency objects](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_adaptor.cpp#L61-L140)
    for consistency checks at later stages.
    - In an effort to reduce the memory footprint of validation, the `IndexConsistency` objects
      [hashes](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/index_consistency.cpp#L307-L309)
      the keys (or paths) passed in to one of many buckets.
    - Document keys (or paths) will
      [increment](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/index_consistency.cpp#L204-L214)
      the respective bucket.
    - Index keys (paths) will
      [decrement](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/index_consistency.cpp#L239-L248)
      the respective bucket.
  - Checks that the `RecordId` is in [increasing order](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_adaptor.cpp#L305-L308).
  - [Adjusts the fast count](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_adaptor.cpp#L348-L353)
    stored in the `RecordStore` (when performing a foreground validation only).
- Traverses the index entries for each index in the collection.
  - [Validates the index key order to ensure that index entries are in increasing or decreasing order](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_adaptor.cpp#L144-L188).
  - Adds the index key to the `IndexConsistency` objects for consistency checks at later stages.
- After the traversals are finished, the `IndexConsistency` objects are checked to detect any
  inconsistencies between the collection and indexes.
  - If a bucket has a `value of 0`, then there are no inconsistencies for the keys that hashed
    there.
  - If a bucket has a `value greater than 0`, then we are missing index entries.
  - If a bucket has a `value less than 0`, then we have extra index entries.
- Upon detection of any index inconsistencies, the [second phase of validation](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/collection_validation.cpp#L186-L240)
  is executed. If no index inconsistencies were detected, we’re finished and we report back to the
  user.
  - The second phase of validation re-runs the first phase and expands its memory footprint by
    recording the detailed information of the keys that were inconsistent during the first phase
    of validation (keys that hashed to buckets where the value was not 0 in the end).
  - This is used to [pinpoint exactly where the index inconsistencies were detected](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/index_consistency.cpp#L109-L202)
    and to report them.

## Repair Mode

Validate accepts a RepairMode flag that instructs it to attempt to fix certain index
inconsistencies. Repair mode can fix inconsistencies by applying the following remediations:

- Missing index entries
  - Missing keys are inserted into the index
- Extra index entries
  - Extra keys are removed from the index
- Multikey documents are found for an index that is not marked multikey
  - The index is marked as multikey
- Multikey documents are found that are not covered by an index's multikey paths
  - The index's multikey paths are updated
- Corrupt documents
  - Documents with invalid BSON are removed

Repair mode is used by startup repair to avoid rebuilding indexes. Repair mode may also be used on
standalone nodes by passing `{ repair: true }` to the validate command.

See [RepairMode](https://github.com/mongodb/mongo/blob/4406491b2b137984c2583db98068b7d18ea32171/src/mongo/db/catalog/collection_validation.h#L71).

## Pre-fetching

Pre-fetching is an optimisation feature used in validate. When a page has been read from disk, prefetch workers pre-emptively
read neighbouring leaf pages into the cache in an attempt to avoid waiting on I/O.

It is currently enabled by default across all variants. See PM-3292 for more details.

## Cross-version Validation

Validation also works in a mode similar to repair, i.e. it can be run offline on a standalone
database with `mongod --validate`. In this mode it ignores version constraints, which allows
databases created with older mongods to be validated cross-version, against a more recent mongod.

A cross-version validation always considers the newer version of mongo to define what "correct"
means, specifically this implies that features which were deprecated may be ignored by the
validator.

## Errors and Warnings

The results of a validation pass is a BSON object detailing the sources of invalidity (if any) as
well as some metadata about the collection/indexes, and the operations undertaken by the validation
pass (if `repair:true`). Importantly, this object calls out validation findings either as errors or
warnings.

In a general sense, errors are real detected issues which impact the correctness of the database,
whereas warnings are usually not correctness-related or not guaranteed to be. When adding a new
error, consider whether the finding would previously have been allowed, we shouldn't retroactively
start flagging previously-allowed things as errors unless they directly impact the correctness of
the database. In a more specific sense, `|errors| > 0 <==> valid == false`, so errors are created
in order to draw attention to a specific finding, by declaring that the collection is invalid.

Some examples:

- Index inconsistencies are an _error_, they can directly cause incorrect query results.
  - To assist the user, a _warning_ is generated to count the number of such inconsistencies.
- Reports on repairs/changes made to the database during validation are _warnings_.
- Limitations that the validator encounters during its operation (e.g. due to memory pressure, or
  inability to gain exclusive access to tables) are a _warning_ since they don't prove that there
  is an issue, but they may hide issues.
- Invalid UTF-8 strings in BSON objects are a _warning_ since they were historically considered
  valid.
