/**
 * Nodes using oplogWriterThreadCount keep replicating correctly while the primary role is handed
 * over between them under write load.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kOplogWriterThreadCount = 4;
const kDocsPerRound = 3000;
// Each round must land at least this many writes before its stepUp, so role changes happen under
// real load rather than on an idle set.
const kMinSuccessesBeforeStepUp = 200;

const rst = new ReplSetTest({
    name: "oplog_writer_stepdown",
    nodes: [
        {setParameter: {oplogWriterThreadCount: kOplogWriterThreadCount}},
        {setParameter: {oplogWriterThreadCount: kOplogWriterThreadCount}},
        {setParameter: {oplogWriterThreadCount: kOplogWriterThreadCount}},
    ],
    settings: {chainingAllowed: false},
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

if (!FeatureFlagUtil.isEnabled(primary, "ReduceMajorityWriteLatency")) {
    jsTestLog("featureFlagReduceMajorityWriteLatency is disabled; skipping test.");
    rst.stopSet();
    quit();
}

const dbName = "test";
const collName = "oplog_writer_stepdown";

assert.commandWorked(primary.getDB(dbName)[collName].insert({_id: "seed"}, {writeConcern: {w: 3}}));
rst.awaitReplication();

// Inserts documents against one node. Only errors expected from a role change are tolerated: the
// node losing primary, or the connection being closed or interrupted. Any other error fails the
// shell, and therefore the test.
function makeLoad(round, startId, count) {
    return funWithArgs(function(dbName, collName, round, startId, count) {
        const bigString = "s".repeat(64 * 1024);  // byte-heavy batches on the secondaries.
        const testDB = db.getSiblingDB(dbName);
        let acked = 0;
        let tolerated = 0;
        for (let i = startId; i < startId + count; i++) {
            let res;
            try {
                res = testDB.runCommand({insert: collName, documents: [{_id: i, payload: bigString}]});
            } catch (e) {
                if (isNetworkError(e) || ErrorCodes.isNotPrimaryError(e.code) ||
                    ErrorCodes.isInterruption(e.code)) {
                    tolerated++;
                    continue;
                }
                throw e;
            }
            const errors = res.ok ? (res.writeErrors || []) : [res];
            if (res.writeConcernError) {
                errors.push(res.writeConcernError);
            }
            if (errors.length > 0) {
                for (const err of errors) {
                    assert(ErrorCodes.isNotPrimaryError(err.code) || ErrorCodes.isInterruption(err.code),
                           "unexpected write error during role change: " + tojson(res));
                }
                tolerated++;
                continue;
            }
            assert.commandWorked(res);
            assert.eq(1, res.n, "insert did not acknowledge one document: " + tojson(res));
            acked++;
        }
        jsTestLog("Round " + round + ": acked=" + acked + " tolerated=" + tolerated);
    }, dbName, collName, round, startId, count);
}

function countRoundDocs(node, startId, count) {
    return node.getDB(dbName)[collName]
        .find({_id: {$gte: startId, $lt: startId + count}})
        .readConcern("local")
        .itcount();
}

// Starts load on 'node' and waits until it has landed enough writes before returning.
function startLoad(round, node, startId) {
    const join = startParallelShell(makeLoad(round, startId, kDocsPerRound), node.port);
    assert.soon(() => countRoundDocs(node, startId, kDocsPerRound) >= kMinSuccessesBeforeStepUp,
                "round " + round + " load did not make progress before stepUp");
    return join;
}

// stepUp() is used instead of replSetStepDown, which can close the client connection.
jsTestLog("Round 1: driving load, then stepping up a secondary.");
const round1Start = 0;
let load1 = startLoad(1, primary, round1Start);
const targetPrimary = rst.getSecondaries()[0];
rst.stepUp(targetPrimary, {awaitReplicationBeforeStepUp: false});
load1();  // Fails the test if the shell hit an unexpected error.

let newPrimary = rst.getPrimary();
assert.eq(targetPrimary.host, newPrimary.host, "expected the selected secondary to become primary");
assert.neq(primary.host, newPrimary.host, "expected a different primary after role handoff");

jsTestLog("Round 2: driving load against the new primary, then stepping up the original node.");
const round2Start = 100000;
let load2 = startLoad(2, newPrimary, round2Start);
rst.stepUp(rst.nodes[0], {awaitReplicationBeforeStepUp: false});
load2();

jsTestLog("Waiting for the set to converge and verifying consistency.");
rst.awaitReplication();
rst.checkOplogs();
rst.checkReplicatedDataHashes();

primary = rst.getPrimary();
for (const [round, start] of [[1, round1Start], [2, round2Start]]) {
    const landed = countRoundDocs(primary, start, kDocsPerRound);
    jsTestLog("Round " + round + " documents on the final primary: " + landed);
    assert.gte(landed,
               kMinSuccessesBeforeStepUp,
               "round " + round + " did not keep enough writes across the role change");
}

const expectedCount = primary.getDB(dbName)[collName].find().itcount();
for (const node of rst.nodes) {
    node.setSecondaryOk();
    assert.eq(expectedCount,
              node.getDB(dbName)[collName].find().itcount(),
              "node " + node.host + " has an inconsistent document count after role changes");
}

rst.stopSet();
