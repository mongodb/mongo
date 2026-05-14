# SERVER-126377 — pagematd refresh-on-reconnect design

## Problem

When `pagematd` reconnects to a log server, the `ReadLog` subscription stream
is established with a segment filter fixed at connection time
(`log_server_manager.cpp:2545`). If CMS registers a new segment in the window
between the reconnect completing and the first seal arriving, the segment is
never in the filter and `pagematd` never sees it; the page server's
materialization frontier freezes at the prior boundary.

Observed in BF-43088: `pagematd1` reconnected to a new `logd1` at 13:23:55.133;
CMS registered segment `1:2` 183 ms later; `pagematd1` then emitted zero log
lines for `1:2` across 8.56 M subsequent log lines. The other two pagematd
instances, which reconnected after `1:2` was already registered, picked it up
correctly.

Root cause: the reconnect path treats the in-process segment view as a
delta-stream snapshot. The filter is rebuilt from local state and CMS is never
re-queried, so any segment that arrives between the previous view-sync and
the next reconnect is permanently invisible.

## Fix

Treat the CMS segment list as authoritative truth at every reconnect boundary,
not as a delta over a stale local snapshot.

In the `LogServerManager` reconnect path (around
`log_server_manager.cpp:2554`), add an explicit CMS topology refresh before
establishing the `ReadLog` subscription, analogous to the existing
`checkAndRefreshConfig`:

1. Before calling `ReadLog::subscribe`, issue a `getSegmentList` RPC to CMS
   and use its response as the subscription filter.
2. Diff the refreshed list against the local view; include any newly-discovered
   segment in the filter and tracked state.
3. Persist the refresh result before `subscribe` so a crash mid-reconnect
   resumes at the same observation point.

This closes the race: any segment registered up to the moment the CMS RPC
returns is in the filter when the stream opens. Later registrations either
land before the next reconnect (caught at the next refresh) or arrive on the
live stream as a push-side seal.

## Spec invariants

The companion spec
(`src/mongo/tla_plus/Disagg/PagematdSegmentReconnectRace/`) models CMS, log
server, and pagematd, and pins the liveness invariant
`EverySegmentEventuallySeen`. With `BugMode = TRUE` the subscription is rebuilt
from the local view on reconnect and TLC produces a counterexample matching
the BF-43088 trace; with `BugMode = FALSE` it is rebuilt from CMS and the
invariant holds.

## Test plan

- Unit: inject a `getSegmentList` response with a segment unknown to the local
  view; assert it lands in the filter and tracked state.
- Integration: kill the log server during a segment seal; assert the
  reconnected pagematd picks up the new segment.
- Regression: replay the BF-43088 timing (disconnect, register at +200 ms,
  reconnect); assert materialization advances past the new segment.
