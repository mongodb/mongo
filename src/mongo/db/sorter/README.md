# The External Sorter

The [external sorter](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter.h#L653) is a MongoDB component that sorts large volumes of data while keeping its memory usage bounded by spilling in-memory state to external storage as needed. It exposes a generic “document sorter” abstraction that callers use to push `(Key, Value)` pairs and later pull them back in sorted order.

The working set of `(Key, Value)` pairs can easily exceed available memory. To handle this, the sorter iteratively consumes keys from its caller, sorts them in memory, and, once a configurable memory limit is reached, spills sorted runs via its configured spiller (an abstraction over the chosen external storage, such as files or a container). Those spilled runs are later merged — using a bounded fan‑in k‑way merge — by streaming the spilled data back through the spiller’s API and emitting the final sorted sequence to downstream consumers.

### Container-based vs. file-based spillers

The external sorter has two different underlying storages to handle spilling:

1. A file-based spiller, which:

- [Writes](https://github.com/mongodb/mongo/blob/c344aead7fcae2aff3cd6d35419a0f9a3746ff8d/src/mongo/db/sorter/file_based_spiller.h#L273) sorted runs to private temporary files on the local filesystem.
- Tracks [file-specific statistics](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_stats.h#L76).
- Writes spill files in fixed-size blocks, which can be [compressed](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/file_based_spiller.h#L336) (using Snappy) at the sorter layer.
- When the server’s encryption at rest is enabled, the fixed size blocks are [encrypted](https://github.com/mongodb/mongo/blob/c344aead7fcae2aff3cd6d35419a0f9a3746ff8d/src/mongo/db/sorter/file_based_spiller.h#L350-L359) using the configured persistent key rather than being left in plaintext on disk.

2. A container-based spiller, which:

- [Writes](https://github.com/mongodb/mongo/blob/c344aead7fcae2aff3cd6d35419a0f9a3746ff8d/src/mongo/db/sorter/container_based_spiller.h#L192) the same logical spill runs into a storage engine-backed record-oriented container (for example, a `RecordStore`).
- Tracks [container-specific statistics](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_stats.h#L56).
- Each key in a container-based sorter is a [monotonically increasing integer](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/container_based_spiller.h#L233) and stores an opaque blob for each `(Key, Value)` pair.
- Compression and encryption are provided by the storage engine: the spill data is subject to whatever storage engine compression and encryption settings are configured for that container’s ident.
- Because the container-based sorter is implemented on top of the storage engine, its spills participate in replication and recovery and respect storage configuration such as `directoryPerDb`/`directoryForIndexes`.

Both implementations expose the same API and merge behavior, so they produce an identical sorted stream of entries. The two implementations differ only in how and where spilled data is stored and monitored.

The caller on creation of the sorter can construct and pass in the appropriate spiller (file-based or container-based) if they want to enable spilling.

### Sorter memory accounting and spilling

The sorter enforces a memory budget passed in by its caller using the caller's configured [`maxMemoryUsageBytes`](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter.h#L97) when they construct their Sorter instances.

#### Splitting memory between data and iterators

The sorter divides this budget into:

- **Data budget**: used for the in-memory run of `(Key, Value)` entries currently being built.
- **Iterator budget**: reserved for the set of iterators that will be used to read back spilled runs (file-based and container-based).

Both backends share the same _iterator_ reservation; the storage-specific code only affects the _per-iterator_ size.

Let:

- `M` be the initial maximum number of bytes allocated for the sorter to use.
- `P` be fraction of the total memory budget reserved for iterators (default [`0.1`](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter.idl#L38) = 10%).
- `I_cap` be the hard cap on iterator memory (currently [1 MB](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L537)).

The iterator reservation is:

- `iteratorReservationBytes = min(I_cap, P * M)`

The remaining memory becomes the data budget:

- `dataMaxMemoryUsageBytes = M - iteratorReservationBytes`

In the implementation, the adjusted `maxMemoryUsageBytes` on the sorter corresponds to this data budget; the iterator reservation is tracked separately via an [internal iterator memory cap](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L560). Throughout this documentation, we will refer to the adjusted `maxMemoryUsageBytes` as `dataMaxMemoryUsageBytes`.

#### How many iterators can exist?

Iterator count is bounded by the reserved iterator budget and the size of a single iterator object:

The file-based iterator buffer is configured to `64 KB` per active iterator. The container-based iterator buffer size is computed dynamically as the average serialized entry size across all spilled data, since container iterators read one entry at a time from a cursor rather than loading a fixed-size block.

The per‑iterator memory footprint is obtained from the underlying spiller implementation. ([container-based spiller](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/container_based_spiller.h#L300), [file-based spiller](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/file_based_spiller.h#L454))

The max number of iterators is calculated as:

- `maxIterators = floor(iteratorReservationBytes / iteratorSize)`

#### Merge fan-in limit

When merging many spilled runs, the sorter limits the number of iterators it drives concurrently. It derives a [**fan-in limit**](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L568) (how many sorted inputs you merge at once) from the data budget and the per-iterator buffer size:

- `fanInLimit = max(dataMaxMemoryUsageBytes/ bufferSize, 2)`

For file-based spillers, `bufferSize` is a fixed `64 KB`. For container-based spillers, `bufferSize` is the average serialized entry size (recomputed after each spill), so the fan-in limit adapts to the actual data being sorted.

`fanInLimit` is used as:

- The maximum number of child iterators a merge can drive at once, and
- The target spill count for the final merge when the caller indicates we are finished sorting.

The fan-in limit bounds how many iterator buffers can be live at once during merges (`64 KB` each for file-based, average entry size for container-based).

#### Rough upper-bound data volumes

The sorter works in terms of _spills_ (sorted runs):

- One spill → one sorted range → one iterator.
- Each spill is created when the in-memory staging vector grows past `dataMaxMemoryUsageBytes`, so its size is at most the `dataMaxMemoryUsageBytes`.

- The final merge is a no-op if the size of the vector of iterators is less than or equal to `fanInLimit`.

#### Add path and spilling

The sorter’s basic workflow is:

1. **Add**

   - New entries are first accumulated in an in‑memory staging buffer, and the sorter tracks their total memory footprint.
   - When the tracked memory usage exceeds the configured limit, the sorter spills if spilling is configured. Otherwise, the sorter aborts the operation with a “sort exceeded memory limit but did not opt in to external sorting” error.

2. **Spill**

   When spilling, the sorter:

   - Sorts the staging vector in memory.
   - Writes the sorted run out as a new _spilled range_:

     - **File-based**:

       - Uses file-specific classes for spilling and writing to files.
       - Data is written in [`64 KB` blocks](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/file_based_spiller.h#L52), optionally compressed and/or encrypted.

     - **Container-based**:
       - Uses container-specific classes for spilling and writing to containers.
       - Data is written entry-by-entry into an internal container (for example, a temporary `RecordStore` keyed by `KeyFormat::Long`).

   - Creates a new iterator for the spilled range and pushes it into a vector of iterators:
     - Each iterator represents one logical sorted range, even though the data may occupy multiple blocks on disk or entries in the container.
     - Idle iterators hold mostly metadata; file-based `64 KB` buffers are only allocated while an iterator is actively being driven, and container-based iterators read one entry at a time from a cursor.

   After adding the new iterator:

   - If the size of the vector of iterators is greater than the maximum allowed number of spill iterators, then the sorter merges spills.

3. **Merging spill**

   [Merging spills](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L486-L506) reduces the iterator count while staying within memory bounds:

   - Selects up to `fanInLimit` child iterators to merge at once.
   - Performs a k-way merge over those iterators.
   - Writes the merged output as a new spilled range:

     - File-based: to a new spill file.
     - Container-based: to a new container range, reusing the same physical container.

   - Replaces the old iterators with a single iterator over the merged range, shrinking the vector of iterators until it falls below roughly half of the original size.

Multiple logical ranges may share a physical file or container; merges create new ranges and retire the old ones.

#### Finishing

The caller [signals](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter.h#L699) when no more input will arrive. Once input is complete:

- If there have been **no spills** (our vector of iterators is empty):

  - The sorter [sorts](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L654) the remaining in-memory vector.
  - It [returns an iterator](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L659) over this single in-memory run.

- If there **have been spills**:

  - The sorter first [spills](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L662) any remaining in-memory data (creating one more range and iterator if needed).
  - It then performs a final merge:

    - If the size of the vector of iterators is greater than the fan-in limit, it [merges spills](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L663) down toward the fan-in limit, using the same k-way merge logic when we perform spilling.
    - The result is one or more merged ranges, each represented by an iterator.

  - Finally, the sorter [returns a merge iterator](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L665) that yields all remaining ranges in sorted order. The merge fan-in is again capped by the fan-in limit, so the number of active iterator buffers remains bounded for both file- and container-based sorters.

### Sorter variants and limits

Callers interact with the sorter through a single logical API, but internally we use different implementations depending on the configured result limit. Conceptually, there are four variants.

- [**No‑limit sorter**](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L574) (unbounded)
- [**Limit‑one sorter**](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L778)
- [**Top‑K sorter**](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L854)
- [**Bounded sorter**](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L1345)

The first three `Sorter` variants are chosen by the [sorter factory](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L1545-L1561) based on the caller’s logical limit: when there is no limit, it uses an unbounded no‑limit sorter; when the limit is exactly 1, it uses a specialized limit‑one sorter; and when the limit is a finite `K > 1`, it uses a top‑K sorter that keeps only the best `K` entries while still sharing the same spilling and merging machinery.

These first three `Sorter` variants share the same spill and merge machinery described above; they differ mainly in how aggressively they prune data and when they can produce results. The `BoundedSorter` uses the same underlying spiller and merge iterator primitives, but applies them through a separate streaming interface and spilling strategy.

#### No‑limit sorter

The **no‑limit sorter** is used when there is **no limit** on the number of results (the logical `K = ∞` case).

- Maintains all `(Key, Value)` pairs, spilling and merging as needed to respect the memory budget.
- Only starts producing output once the caller has finished adding input and the sorter has completed any required merges.

##### Resuming from existing spilled ranges

The external sorter also supports resuming a no-limit sorter from previously spilled data. When a no‑limit sorter needs to survive shutdown (for example, during a controlled restart), the caller can first ask it to [persist its state](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L681-L693). Internally, the sorter ensures that any remaining in‑memory data is spilled and returns an opaque description of its storage: a storage identifier (such as a spill file name or container ident) plus a list of logical sorted ranges that cover all spilled data.

After restart, the caller can use [a dedicated factory entry point](https://github.com/mongodb/mongo/blob/65e3cec07260d56eea6f96c9edf6ab780b11b751/src/mongo/db/sorter/sorter_template_defs.h#L1564-L1580) that constructs a new sorter from existing ranges. This factory takes the storage identifier, the list of ranges, and the same sort options and comparator, and it reconstructs a no‑limit sorter that reads from the already‑sorted spilled data instead of repeating the sort.

#### Limit‑one sorter

The **limit‑one sorter** is a specialized variant for the case where only the **single best element** is needed.

- Keeps track of the current best `(Key, Value)` pair and discards all worse candidates.
- Omits external spill logic entirely and only tracks memory usage when explicitly requested.
- Is used when the caller requests a limit of exactly 1; in the common case, this sorter minimizes both CPU and memory overhead.

#### Top‑K sorter

The **top‑K sorter** handles the general case where the caller requests a **finite limit `K > 1`**.

- Maintains up to `K` best elements in memory using a heap, discarding worse elements early.
- Can spill and merge when in‑memory state exceeds the configured memory budget, but does so over a much smaller working set than the no‑limit sorter.
- Uses the same k‑way merge, iterator, and spill machinery as the no‑limit sorter; the main difference is that many candidates are pruned before ever reaching disk.

#### Bounded sorter

The **bounded sorter** is a separate abstraction for inputs that are **“almost sorted”** according to the sort key: we can bound how far out of order any two input elements are (for example, time‑series data where no document is more than an hour out of order). It is implemented by `BoundedSorter` on top of `BoundedSorterInterface`.

- Exposes a **streaming interface** that can interleave input and output, rather than the strict “load then iterate” pattern of the other three `Sorter` variants.
- Uses a **bound function** to maintain a moving lower bound on future keys; once a key is known to be strictly below this bound, it is safe to return it.
- Maintains a heap of candidate `(Key, Value)` pairs and, when its internal memory usage exceeds the configured budget, spills to external storage.
- Can **incrementally produce results while input is still arriving**: as the bound advances, earlier elements become safe to emit even before the final input has been seen.
- Can optionally enforce that input is not “too far” out of order: when input checking is enabled, the bounded sorter will raise an error if an incoming key violates the current bound, protecting callers that rely on bounded out‑of‑order assumptions for efficiency.
