# FTDC

## Table of Contents

- [FTDC](#ftdc)
  - [Table of Contents](#table-of-contents)
  - [High Level Overview](#high-level-overview)
  - [Files](#files)
  - [Archive File Format](#archive-file-format)
    - [Compressed Chunk Format](#compressed-chunk-format)
      - [Extraction](#extraction)
      - [Delta encode](#delta-encode)
      - [Run length encoding of zeros](#run-length-encoding-of-zeros)
      - [Varint compression](#varint-compression)
      - [ZLIB Compression](#zlib-compression)
      - [Walkthrough](#walkthrough)
    - [Metadata Delta encoding](#metadata-delta-encoding)
      - [Delta Algorithm](#delta-algorithm)
      - [Reconstruction algorithm](#reconstruction-algorithm)

## High Level Overview

FTDC stands for Full-Time Diagnostic Data Capture. FTDC is used to capture data about the mongod and
mongos processes and the system that a mongo process is running on.

From a top down view, An
[`FTDCController`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/controller.h)
object lives as a decoration on the service context. It is registered
[`here`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/ftdc_server.cpp#L55). The
`FTDCController` is initialized from the mongod and mongos main functions, which call
[`startMongoDFTDC`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/ftdc_mongod.h)
and
[`startMongoSFTDC`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/ftdc_mongos.h)
respectively. The FTDC controller owns a collection of collector objects that gather system and
process information for the controller. These sets of collector objects are stored in a
[`FTDCCollectorCollection`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/collector.h#L77-L116)
object, allowing all the data to be collected through one call to collect on the
`FTDCCollectorCollection`. There are two sets of `FTDCCollectorCollection` objects on the
controller:
[\_periodicCollectors](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/controller.h#L200-L201)
that collects data at a specified time interval, and
[\_rotateCollectors](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/controller.h#L207-L208)
that collects one set of data every time a file is created.

At specified time intervals, the FTDC Controller calls collect on the `_periodicCollectors`
collection. To collect the data, most collectors run existing commands via
[`CommandHelpers::runCommandDirectly`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands.h#L219-L224)
which is abstracted into `FTDCSimpleInternalCommandCollector`. Some process data is gathered via
custom implementations of `FTDCCollectorInterface`. After gathering the data, the FTDC Controller
[writes](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/controller.cpp#L251-L252)
the data out as described below using an
[`FTDCFileManager`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/file_manager.h).

Most collectors are of the class
[`FTDCSimpleInternalCommandCollector`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/ftdc_server.cpp#L173).
The
[`FTDCServerStatusCommandCollector`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/ftdc_server.cpp#L195).
has a very specific query that it needs to run to get the server information. For example, it does
not collect sharding information from `serverStatus` because sharding could trigger issues with
compression efficiency of FTDC. The system stats are collected by `LinuxSystemMetricsCollector` via
`/proc` on Linux and `WindowsSystemMetricsCollector` via Windows perf counters on Windows.

The
[`FTDCFileManager`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/file_manager.h)
is responsible for rotating files, managing disk space, and recovering files after a crash. When it
gets a hold of the data to be written to disk, it initially writes the data to an
[`FTDCCompressor`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/compressor.h)
object. The
[`FTDCCompressor`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/compressor.h)
compresses the data object and appends it to a chunk to be written out. If the chunk is full or the
BSON Schema has changed, the compressor indicates that the chunk needs to be written out to disk
immediately. Otherwise, the compressor indicates that it can store more data.

If the compressor can store more data there are two possibilities. If the data in the compressor has
reached a certain threshold, the
[`FTDCFileWriter`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/file_writer.h)
will decide to write out the unwritten section of data to an interim file. The goal of the interim
file is to flush data out in smaller intervals between archives to ensure the most up-to-date data
is recorded before a crash. The interim file also provides a chance for the regular files to have
maximum sized and compressed chunks. If not, nothing is written to outside of the compressor. If the
compressor indicates that the chunk needs to be written out immediately, data is flushed out of the
compressor into the archive file. The interim file is erased and written over, and the compressor
resets to take in new values.

The
[`FTDCFileManager`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/file_manager.h)
also decides when to rotate the archive files. When the file gets too large, the manager deletes the
reference to the old file and starts writing immediately to the new file by calling
[rotate](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/file_manager.cpp#L304-L324).

## Files

FTDC writes two types of files in `diagnostic.data`.

**Archive files**: `metrics.%Y-%m-%dT%H-%M-%SZ-CCCCC`
where

- `%Y-%m-%dT%H-%M-%SZ` - `strftime` format string to format a UTC date time string. Ex:
  `2024-03-08T22-58-41Z`
- `CCCCC` - is a five digit uniquifier in case multiple files are opened in one second. It is
  always `00000` except in unit tests.

It is an append only file which can be read with `bsondump`. It is composed of several types of bson
documents. See [`Archive Format`](#archive-file-format). FTDC creates new archive files on server
restart and when the file grows larger then the size cap.

**Interm file**: `metrics.interim`

There is always one file and it always has just one document of the metric type. It represents the
most recent uncompressed metrics sample. The file is constantly overwritten in contrast to the
archive file.

## Archive File Format

Assumptions:

- All numbers are encoded little endian
- Everything is a BSON document or BSON field unless noted

Using a pseudo EBNF format, the FTDC archive format is as follows:

```cpp
FTDC = ftdc_doc*

ftdc_doc = metadata
    | metric
    | metadata_delta

metadata =
    _id : DateTime
    type: 0
    doc : role_based_collectors_doc | collectors_doc

// sharded cluster 8.0+ - metrics reference doc
role_based_collectors_doc =
    start: DateTime
    shard: collectors_doc
    router: collectors_doc
    common: collectors_doc
    end: DateTime

// 8.0 non-sharded cluster or pre-8.0 sharded cluster - metrics reference doc
collectors_doc = // pre-8.0
    start: DateTime
    collect_doc+
    end: DateTime

collect_doc =
    string : {  // string is an arbitrary field name here
        start: DateTime
        bson_elements+
        end: DateTime
    }

metric =
    _id : DateTime
    type: 1
    doc : BinData(0) // see metrics_chunk

metrics_chunk = // Not a BSON document, raw bytes
    uncompressed_size : uint32_t
    compressed_chunk: uint8_t[] // zlib compressed

compressed_chunk = // Not a BSON document, raw bytes
    reference_document uint8_t[] // a BSON Document -see role_based_collectors_doc
    sample_count uint32_t
    metric_count uint32_t
    compressed_metrics_array uint8_t[] // See compressed chunk format below

metadata_delta =
    _id : DateTime
    type: 2
    index: uint32_t
    doc : collectors_doc | collectors_delta_doc

collectors_delta_doc = // collect_doc exclude unchanged fields from previous metadata_delta
    start: DateTime
    collect_doc+
    end: DateTime
```

### Compressed Chunk Format

Compressed Chunk is a delta, run length encoded, and varint compressed array of numbers.

The FTDC compressors extracts a series of uint64_t numbers from a subset of BSON fields from a
series of BSON documents. All documents in a compressed chunk have the same number of numbers. If
the count changes, the current chunk is closed and a new one is open. As a result of this behavior,
FTDC compression is poor if the number of numerical fields changes. FTDC compression only extracts
numeric fields from certain BSON types. These types are `double`, `int64`, `int32`, `boolean`,
`datetime`, `decimal128`, and `timestamp`. It is a lossy compression for some types as numbers are
cast to `uint64_t`.

FTDC extracts these metrics and stores them in a column oriented format. These numbers are then
processed in three phases

1. Extraction
2. Delta encode
3. Run length encoding of zeros
4. Varint compression
5. ZLIB Compression

#### Extraction

For a given BSON document, all numeric fields are extracted and converted to `uint64_t`. Timestamps
are extracted as two `uint64_t`, seconds followed by increment. For doubles, `NaN` is encoded as
zero, doubles less than `MIN_INT64` are stored as `MIN_INT64`, and doubles greater than `MAX_INT64`
are stored as `MAX_INT64`.

#### Delta encode

Often, values do not change between samples (for instance the process id of a process never
changes). To reduce the space these values take, we subtract the value of previous sample from the
current sample. The first sample after the reference document uses the values from the reference
document as its baseline.

#### Run length encoding of zeros

A sequence of zeros is compressed to a pair of numbers `[0, x]` where `x` is non-zero positive
integer that indicates the number of zeros in a sequence. For instance, an array of zeros `[0, 0, 0,
0]` is transformed to `[0, 4]`.

#### Varint compression

Varint encoding is a way to reduce the number of bytes needed to represent an integer. For more
information, see [VarInt reference](https://en.wikipedia.org/wiki/Variable-length_quantity). The
FTDC reference implementation uses the varint implementation from S2.

#### ZLIB Compression

The entire block is compressed with the `zlib` compression algorithm.

#### Walkthrough

For instance, for the following sequence of documents:

**Example**:

```js
{"a": 1, "x" : 2, "s" : "t"}
{"a": 2, "x" : 2, "s" : "t"}
{"a": 3, "x" : 2, "s" : "t"}
{"a": 4, "x" : 2, "s" : "t"}
```

The first `a : 1` is stored as the reference document. FTDC then builds an array of `[2, 3, 4, 2, 2,
2]` to represent the `a` field followed by the `x` field.

Next, FTDC computes the delta for each sample in the chunk from the previous chunk. Nothing changes
in the reference document but array is transformed to `[1, 1, 1, 0, 0, 0]`.

Next the array is encoded with run length encoding for zeros `[1, 1, 1, 0, 3]`. Notice the length of
the array is shorter.

Next, these numbers are written to a block of memory with VarInt encoding. If these numbers are
written as is, it would be 8 bytes \* 5 numbers for a total of 40 bytes. But by using varint, a
smaller number of bytes can be used (5 in this example).

Finally, the chunk is compressed with zlib compression.

### Metadata Delta encoding

The metadata delta files are plain bson but use a simple diff algorithm to de-duplicate data. This
format was chosen because it tracks things like server parameters which change infrequently and to
preserve string values.

#### Delta Algorithm

The deltas are computed by comparing the previous document and the current document for a periodic
metadata sample together. This algorithm is not recursive as it only compares one level of fields.

Python like pseudo code:

```python
previous_doc = None
counter = 0

def delta_doc(doc):
   # reset delta tracking
   if previous_doc is None or doc.field_count != previous_doc.field_count:
       previous_doc = doc
       counter = 0
       return (counter, doc)

   out_doc = {}
   has_changes = False
   for i in range(0, previous_doc.field_count):
       if doc[i].name != previous_doc[i]:
            # reset delta tracking, omitted for brevity
           return (counter, doc)

       if doc[i].name == "start" or doc[i].name == "end":
           out_doc.append(doc[i])
       else doc[i] != previous_doc[i]:
           out_doc.append(doc[i])
           has_changes = True

   if not has_changes:
       return None

   counter += 1
   return (counter, out_doc)
```

#### Reconstruction algorithm

To reconstruct a periodic metadata sample at a point in time, you can replay all the previous
deltas. If you treat the document as dictionary/map, simply update a field with the most recent
value to get a complete snapshot. Note that FTDC does not currently implement this algorithm since
it does not need to reconstruct point-in-time snapshots

Python like pseudo code:

```python
def reconstruct(docs[]):
   out_doc = {}
   for doc in docs:
       for i in range(0, doc.field_count):
           out_doc[doc[i].name] = doc[i].value


   return out_doc
```
