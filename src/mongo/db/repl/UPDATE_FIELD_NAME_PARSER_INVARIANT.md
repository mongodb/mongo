# Update Oplog Entry — Field-Name Parser Invariant

> Companion spec for SERVER-124369. Pins the contract that the
> replication oplog-update classifier MUST honour. Replication
> correctness for replacement-style updates depends on it.

## Statement of the invariant

A replication oplog entry with `op: "u"` carries an update description
in its `o` field. Two shapes are valid:

| Shape | `o` form | Routed through |
|-------|---------|----------------|
| v2 delta | `{$v: 2, diff: {...}}` | `UpdateModification::parseFromOplogEntry` → diff applier |
| replacement | full new document (no `$v: 2` wrapper) | replacement applier |

**Invariant.** The classifier that picks between the two routes MUST
discriminate by the *presence and value of the top-level `$v` sentinel*,
NOT by the presence of any reserved-looking key (`$diff`, `$set`,
`$unset`, `$rename`, `$inc`, etc.) inside `o`.

Equivalently: when `o` is a replacement document, every top-level field
of `o` — including any whose name happens to start with `$` — MUST be
preserved literally as a data field on the secondary. The replacement
applier MUST NOT inspect reserved names for modifier semantics.

## Why it matters

User documents may legally contain fields named `$diff`, `$set`, etc.
(BSON allows `$`-prefixed names at non-top-level positions, and a number
of drivers / pipeline stages produce them at the top level too). A
classifier that latches onto a reserved name without first checking `$v`
will:

1. Treat the user's literal data as a delta operator.
2. Apply that "delta" to whatever happens to live on the secondary.
3. Silently corrupt the document, or crash the secondary on a malformed
   delta payload — a hard data-divergence event.

This bug class also recurs whenever the diff format adds new reserved
names: any schema change that introduces a sentinel must update the
classifier and add a regression row to the jstest matrix.

## Parser site

`src/mongo/db/exec/agg/internal_apply_oplog_update_stage.cpp` —
`InternalApplyOplogUpdateStage::InternalApplyOplogUpdateStage`
(constructor, ~line 64). It invokes
`write_ops::UpdateModification::parseFromOplogEntry(oplogUpdate, ...)`,
which is the single chokepoint that decides delta-vs-replacement. The
classifier inside `parseFromOplogEntry` is the load-bearing function;
it must read `$v` FIRST and refuse to interpret any other key on the
replacement path.

A secondary site is `src/mongo/db/repl/oplog.cpp` near the update
applier dispatch — both sites must agree on the discriminator.

## Regression-pinning unit test (proposed)

A C++ unit test belongs alongside `UpdateModification::parseFromOplogEntry`
(e.g. `src/mongo/db/ops/write_ops_parsers_test.cpp` or the nearest
equivalent in this checkout). Shape:

```cpp
TEST(UpdateModificationParseFromOplog, ReplacementWithReservedKeysIsNotDelta) {
    for (auto&& key : {"$diff", "$set", "$unset", "$v", "$rename", "$inc"}) {
        BSONObj replacement = BSON("_id" << 1 << key << BSON("a" << 1));
        auto mod = write_ops::UpdateModification::parseFromOplogEntry(
            replacement, {true});
        ASSERT(mod.type() ==
               write_ops::UpdateModification::Type::kReplacement);
        ASSERT_BSONOBJ_EQ(mod.getUpdateReplacement(), replacement);
    }
}
```

The matching end-to-end coverage lives at
`jstests/replsets/oplog_update_reserved_field_names.js`. Both must be
kept green together — the C++ test pins parser intent; the jstest pins
that the replication pipeline composes the parser correctly.

## When this invariant is amended

If a future oplog version legitimately needs new reserved top-level
keys, bump `$v` to a new sentinel value AND add the new key to the
jstest matrix under that sentinel. The invariant text above must then
specify: discrimination is by `$v` value, never by reserved-name
presence under the replacement branch.
