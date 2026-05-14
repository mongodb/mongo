# ViewResolutionGetMore

Formal model of view resolution for `$lookup` / `$graphLookup` commands whose
cursor spans an initial batch plus one or more `getMore` batches, in the
presence of concurrent catalog mutations.

Tracks SERVER-121988: between view-resolution kickbacks (especially across
`getMore` boundaries) a drop+recreate flips the view definition, and the
second batch silently resolves the secondary `$lookup`/`$graphLookup` against
the new definition. The cursor returns zero matches with no error surfaced.

## Actors

1. **Mongos view-resolver** — dispatches the user command, receives a
   `CommandOnShardedViewNotSupportedOnMongod` kickback, refetches the view
   definition, re-dispatches, and opens the cursor.
2. **Catalog mutator** — fires `DropView`, `RecreateView`, and `SwapView`
   actions (swap = drop + recreate pointing at a different backing
   collection). Gated by `AllowViewMutationMidCommand`.
3. **Consumer / getMore** — drives the cursor: initial batch, then up to
   `MAX_BATCHES - 1` `getMore` calls, then `CloseCursor`.

## Invariant

`SingleViewDefinitionPerCommand`: for every command `c` whose cursor reached
`open`, the set of view definitions actually consumed by `c`'s `$lookup` /
`$graphLookup` stages across all batches is a singleton (`(gen, backing)` is
the natural identity). `NoSilentEmptyBatchAfterOk` is a complementary
user-facing invariant.

## Bug toggle

`AllowViewMutationMidCommand` ∈ BOOLEAN.

- `FALSE` (green) — catalog frozen while any cursor is open. Encoded in
  `MCViewResolutionGetMore.cfg`. TLC verifies the invariants hold.
- `TRUE` (bug) — catalog can mutate freely. Encoded in
  `MCViewResolutionGetMore_Bug.cfg`. TLC produces a counterexample for
  `SingleViewDefinitionPerCommand` and a separate trace for
  `NoSilentEmptyBatchAfterOk`.

## Running

```
cd src/mongo/tla_plus
./model-check.sh Catalog/ViewResolutionGetMore
```

Swap `MCViewResolutionGetMore.cfg` ↔ `MCViewResolutionGetMore_Bug.cfg` to
flip modes (the dispatcher reads the cfg named after the MC module).

Paired empirical reproduction: `jstests/aggregation/lookup_view_drop_recreate_getMore.js`.
