# BSONColumn compressor

The BSON Column compressor at a high level aims to realize compression benefits
for BSON data by serializing deltas or delta-of-deltas into Simple-8b format,
as well as capturing long runs of identical values or deltas. Deltas can be
taken from any sequence of same-typed values that can be interpreted as 16-byte
integral values (which includes strings of length 16 bytes or less). They can
also be taken from sequences of compatible objects or arrays by using
interleaved mode. We will give a short, higher-level description of the
format, more focused on implementation and intended toward pointing to key
areas of code to understand the implementation. For a more detailed breakdown
of the format itself, refer to
[BSON Column Compression](https://docs.google.com/document/d/1abJkDgR-RDbf65mg0y1Sfy_UiE2BYtBHhesVrdF-31k/edit)

Objects or arrays in interleaved mode are encoded with a sequence that begins
with a reference object, which contains a superset of all nested fields
in order in the series (order is important as this allows us to specify
delta streams across all of the elemtns). This is followed by a series of
Simple8b blocks that contains diffs in interleaved-order across all of the
fields. In order to find suitable objects for such a sequence, the
`BSONColumnBuilder` will build the reference object speculatively while
receiving objects, using merges as necessary to maintain a superset of fields.
Only once a sequence is terminated will the reference object be finalized.

Large runs of identical values (or identical delta or delta-of-delta values)
are encoded by keeping a distinct Simple8b selector value, which reserves 4
bits for specifying a number of 120-value repeating blocks. These sequences are
exact repeats of the last seen object, regardless of what type or value it had.

In order to specify what is being encoded, the encoder outputs control bytes at
the start of each sequence, specifying whether it is a literal value, a
sequence of delta blocks, or an interleaved sequence.

Decoding can be done in one of two ways: using the `BSONColumn` iterator, or
the `BSONColumnBlockBased` decoder. The block based decoder will be more
efficient for decoding large sequences of elements, and also allows for pulling
out specific path values, but requires defining a `Materializer` to receive
values.

## General Usage

```
// Using the bson column builder to encode values
BSONColumnBuilder cb;
cb.append(elem1);
cb.append(elem2);
BSONBinData binData = cb.finalize();

// Using the BSONColumn iterator to decode values
BSONColumn col(binData.data, binData.length);
ASSERT_EQ(col.size(), 2);
auto it = col.begin();
ASSERT_EQ(*it, elem1);
++it;
ASSERT_EQ(*it, elem2);
++it;
ASSERT_FALSE(it.more());

// The block decoder requires defining an Appendable or Materializer which receives all decoded values
// from BSONColumn, see bsoncolumn_helpers.h for definitions of these concepts
BSONColumnBlockBased col2(binData.data, binData.length);
boost::intrusive_ptr allocator{new BSONElementStorage()};
std::vector<BSONElement> collection;
col2.decompress<BSONElementMaterializer, std::vector<BSONElement>>(collection, allocator);
ASSERT_EQ(collection.size(), 2);
ASSERT_EQ(collection[0], elem1);
ASSERT_EQ(collection[1], elem2);
```

## Delta and numeric encodings

All types of `BSONElement` that can be represented in numeric form will be
transformed and represented as a series in either delta or delta-of-delta
encoding. The first of a run is encoded as a literal (i.e. written in its raw
BSONElement form), and the rest as numeric deltas by the Simple8b encoder. See
`usesDeltaOfDelta()` in bsoncolumn_util.h for what types of elements use
delta-of-delta encoding. Elements which cannot be represented in numeric form
will allow a delta sequence that contains only 0 deltas (meaning they are
repeats of the first literal). See `onlyZeroDelta()`. Some types are
encoded as 64-bit values, and some as 128-bit values, see `uses128bit()`.

The `BSONColumn::Iterator` (implementation in bsoncolumn.cpp) tracks previous
values as needed to decode delta or delta-of-delta encodings. Decoding of
numerical values is done by `Decoder64::materialize()` and
`Decoder128::materialize()`. The block decoder is implemented in
bsoncolumn.inl and unwinds both delta encodings and numeric conversions in
various `decompresAll*()` templates defined in bsoncolumn_helpers.h

The `BSONColumnBuilder` is defined in bsoncolumnbuilder.h and .cpp and
maintains distinct internal states when currently processing interleaved mode
or non-interleaved mode. In regular mode, an `Encoder64` or `Encoder128` is
maintained as appropriate based on the BSONElement types last seen and will
handle delta and numeric encoding before passing results on to the Simple8b
encoder.

## Simple8b

Simple8b encodes a series of numeric values compactly into a sequence of 64-bit
blocks. In each block, the lower 4 bits are used as a selector, which
indicates how to interpret the other 60 bits. For example, if the numbers are
sufficiently small, it may indicate that they should be interpreted as 15
distinct values of 4 bits each, or if they are much larger, 3 values of 20 bits
each. Additionally, some of the BSONElement types we wish to encode are
128-bit values (e.g. strings), which often have deltas which fit within the 60
meaningful bits allowed for by Simple8b. In these cases, we can pass these
deltas to Simple8b as well, and furthermore in some cases the deltas may fit
within 60 meaningful bits when we restrict the deltas to the higher order bits
of the string. To handle this, we use extended selectors: in two of the
Simple8b selectors there are 4 additional "leftover" bits we can use (the 7
value selector and the 8 value selector). We then use these extra bits to
define where the delta is occuring in the 128-bit value.

Finally, one last selector is reserved for run-length encoding. This value
indicates we should use the 4 extended bits to define how many blocks of 120
repeats occur in sequence.

The logic for Simple8b encoding and decoding lives in simple_builder.h and inl
and simple8b.h and inl. Basic encoding and decoding is done with a builder and
iterator interface:

```
BufBuilder buffer;
auto writeFn = [&buffer](uint64_t simple8bBlock) {
    buffer.appendNum(simple8bBlock);
    return true;
};
Simple8bBuilder<uint64_t> builder;
builder.append(elem1, writeFn);
builder.append(elem2, writeFn);
builder.flush(writeFn);

Simple8b<uint64_t> decoder(buffer.release(), buffer.len());
auto it = decoder.begin();
auto end = decoder.end();
ASSERT_EQ(*it, elem1);
++it;
ASSERT_EQ(*it, elem2);
++it;
ASSERT_FALSE(it.more());

```

Simple8b.h and inl also contains functions for performing aggregate
calculations or visiting decoded values in bulk by leveraging partially
pre-computed lookup tables. These can be used to compute a sum or prefixSum (i
e. a running sum across delta'ed values) across a block, to return the last
value in a block, or to apply a user-provided function on all values in the
block. Implementations for these are found in simple8b.inl across the
`TableDecoder`, `ParallelTableDecoder`, `OneDecoder`, and `SimpleDecoder`.
These various implementations are optimized for different size tables, we then
use a set of static definitions to set which implementations are used for
various block sizes, which are used in the implementations at the bottom of
simple8b.inl.

## Interleaved mode

A sequence of objects or arrays encoded in interleaved mode begins with a
reference object encoded as a raw literal. This reference object contains a
superset of all paths in the sequence, and it itself is not decoded as an
element to be recovered. The remainder of objects are encoded as a set of
delta streams for each element in the reference object, which may also contain
"missing" values for fields not present in objects encoded in the sequence.
These delta streams are encoded in Simple8b and are "interleaved" in the sense
that simple8b blocks are written to the same buffer in whatever order they
complete blocks in (this ordering is predictable based on element counts in the
blocks and thus does not need to be written during encoding).

In order for the reference object to be a superset, the `BSONColumnBuilder` (in
bsoncolumnbuilder.cpp) builds it up in tandem with visiting additional objects
to add to the sequence. `BSONColumnBuilder::mergeObj()` is used to continually
merge the reference object with new objects to cover newly discovered fields.
The majority of the logic for this process is in
`BSONColumnBuilder::_appendObj()`, which manages a distinct state while
building up the reference object. Once the reference object is finalized it
will be written, along with the interleaved content built up so far. New
objects will continue to be delta-encoded against it until an incompatible one
is seen, at which point the interleaved mode sequence will be finalized.

The `BSONColumn::Iterator` (in bsoncolumn.cpp) maintains two distinct modes
depending on whether it is currently visiting interleaved data. Depending on
this state, it calls either `_incrementRegular()` or `_incrementInterleaved()`
when incremented. The first time an interleaved mode control tag is seen,
`_initializeInterleaving()` is called, which traverses the reference object to
find all of the subfields and initializes a set of interleaved states which
includes Simple8b decoders for each.

## Block decoding

The `BSONColumnBlockBased` decoder is more efficient when decoding a full
series of values in bulk. It provides implementations of `decompress()` that
take callbacks that either satisfy the `Appendable` concept or containers and
an implementation of the `Materializer` concept. An `Appendable` is intended
to represent a recipient that accepts all of the types of `BSONElement` that
can come out from column decoding, while a `Materializer` represents just the
portion of the logic that defines and allocates a specific representation for
storing these values. Both of these concepts are defined in
bsoncolumn_helpers.h. A default `BSONElementMaterializer` is supplied that
represents these as `BSONElement`. A default templated `Collector` class is
also provided which satisfies the `Appendable` concept when given a
`Materializer` and STL-style container class to be filled with decompressed
values.

The iterative block decoder implementation lives in bsoncolumn.inl and
bsoncolumn_helpers.h. The block decoder largely leverages templated
`decompressAll*()` functions that iteratively decode entire blocks of values of
matching type. There is also a path-decoder implementatio of
`BSONColumnBlockBased::decompress()` that accepts a set of `Path` and
`Container`. This takes a list of fieldname paths from a root object, and
decodes values from the specified paths separately into their corresponding
containers.

## Aggregate queries

bsoncolumn_expressions.h provides several functions for efficiently computing
aggregate functions (first, last, min, max, minmax). These are implemented in
bsoncolumn_expressions_internal.h. Many of these make use of custom collectors
passed to the block decoder (and through this take advantage of table decoders
to get aggregate results from the Simple8b decoders). These also serve as a
good example of how to implement custom collectors to get decoded results in
alternate formats or with additional computation.
