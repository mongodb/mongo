# OplogTruncationLiveness

Formal model of the oplog cap-maintainer's liveness obligation, motivated by a
production incident in which six fleet clusters stopped truncating oplog after
time-based retention was enabled on March 6, 2026.

## What the spec captures

The cap-maintainer thread is required to satisfy:

> If `MinRetentionHours > 0` and the oplog has grown beyond the retention
> cap, the maintainer MUST eventually shrink the oplog back to the cap.

Formally (in `OplogTruncationLiveness.tla`):

    EventuallyTruncates == [](Excess > 0 ~> Excess = 0)

The spec models four state variables:

| Variable      | Meaning                                                       |
|---------------|---------------------------------------------------------------|
| `oplogSize`   | Number of entries currently in the oplog (abstract units).    |
| `attemptSize` | Size of the truncation the maintainer will request next tick. |
| `truncated`   | Cumulative entries successfully removed.                      |
| `phase`       | `"running"` while the workload appends, `"drained"` after.    |

And five actions:

| Action                       | When it fires                                          |
|------------------------------|--------------------------------------------------------|
| `ProducerAppend`             | Workload appends 1..`MaxBurst` entries to the oplog.    |
| `WorkloadDrains`             | Workload stops generating new entries.                  |
| `MaintainerTruncateSuccess`  | Requested window fits the `CacheBudget`; shrink.        |
| `MaintainerTruncateConflict` | Requested window exceeds `CacheBudget`; WCE. Only enabled when `BackoffOnConflict = TRUE`. |
| `MaintainerRecomputeWindow`  | Maintainer is at its post-success reset and Excess exceeds the cap; raise the next request to Excess. |

`MaintainerTruncateConflict` models the WCE path. In the green config it
drops `attemptSize` to `CacheBudget` so the next cycle is guaranteed to
fit. In the bug config the action is disabled entirely - the prod
maintainer had no working backoff path, so we model that by removing
the action's enabling clause. The stuck state (drained, Excess > 0,
attemptSize > CacheBudget) then has no outgoing edge, which is exactly
the production behavior and the basis of the liveness counter-example.

## Why this maps to the prod incident

`SERVER-121352` records that the affected clusters had `Replication Oplog
Window` of roughly a week when time-based retention was first enabled.
The cap-maintainer woke, computed the excess (the full backlog), asked
WiredTiger to slow-truncate the entire backlog in one shot, hit the
~3 GB / 20%-of-cache slow-truncate budget, raised a
`WriteConflictException`, slept the retry interval, and asked for the
same oversize window again. The ticket comment dated 2026-04-08 shows
the `ReplicatedOplogTruncationThread` counter at `"attempts": 36` and
still climbing - direct evidence that the maintainer was making zero
progress.

The mitigation that shipped (`SERVER-122519`) shortened the wake interval
from 5 minutes to 30 seconds, so each cycle's requested truncation stays
well below the cache budget. In the spec that corresponds to setting
`BackoffOnConflict = TRUE`: each cycle requests a smaller window than
the previous failing one, eventually fits the cache, and makes progress.

## How to run

From `src/mongo/tla_plus`:

    ./download-tlc.sh             # one-time, requires network access
    ./model-check.sh Replication/OplogTruncationLiveness

That invokes the green config (`MCOplogTruncationLiveness.cfg`) which
sets `BackoffOnConflict = TRUE`. Expected output: invariants
`TypeOK` and `NeverTruncateRetained` hold, property
`EventuallyTruncates` holds, no liveness violation.

To exercise the bug config, run by hand from this directory after a
`download-tlc.sh`:

    java -cp ../../tla2tools.jar tlc2.TLC \
        -config MC_bug.cfg MCOplogTruncationLiveness

`MC_bug.cfg` sets `BackoffOnConflict = FALSE`. Expected output: TLC
reports `Error: Temporal properties were violated.` and emits a
counter-example trace ending in a stutter at a state with
`phase = "drained"`, `oplogSize > RetentionCap`, and
`attemptSize > CacheBudget`. From that state no action is enabled and
the oplog never shrinks, exactly mirroring the production behavior
reported on `sls-smoke-qa-aws-use1-restore-target` (Jira comment
2026-04-08) where `ReplicatedOplogTruncationThread` was logging
increasing `attempts` while the oplog continued to grow.

If `tla2tools.jar` cannot be downloaded in the current environment,
`./model-check.sh` returns with `No tla2tools.jar, run download-tlc.sh
first`. The spec itself is self-contained and can be opened in any
TLA+ Toolbox or TLC distribution that supports TLA+ 1.7 / Java 11+.

## State space

Both configs use:

    MaxBurst       = 2
    CacheBudget    = 3
    RetentionCap   = 2
    MaxOplogSize   = 8

These bounds were chosen to make the bug manifest within a handful of
states while keeping the green check tractable. The green check
explores 310 distinct states (depth 16) and reports no liveness
violation. The bug check explores the same state graph and reports
the temporal-property violation at depth 16.

Both configs set `CHECK_DEADLOCK FALSE`. Without that, TLC would
flag the natural terminal state (Excess = 0, phase = "drained", no
action enabled) as a deadlock in both configs and we'd lose the
ability to distinguish "bug" from "green". The actual production
issue surfaces purely as a liveness counter-example to
`EventuallyTruncates`.

## Paired empirical artifact

A jstest that simulates the same condition lives at
`jstests/replsets/oplog_cap_maintainer_truncation_liveness.js`. It
uses the `hangOplogCapMaintainerThread` failpoint (also used by
`jstests/replsets/libs/oplog_rollover_test.js`) to stall the
maintainer, accumulate oplog beyond the retention cap, then unblock
the thread and assert that within a bounded time the oplog shrinks
back below the cap. The failpoint is the documented runtime knob that
reproduces the prod scenario described in the ticket.
