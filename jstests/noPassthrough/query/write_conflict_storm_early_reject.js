/**
 * SERVER-126462: pins the storm-prevention behavior of the proposed
 * early-reject backoff algorithm in PlanExecutorImpl::_handleNeedYield.
 *
 * With low thresholds (releaseBackoff=2, maxRetry=5) and K parallel ops
 * contending on a hot document held by an open transaction, the test
 * asserts:
 *   - at least one client observes TemporarilyUnavailable (Phase-C shed);
 *   - the server-side writeConflicts metric grew but stayed below the
 *     unlimited-retry upper bound (K * maxRetry), proving the cap is
 *     active rather than a coincidental success;
 *   - after the holder commits, surviving ops complete within a bounded
 *     retry budget.
 *
 * This test will FAIL on a build that has not landed the early-reject
 * branch — by design. It is the regression-pin half of the design doc.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 *   uses_transactions,
 *   featureFlagWriteConflictStormEarlyReject,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {Thread} from "jstests/libs/parallelTester.js";

const kReleaseBackoff = 2;
const kMaxRetry = 5;
const kStormParallelism = kMaxRetry * 3;  // 15 contenders, well over the cap.

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            internalQueryWriteConflictReleaseBackoff: kReleaseBackoff,
            internalQueryWriteConflictMaxRetry: kMaxRetry,
            // Keep the legacy alias coherent for the rollout window.
            internalQueryEnableWriteConflictBackoffWithoutTicket: true,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const collName = jsTestName();
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({_id: 0, counter: 0}));

// Snapshot the writeConflicts metric so we can bound growth at the end.
const beforeStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
const wcBefore = beforeStatus.metrics.operation.writeConflicts || 0;

// Hold a write on _id:0 inside a transaction. Every contender will
// fail with WriteConflict until this session commits.
const holderSession = primary.startSession({causalConsistency: false});
const holderColl = holderSession.getDatabase(dbName).getCollection(collName);
holderSession.startTransaction();
assert.commandWorked(holderColl.update({_id: 0}, {$inc: {counter: 1}}));

// Worker body: try the same findAndModify and report the terminal status.
// We deliberately use a generous maxTimeMS so the server-side cap (not
// the client deadline) is what shapes the outcome.
function stormWorker(host, dbName, collName) {
    const conn = new Mongo(host);
    const coll = conn.getDB(dbName).getCollection(collName);
    const result = coll.runCommand({
        findAndModify: collName,
        query: {_id: 0},
        update: {$inc: {counter: 1}},
        maxTimeMS: 30 * 1000,
    });
    return {ok: result.ok, code: result.code, codeName: result.codeName};
}

const threads = [];
for (let i = 0; i < kStormParallelism; i++) {
    const t = new Thread(stormWorker, primary.host, dbName, collName);
    t.start();
    threads.push(t);
}

// Let the storm reach steady state before releasing the holder.
sleep(2 * 1000);
assert.commandWorked(holderSession.commitTransaction_forTesting());

// Drain.
const outcomes = threads.map((t) => {
    t.join();
    return t.returnData();
});

// Phase-C assertion: at least one contender saw the cap fire.
const tempUnavail = outcomes.filter(
    (o) => o.code === ErrorCodes.TemporarilyUnavailable);
assert.gte(
    tempUnavail.length,
    1,
    `expected at least one TemporarilyUnavailable shed; outcomes=${tojson(outcomes)}`);

// Bound assertion: total observed writeConflicts grew, but stayed below
// the unlimited-retry ceiling (K contenders * maxRetry retries each).
const afterStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
const wcAfter = afterStatus.metrics.operation.writeConflicts || 0;
const wcGrowth = wcAfter - wcBefore;
assert.gt(wcGrowth, 0, "writeConflicts metric did not grow — storm did not engage");
assert.lte(
    wcGrowth,
    kStormParallelism * kMaxRetry,
    `writeConflicts growth ${wcGrowth} exceeded the K*maxRetry ceiling ` +
        `${kStormParallelism * kMaxRetry} — cap did not fire`);

// Recovery assertion: post-commit, the successful contenders together
// advanced the counter without anyone getting stuck.
const successCount = outcomes.filter((o) => o.ok === 1).length;
assert.gte(
    successCount,
    1,
    `expected at least one success after holder committed; outcomes=${tojson(outcomes)}`);
const finalDoc = testColl.findOne({_id: 0});
assert.eq(
    finalDoc.counter,
    1 + successCount,
    `counter should equal holder + successes; finalDoc=${tojson(finalDoc)} ` +
        `successes=${successCount}`);

rst.stopSet();
