# SERVER-115952 — PSR backoff must treat read errors as failures

## Symptom

HELP-86312 surfaced a hot-loop scenario where a Page Server Reader (PSR)
attached to a lagged page-server cell repeatedly issued reads, each blocking
for the full 30s response deadline before timing out. The backoff state on the
underlying `PageServerReplicaSet` did not advance, so the client never
demoted the bad cell and never tried an out-of-cell replica. The result was
an effective unavailability window bounded only by client patience, despite
healthy peers being one hop away.

## Root cause

The PSR retry loop keeps two failure counters per upstream:

1. `_connectionFailures` — incremented when `network::connect()` returns a
   non-OK status (host unreachable, TLS handshake failure, etc.). This
   counter feeds `BackoffState::shouldEscalate()`, which is what eventually
   marks a peer unhealthy and rotates traffic to a sibling cell.
2. `_readFailures` — incremented on `responseFailed` paths that originate
   *after* a connection has been established (timeouts, malformed payloads,
   `NetworkInterfaceExceededTimeLimit`, `HostUnreachable` returned by an
   in-band error frame). This counter is plumbed into observability
   (`serverStatus.disagg.pageServer.readErrors`) but is **not** read by
   `BackoffState`.

Concretely, the timeout path in
`PageServerReplicaSet::_onReadComplete` calls `_recordReadError(...)` but
does not call `responseFailed(...)` on the backoff tracker. The escalator
therefore observes a steady stream of "0 connection failures" and concludes
that the peer is healthy.

## Fix

Collapse the split. Introduce a single `_requestFailures` counter that
`BackoffState::shouldEscalate()` consumes. Every code path that today
increments `_connectionFailures` or `_readFailures` instead routes through a
new `_recordRequestFailure(ErrorCategory cat, Status s)` helper that:

1. Increments the unified counter (drives backoff).
2. Increments a per-category sub-counter (preserves the
   `connectionErrors` / `readErrors` / `protocolErrors` breakdown in
   `serverStatus`).
3. Emits a structured log line at log level 2 with both the unified count
   and the category, so existing dashboards keep their fidelity.

The timeout path in `_onReadComplete` is changed to call
`_recordRequestFailure(ReadTimeout, status)`. This matches Aaron's last
comment on the ticket: "the required work for this ticket is to call
responseFailed from the timeout path."

## Why a unified counter is the right shape

A read-timeout against a lagged cell is, from the client's standpoint,
indistinguishable from a connection refusal: both mean "this peer cannot
serve my next request inside the SLA, route elsewhere." The previous split
encoded a transport-layer distinction that no consumer of `BackoffState`
ever needed. Keeping the breakdown for *observability* is a separate
concern from using it for *control*; the design preserves the former
without coupling it to the latter.

## Deferred follow-up

Aaron's comment also flags a sliding-window success/failure ratio as
future work, blocked on in-band error reporting. That is out of scope for
SERVER-115952; this ticket is the minimum-viable fix that unblocks the
HELP-86312 class of incident.

## Test

`jstests/noPassthrough/page_server_read_error_counts_as_failure.js`
injects read errors via failpoint and asserts that `BackoffState` escalates
inside a bounded retry budget rather than spinning indefinitely. The test
skips gracefully when disagg is not present in the build.
