# PageServerReader must not prefer a stale local cell on step-up

## Problem

`PageServerReader::_sortPageServerCandidates()` ranks candidates by
`score = lagInLsns + (remote ? gPageServerLocalZoneMatLagThreshold : 0)`. A
cell with no known frontier defaults `lagInLsns` to
`Timestamp::max().getSecs() ~= 4.29e9` (`page_server_reader.cpp:707`). This
encoding conflates two operationally distinct states:

1. *Remote known to be behind* — we have observed a frontier value below
   the requested LSN, so this remote really is a bad candidate.
2. *Remote frontier unknown* — we have not yet consumed the segment-2 log
   entries that publish it, so we have no evidence at all.

Right after step-up the second case is the common one. The new primary's
local frontier is a small (stale) number, every remote scores ~4.29e9, and
the sort puts the local cell first on every request. The local pagematd
never subscribed to the new segment, so each `GetPageAtLSN` eats the 14 s
timeout before falling back to a remote. The penalty compounds across the
many page reads needed to install a checkpoint, and step-up trips the task
idle timeout. `TimedOutWaitForMaterialization` responses carry the cell's
actual `highest_lsn`, but today that value is dropped, so the same stale
score is replayed on the next request.

## Fix

Introduce an explicit `FrontierState` tri-state:
`{ Known(lsn), BehindKnown(lsn), Unknown }`. Replace the
`Timestamp::max()`-as-Unknown encoding in
`_sortPageServerCandidates()`.

Sorting rule becomes:

- If the local cell's known frontier is < min `Known(...)` remote
  frontier, do **not** prefer local; pick a `Known(...)` remote.
- If **any** remote is `Unknown`, do **not** prefer local; pick a
  `Known(...)` remote when one exists, otherwise fall through to an
  arbitrary remote so the gossip / refresh path is exercised rather than a
  doomed local read.
- Only prefer local when every remote is `Known(...)` **and** local is
  not provably staler than the min remote frontier.

Feed `TimedOutWaitForMaterialization::highest_lsn` back into the frontier
tracker so subsequent sorts in the same `sendPageRequestSync` session use
the freshest information. Combined with a per-session per-cell failure
counter (per the ticket), one timeout is sufficient to flip the chosen
cell away from local for the remainder of step-up.

## Verified properties

The TLA+ model in
`src/mongo/tla_plus/Disagg/PageServerReaderStaleSorting/` checks the
invariant `NoServingStaleReadFromLocal`: no post-step-up pick may select
the local cell when (a) local's known frontier is below the min known
remote frontier, or (b) every remote frontier is `Unknown` and local
cannot serve the requested LSN. The `BugMC` config (`SortingRule =
"PreferLocalUnconditional"`) produces a counter-example; the `MC` config
with the fix rule (`PreferKnownOverUnknown`) passes.
