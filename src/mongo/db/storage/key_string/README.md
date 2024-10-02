# KeyString

The `KeyString` format is an alternative serialization format for `BSON`. In the text below,
`KeyString` may refer to values in this format or the format itself, while `key_string` refers to the C++ namespace.
Indexes sort keys based on their BSON sorting order. In this order all numerical values compare
according to their mathematical value. Given a BSON document `{ x: 42.0, y : "hello"}`
and an index with the compound key `{ x : 1, y : 1}`, the document is sorted as the BSON document
`{"" : 42.0, "": "hello" }`, with the actual comparison as defined by [`BSONObj::woCompare`][] and
[`BSONElement::compareElements`][]. However, these comparison rules are complicated and can be
computationally expensive, especially for numeric types as the comparisons may require conversions
and there are lots of edge cases related to range and precision. Finding a key in a tree containing
thousands or millions of key-value pairs requires dozens of such comparisons.

To make these comparisons fast, there exists a 1:1 mapping between `BSONObj` and `KeyString`, where
`KeyString` is [binary comparable](#glossary). So, for a transformation function `t` converting
`BSONObj` to `KeyString` and two `BSONObj` values `x` and `y`, the following holds:

- `x < y` ⇔ `memcmp(t(x),t(y)) < 0`
- `x > y` ⇔ `memcmp(t(x),t(y)) > 0`
- `x = y` ⇔ `memcmp(t(x),t(y)) = 0`

## Ordering

Index keys with reverse sort order (like `{ x : -1}`) have all their `KeyString` bytes negated to
ensure correct `memcmp` comparison. As a compound index can have up to 64 keys, for decoding a
`KeyString` it is necessary to know which components need to have their bytes negated again to get
the original value. The [`Ordering`] class encodes the direction of each component in a 32-bit
unsigned integer.

## TypeBits

As the integer `42`, `NumberLong(42)`, double precision `42.0` and `NumberDecimal(42.00)` all
compare equal, for conversion back from `KeyString` to `BSONObj` additional information is necessary
in the form of `TypeBits`. When decoding a `KeyString`, typebits are consumed as values with
ambiguous types are encountered.

## Storage Format

The `KeyString` encoding for stored KeyStrings has the general format:

| CType (1st elem) | Data (1st elem) | CType (2nd elem) | Data (2nd elem) | ... | End Byte | RecordId (optional) |
| ---------------- | --------------- | ---------------- | --------------- | --- | -------- | ------------------- |
| 1 byte           | N bytes         | 1 byte           | M bytes         | ... | 0x4      | X bytes             |

Data is encoded from BSONObj element-by-element, excluding field names. The
[CType](https://github.com/mongodb/mongo/blob/513263f750668a80f294f1a8621e3cda81194a9f/src/mongo/db/storage/key_string.cpp#L83)
is derived from the BSON data. The contents of the data varies depending on the CType for each
element.

The [end](https://github.com/mongodb/mongo/blob/513263f750668a80f294f1a8621e3cda81194a9f/src/mongo/db/storage/key_string.cpp#L337)
byte is `0x4` and is designed to compare less than the [Greater](https://github.com/mongodb/mongo/blob/513263f750668a80f294f1a8621e3cda81194a9f/src/mongo/db/storage/key_string.cpp#L343)
discriminator, `0xFE` and greater than the [Less](https://github.com/mongodb/mongo/blob/513263f750668a80f294f1a8621e3cda81194a9f/src/mongo/db/storage/key_string.cpp#L342)
discriminator, `0x1`.

The `RecordId` is encoded in one of two formats:

- [Long](https://github.com/mongodb/mongo/blob/513263f750668a80f294f1a8621e3cda81194a9f/src/mongo/db/storage/key_string.cpp#L666)
  for RecordIds that are represented by a 64-bit integer.
- [Str](https://github.com/mongodb/mongo/blob/513263f750668a80f294f1a8621e3cda81194a9f/src/mongo/db/storage/key_string.cpp#L713)
  for RecordIds that are represented by a variable-length string of binary data.

Both RecordId encoding formats are decoded in reverse from [the end](https://github.com/mongodb/mongo/blob/513263f750668a80f294f1a8621e3cda81194a9f/src/mongo/db/storage/key_string.cpp#L2886-L2892)
of the `KeyString` so that the full key string does not need to parsed to obtain the RecordId.

## Query Format

`KeyString` has a query-only format, not designed for storage, that allows callers to find the
closest-matching `RecordId` given a `KeyString` without a `RecordId` using Discriminators.

In a sorted table, a caller can search for the string `[CType][Data][End]` and perform an exact
match on a stored string `[CType][Data][End][RecordId]` by determining that all common bytes
match.

Using Discriminators, a caller can perform an exclusive lower-bound search for the string
`[CType][Data][Greater]` to return all keys strictly greater than `[CType][Data][End][RecordId]`.
An inclusive lower-bound search for the string `[CType][Data][Less]` will return keys greater than
or equal to `[CType][Data][End][RecordId]`.

## Use in WiredTiger indexes

For indexes other than `_id` , the `RecordId` is appended to the end of the `KeyString` to ensure
uniqueness. In older versions of MongoDB we didn't do that, but that lead to problems during
secondary oplog application and [initial sync][] where the uniqueness constraint may be violated
temporarily. Indexes store key value pairs where the key is the `KeyString`. Current WiredTiger
secondary unique indexes may have a mix of the old and new representations described below.

| Index type                                                      | (Key, Value)                                                                                                                                          | Data Format Version            |
| --------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------ |
| `_id` index                                                     | (`KeyString` without `RecordId`, `RecordId` and optionally `TypeBits`)                                                                                | index V1: 6<br />index V2: 8   |
| non-unique index                                                | (`KeyString` with `RecordId`, optionally `TypeBits`)                                                                                                  | index V1: 6<br />index V2: 8   |
| unique secondary index created before 4.2                       | (`KeyString` without `RecordId`, `RecordId` and optionally `TypeBits`)                                                                                | index V1: 6<br />index V2: 8   |
| unique secondary index created in 4.2 OR after upgrading to 4.2 | New keys: (`KeyString` with `RecordId`, optionally `TypeBits`) <br /> Old keys:(`KeyString` without `RecordId`, `RecordId` and optionally `TypeBits`) | index V1: 11<br />index V2: 12 |
| unique secondary index created in 6.0 or later                  | (`KeyString` with `RecordId`, optionally `TypeBits`)                                                                                                  | index V1: 13<br />index V2: 14 |

The reason for the change in index format is that the secondary key uniqueness property can be
temporarily violated during oplog application (because operations may be applied out of order).
With prepared transactions, out-of-order commits would conflict with prepared transactions.
Instead of forcing users to rebuild secondary unique indexes, new keys are inserted in the new
format and old keys stay in the old format.

For `_id` indexes and non-unique indexes, the index data formats will be 6 and 8 for index version
V1 and V2, respectively. For unique secondary indexes, if they are of formats 13 or 14, it is
guaranteed that the indexes only store keys of `KeyString` with `RecordId`. If they are of formats
11 or 12, they may have a mix of the keys with and without `RecordId`. Users can run a `full`
validation to check if there are keys in the old format in unique secondary indexes.

## Building KeyString values and passing them around

There are three kinds of builders for constructing `KeyString` values:

- `key_string::Builder`: starts building using a small allocation on the stack, and
  dynamically switches to allocating memory from the heap. This is generally preferable if the value
  is only needed in the scope where it was created.
- `key_string::HeapBuilder`: always builds using dynamic memory allocation. This has advantage that
  calling the `release` method can transfer ownership of the memory without copying.
- `key_string::PooledBuilder`: This class allow building many `KeyString` values tightly packed into
  larger blocks. The advantage is fewer, larger memory allocations and no wasted space due to
  internal fragmentation. This is a good approach when a large number of values is needed, such as
  for index building. However, memory for a block is only released after _no_ references to that
  block remain.

The `key_string::Value` class holds a reference to a `SharedBufferFragment` with the `KeyString` and
its `TypeBits` if any and can be used for passing around values.

# Glossary

**binary comparable**: Two values are binary comparable if the lexicographical order over their byte
representation, from lower memory addresses to higher addresses, is the same as the defined ordering
for that type. For example, ASCII strings are binary comparable, but double precision floating point
numbers and little-endian integers are not.
