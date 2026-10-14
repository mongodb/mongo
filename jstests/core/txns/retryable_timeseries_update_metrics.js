/**
 * Confirms that a retried time-series update records the retryable-write metrics.
 *   - transactions.retryableCommandsCount : total retryable time-series update commands received,
 *   - transactions.retriedCommandsCount / retriedStatementsCount : the retried subset.
 *
 * Runs against the suite's 1-node replica set so the counters are deterministic (no sharded
 * background writes). assumes_against_mongod_not_mongos keeps it off mongos/sharded passthroughs
 * (the counters are mongod-side), and multiversion_incompatible because retryableCommandsCount /
 * retriedWritesDelayMillis are new fields.
 *
 * @tags: [
 *   requires_replication,
 *   assumes_against_mongod_not_mongos,
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   multiversion_incompatible,
 * ]
 */

const coll = db.getCollection("jstests_retryable_timeseries_update_metrics");
coll.drop();

const timeFieldName = "time";
const metaFieldName = "tag";
assert.commandWorked(
    db.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    }),
);

assert.commandWorked(
    coll.insert({
        _id: 0,
        [timeFieldName]: ISODate("2021-01-01T01:00:00Z"),
        [metaFieldName]: "A",
        f: 0,
    }),
);

// Use a unique lsid per run. A fixed lsid would collide with a prior run's session when
// burn_in reuses the same fixture, causing this run's first update to be mistaken for a retry.
const lsid = {id: UUID()};
const cmd = {
    update: coll.getName(),
    updates: [
        {q: {[timeFieldName]: ISODate("2021-01-01T01:00:00Z")}, u: {$set: {f: 1}}, multi: false},
    ],
    lsid: lsid,
    txnNumber: NumberLong(1),
};

const initialStatus = db.serverStatus().transactions;
assert.hasFields(
    initialStatus,
    ["retryableCommandsCount", "retriedCommandsCount", "retriedStatementsCount"],
    "serverStatus.transactions missing the retryable-write metric fields",
);

jsTestLog.info("Initial status before test: ", initialStatus);
jsTestLog.info("Running time-series update command twice: ", cmd);

// First run performs the update; the identical replay is served as a retry.
assert.commandWorked(db.runCommand(cmd));
assert.commandWorked(db.runCommand(cmd));

const newStatus = db.serverStatus().transactions;
jsTestLog.info("New status after retrying time-series update: ", newStatus);

// Two retryable time-series update commands were received.
assert.eq(
    newStatus.retryableCommandsCount,
    initialStatus.retryableCommandsCount + 2,
    "expected retryableCommandsCount to increase by 2",
);
// Exactly one of them was a retried command (the replay), with one retried statement.
assert.eq(
    newStatus.retriedCommandsCount,
    initialStatus.retriedCommandsCount + 1,
    "expected retriedCommandsCount to increase by 1",
);
assert.eq(
    newStatus.retriedStatementsCount,
    initialStatus.retriedStatementsCount + 1,
    "expected retriedStatementsCount to increase by 1",
);

// Assert the retry-delay histogram gained one sample.
const totalInHist = (newStatus.retriedWritesDelayMillis || []).reduce((a, b) => a + b.count, 0);
const initialTotalInHist = (initialStatus.retriedWritesDelayMillis || []).reduce(
    (a, b) => a + b.count,
    0,
);
assert.eq(
    totalInHist,
    initialTotalInHist + 1,
    "expected retriedWritesDelayMillis to gain one sample",
);
