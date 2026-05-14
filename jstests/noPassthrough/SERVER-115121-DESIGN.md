# SERVER-115121 -- keysExamined / docsExamined render as negative in mongod slow-query logs

Ticket: https://jira.mongodb.org/browse/SERVER-115121
Priority: Critical -- P2 (Server Triage)
Branch: substrate-contrib/w3-104
Repro test: jstests/noPassthrough/slow_query_log_keys_examined_negative.js

## Symptom

Slow-query log lines (parsed via `lv` / `lq`) display
`keysExamined: NumberLong("-...")` and `docsExamined: NumberLong("-...")`.
Raw log text in a plain editor renders the same field as a large positive
NumberLong, because `lv` / `lq` reinterpret the BSON int64 as a signed
JavaScript bigint and the high bit is set. Customer-facing artifact is the
negative number; underlying corruption is a uint64 -> int64 narrowing.

## Fix site

`src/mongo/db/op_debug.cpp` -- `OpDebug::AdditiveMetrics::aggregateCursorMetrics`.

Specifically the lossy casts at lines 1741-1742 (and the matching pattern on
every uint64_t field that follows in that initializer list, through 1771):

    static_cast<uint64_t>(metrics.getKeysExamined()),
    static_cast<uint64_t>(metrics.getDocsExamined()),
    ...

These are read into a `query_stats::DataBearingNodeMetrics` whose fields are
`uint64_t` (see `src/mongo/db/query/query_stats/data_bearing_node_metrics.h`
lines 45-46). The struct is then handed to
`aggregateDataBearingNodeMetrics` (op_debug.cpp:1660), which does:

    keysExamined = keysExamined.value_or(0) + metrics.keysExamined;
    docsExamined = docsExamined.value_or(0) + metrics.docsExamined;

The LHS is `boost::optional<long long>` (declared op_debug.h:172-173), the
RHS is `uint64_t`. C++ usual arithmetic conversions promote the long long
to uint64_t, the addition happens in unsigned, and the result is reassigned
to a long long -- silently reinterpreting the high bit as the sign.

## Two distinct triggering paths

1. **Out-of-range single shard report.** The CursorMetrics IDL field is
   `long` (`src/mongo/db/query/client_cursor/cursor_response.idl:109-116`),
   i.e. signed int64. If any code path (uninitialized field decoded from a
   partial subobject, a future telemetry contributor, a buggy passthrough
   layer) ever produces a negative wire value, `static_cast<uint64_t>(-1)`
   evaluates to 2^64 - 1, which the merging accumulator then folds into a
   long long via the assignment above, instantly flipping the merged value
   to a large negative number.

2. **Accumulated overflow.** Even with strictly non-negative per-shard
   inputs, a sufficiently large multi-shard fan-out (large `$lookup`,
   change-stream merging across many shards, repeated `getMore` rounds in
   one OpDebug lifetime) can cumulatively cross 2^63 in the uint64_t
   intermediate. The narrowing-on-assignment then produces a negative
   long long. This is the path most consistent with the Slack thread:
   the symptom emerges only on heavy production workloads.

The same `getCursorMetrics` shape exists on the shard side at op_debug.cpp:
1395-1400 -- `metrics.setKeysExamined(additiveMetrics.keysExamined.value_or(0))`
takes a long long and writes it through an IDL `long` setter. That direction
is safe today (signed -> signed) but inherits the same vocabulary mismatch.

## Recommended fix sketch

Keep the on-the-wire CursorMetrics IDL type as `long` (signed) -- changing
it is an unstable-stability bump but also a wire-compat change we don't
want here. Instead, fix the narrowing at the accumulation site:

1. **Accumulator type alignment.** Either:

   a. Change `OpDebug::AdditiveMetrics::keysExamined` (and `docsExamined`,
      and the other numeric fields with this pattern -- see op_debug.h:172
      onward) to `boost::optional<uint64_t>`, OR

   b. Change `DataBearingNodeMetrics::keysExamined` (and siblings) back to
      `int64_t` and have `aggregateDataBearingNodeMetrics` clamp negative
      inputs with a tassert + LOGV2_WARNING; do the addition entirely in
      signed long long with explicit overflow check (e.g.
      `mongo::overflow::add`).

   Option (b) is the smaller blast radius for SERVER-115121: it preserves
   the public-facing log type (BSON int64 / NumberLong) and adds an
   explicit, observable detection point for the upstream bug that produces
   a negative wire value.

2. **Input validation at the cast boundary.** In `aggregateCursorMetrics`
   (op_debug.cpp:1739), tassert that `metrics.getKeysExamined() >= 0` and
   that `metrics.getKeysExamined() <= INT64_MAX -
   additiveMetrics.keysExamined.value_or(0)` before the cast. tassert is
   appropriate since a negative wire value is a bug *somewhere* upstream
   and we want the SDP to fire rather than silently log a negative.

3. **Idempotent renderer guard.** As belt-and-braces, in
   `OpDebug::report` / `OPDEBUG_APPEND_OPTIONAL` (op_debug.cpp:635-636 for
   the BSON path, 336-337 for the structured-log path), assert
   non-negative before append. This makes the test in this branch fail
   loudly even if the accumulator regression returns under another guise.

## Test-only failpoint (referenced by the repro)

The behavioral repro in `slow_query_log_keys_examined_negative.js` exercises
path (2) above with a realistic-but-modest workload (4000 docs across 2
shards, multiple getMore rounds, $group + $sort + allowDiskUse). The
appendix at the bottom of that file documents a `injectCursorMetricsForTesting`
failpoint pattern -- a single `MONGO_FAIL_POINT_DEFINE` in
`OpDebug::getCursorMetrics` plus a `failpoint.executeIf(...)` block that
overrides `metrics.setKeysExamined` / `setDocsExamined` with operator-supplied
values from the failpoint `BSONObj`. Landing that failpoint is a 10-line
change and makes the overflow path deterministically reproducible in CI
(via two `NEAR_MAX` per-shard contributions that sum past INT64_MAX on the
merger).

The fixer is encouraged to land the failpoint as part of the fix PR; the
behavioral repro stays useful as a non-failpoint guard against the
out-of-range single-shard-report regression.

## Out of scope for this branch

- The slow-log printers themselves (`lv`, `lq`) are correct -- they print
  what they parse. No change there.
- `OpDebug::getCursorMetrics` on the shard side is currently safe
  (signed-to-signed); revisit if accumulator type is changed in option (1a)
  above.
- Query-stats aggregation in `data_bearing_node_metrics.h` shares the same
  vocabulary mismatch but is keyed on uint64_t throughout -- so its own
  output is correct; only the bridge into `OpDebug::AdditiveMetrics`
  exhibits the narrowing bug.

## References

- op_debug.h lines 172-173: `boost::optional<long long> keysExamined; docsExamined;`
- op_debug.cpp lines 1660-1672: `aggregateDataBearingNodeMetrics` (the assignment)
- op_debug.cpp lines 1739-1772: `aggregateCursorMetrics` (the cast)
- data_bearing_node_metrics.h lines 45-46, 90-92, 154-156:
    uint64_t field decl, `add()`, and `aggregateCursorMetrics()`
- cursor_response.idl lines 105-116: wire-type declaration (signed `long`)
- op_debug.cpp lines 1395-1400: `getCursorMetrics` (shard outbound, signed)
