# SERVER-114615 — Validate must not false-positive on multikey equivalent-numeric values

## Symptom

`db.coll.validate({full: true})` and `dbCheck` report `extra index entries` (or
batch-level inconsistency rows in `local.system.healthlog`) on a multikey index
whose array data contains BSON-equivalent numeric values across distinct numeric
types — for example `{arr: [1, NumberLong(1), 1.0, NumberDecimal("1")]}`. The
collection is in fact internally consistent: the catalog generated one index
entry per array element, and a point read by either typed value returns the
matching `RecordId`. Only the cross-check inside `_validateIndex` flags it.

The bug surfaced even when validation did not exceed
`maxValidateMemoryUsageMB` (see SERVER-89845 for the memory-bucket-zeroing
sibling). Earlier reports were hard to reproduce because index-key type-bits
were not logged; SERVER-113567 and SERVER-113288 unblock diagnosis going
forward.

## Root cause

WiredTiger's `KeyString` collapses BSON-equivalent numeric values
(`NumberInt(n)`, `NumberLong(n)`, `NumberDouble(n.0)`, `NumberDecimal("n")`)
to one storage key plus a type-bits suffix that records the original BSON
type. The index iterator therefore yields **one storage key** for the three
array elements `[1, NumberLong(1), 1.0]`.

`_validateIndex` (in the `validate_adaptor` / `index_consistency` path) builds
two sets:

1. `indexKeys` — the de-duplicated storage-key set returned by the cursor.
2. `documentKeys` — the per-element multikey set derived by re-running
   `MultikeyPathTracker` / `getKeys` against the live document.

Set (1) collapses equivalent values to one entry; set (2) preserves one entry
per BSON-typed array element. The symmetric-difference comparison then
reports `documentKeys \ indexKeys` as **missing** entries and (under the
mirror sweep) `indexKeys \ documentKeys` as **extra** entries — both of which
are false positives. The index is correct; the comparator is asymmetric.

`BSONObjSet` ordering uses `BSONElement::woCompare` with `considerFieldName=false`,
which **does** treat numeric types as equal for ordering — but `std::set`
membership uses the same comparator and therefore deduplicates equal-but-
distinctly-typed array elements out of `documentKeys`, _sometimes_. Whether
the duplicate survives or gets collapsed depends on insertion order, the
catalog's emit sequence, and the storage engine's type-bit reconstruction.
That non-determinism is why the false positive appears intermittently across
otherwise-identical replays.

## Fix

Normalize numeric types **on both sides** of the comparison before
intersecting the sets, inside `_validateIndex` (and the `BSONObjSet` wrapper
used in `index_consistency.cpp`):

1. When building `documentKeys`, canonicalize each numeric `BSONElement` to a
   single representative type (Decimal128 if any decimal participates, else
   double) prior to inserting into the set.
2. When iterating index entries, reconstruct the typed value from the
   type-bits suffix, then apply the same canonicalization before set
   insertion.
3. The symmetric-difference step then operates on type-normalized sets and
   no longer reports phantom extras or missings for the equivalent-numeric
   case.

This is a comparator-locality fix, not a storage change: the on-disk format
is correct as-is; only the validate-side cross-check needs the
canonicalization step. The fix is independent of memory-bucket sizing and
does not require raising `maxValidateMemoryUsageMB`.

## Test surface

`jstests/noPassthrough/validate/validate_multikey_equivalent_values_no_false_positive.js`
inserts arrays mixing `NumberInt`, `NumberLong`, `NumberDouble`, and
`NumberDecimal` equivalents into a multikey index, runs
`validate({full: true})`, asserts `valid:true` with empty `errors` and no
`extraIndexEntries` / `missingIndexEntries`, and (when `dbCheck` is available)
asserts no inconsistency rows in `local.system.healthlog`.

## Diagnostic anchor for the attribution cheat-sheet

Top-level diagnostic uniquely identifying this bug post-fix: validate's
`errors` array containing the literal substring `"extra index entries"`
**combined with** every offending `indexKey` whose first element is numeric
**and** the index's `keysPerDoc` count for that document being a multiple of
the array length under cross-type duplication. The combination
(extra-entries warning + cross-type numeric array + multikey index) is
unique to this signature and is now safe to wire into the corruption
attributor.
