# MergeSpillCursorReset

Formal model of the merge-spill phase of the external sorter under
WiredTiger WriteConflictException (WCE) retry. Pins SERVER-124271.

## Bug summary

`ContainerBasedSpiller::mergeSpills()` drives a `MergeIterator` from
inside a `writeConflictRetry` lambda. On WCE the helper calls
`rollback_transaction()` on the WT session, which resets *every* cursor
on that session — including the read cursors backing each input spill's
`ContainerIterator`. The merge iterator's in-memory heap state is not
rewound, so the next `mergeIterator->next()` call either silently skips
a key (some input cursor still positioned past the popped row) or
re-yields one already consumed (cursor reset to snapshot start). The
classic symptom is an output spill whose element count or checksum
differs across WCE retry trials of the same input.

## Model shape

Two abstract actors:

- **Merger** — owns `RHead[s]` (next-unread offset into `InputKeys[s]`
  for each spill `s`), exposes the minimum visible head as the next
  element to consume.
- **WTSession** — phases `idle → writing → rolled_back`, owns the
  uncommitted batch buffer and the retry anchor `RHeadAnchor`.

Five actions: `ReadHead`, `WCE`, `Resume`, `Commit`, `FlushFinal`. Two
safety invariants: `NoElementLoss` (terminal multiset equality, output
≥ input) and `NoElementDuplication` (step-wise: output + in-flight batch
≤ input on every key).

The central parameter is `ResetAffectsReadCursors ∈ BOOLEAN`:

- `FALSE` — proposed fix (drain the `MergeIterator` into a
  `std::vector` before entering the write loop). Read offsets are
  not touched by rollback. Both invariants hold.
- `TRUE` — current production semantics. TLC produces a counterexample
  trace violating `NoElementLoss` or `NoElementDuplication`.

## Running

```
cd src/mongo/tla_plus
./download-tlc.sh                                # one-time
./model-check.sh Sorter/MergeSpillCursorReset    # uses MCMergeSpillCursorReset.cfg
```

To re-run with the buggy semantics, swap the cfg by hand:

```
cd src/mongo/tla_plus/Sorter/MergeSpillCursorReset
cp MCMergeSpillCursorResetBug.cfg MCMergeSpillCursorReset.cfg.bak.green
cp MCMergeSpillCursorResetBug.cfg MCMergeSpillCursorReset.cfg.in   # then edit ResetAffectsReadCursors
```

(or call TLC directly:
`java -cp ../../tla2tools.jar tlc2.TLC -config MCMergeSpillCursorResetBug.cfg MCMergeSpillCursorReset.tla`).

## Companion regression test

`jstests/noPassthrough/merge_spill_wce_idempotency.js` exercises the
same scenario end-to-end: it forces a `$group` (or `$sort`) pipeline to
spill, then uses the `WTWriteConflictException` failpoint with
`nTimes >= 2` to drive multiple merge-spill rollbacks and asserts that
the output document count and content checksum are bit-identical to a
control run with the failpoint off.
