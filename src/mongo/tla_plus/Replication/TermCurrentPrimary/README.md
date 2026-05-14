# TermCurrentPrimary

Formal model for SERVER-125438: a follower learns about a new term via the
heartbeat / log fetcher path, but the existing code only advances the term
number; the recorded primary index for that term is left pointing at a stale
primary from an earlier term. The fix is to update both fields atomically.

## Files

- `TermCurrentPrimary.tla` — single specification module. Parameterised by
  `AtomicPrimaryUpdate`. When `TRUE`, `LearnNewTerm` advances both
  `currentTerm[i]` and `currentPrimaryIndex[i]` atomically (the fix). When
  `FALSE`, the term advances but `currentPrimaryIndex[i]` is unchanged (the
  pre-fix behaviour).
- `MCTermCurrentPrimary.tla` — model-checking overlay; introduces `MaxTerm`
  and a `StateConstraint` so TLC terminates quickly.
- `MCTermCurrentPrimary.cfg` — **green** config (`AtomicPrimaryUpdate = TRUE`).
  Safety + liveness must both hold.
- `MCTermCurrentPrimaryBug.cfg` — **bug** config (`AtomicPrimaryUpdate =
  FALSE`). Expects an invariant violation, demonstrating that the spec
  catches the bug.

## Property

`PrimaryIndexConsistentWithTerm`: for every node `i`, the recorded primary
index for `currentTerm[i]` is either `Nil` (not yet learned) or matches the
real elected primary of that term. A node must never point at a primary that
was retired by an earlier term boundary.

`EveryNodeEventuallyLearnsCurrentPrimary` (liveness): once a term has an
elected primary, every node at that term eventually learns the correct
primary index.

## Running TLC

```
cd src/mongo/tla_plus
./model-check.sh Replication/TermCurrentPrimary
```

The default run uses `MCTermCurrentPrimary.cfg` (green) and exits clean. To
reproduce the bug counterexample, temporarily swap in the bug config:

```
cd src/mongo/tla_plus/Replication/TermCurrentPrimary
mv MCTermCurrentPrimary.cfg MCTermCurrentPrimary.cfg.green
cp MCTermCurrentPrimaryBug.cfg MCTermCurrentPrimary.cfg
cd ../.. && ./model-check.sh Replication/TermCurrentPrimary
# TLC reports PrimaryIndexConsistentWithTerm violated within ~4 states.
# Restore the green cfg afterwards.
```
