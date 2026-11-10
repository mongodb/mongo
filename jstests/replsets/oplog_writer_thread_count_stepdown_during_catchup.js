/**
 * A node that steps down before it becomes a writable primary must not clear the
 * oplogTruncateAfterPoint set by an oplog batch that its writer has not finished. Otherwise a crash
 * after stepdown could keep later entries of the batch while earlier ones are missing.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

const rst = new ReplSetTest({
    nodes: [
        {setParameter: {oplogWriterThreadCount: 4}},
        {},
        // Votes for node 0 while it is behind node 1.
        {rsConfig: {priority: 0}},
    ],
    settings: {chainingAllowed: false, catchUpTakeoverDelayMillis: -1},
});
const nodes = rst.startSet();
rst.initiate();
rst.stepUp(nodes[1]);
rst.awaitReplication();

if (!FeatureFlagUtil.isEnabled(nodes[1], "ReduceMajorityWriteLatency")) {
    jsTest.log.info("Skipping test because featureFlagReduceMajorityWriteLatency is disabled");
    rst.stopSet();
    quit();
}

function getTruncateAfterPoint(node) {
    const doc = node
        .getDB("local")
        .replset.oplogTruncateAfterPoint.find({_id: "oplogTruncateAfterPoint"})
        .readConcern("local")
        .toArray()[0];
    return doc ? doc.oplogTruncateAfterPoint : Timestamp(0, 0);
}

const coll = nodes[1].getDB("test").coll;
assert.commandWorked(coll.insert({_id: "seed"}, {writeConcern: {w: 3}}));
rst.awaitReplication();
stopServerReplication(nodes[2]);
const holdWriter = configureFailPoint(nodes[0], "rsSyncApplyStop");
let fp;
const kLoadDocs = 64;
try {
    holdWriter.wait({maxTimeMS: 60 * 1000});
    const initialBuffered =
        assert.commandWorked(nodes[0].adminCommand({serverStatus: 1})).metrics.repl.buffer.write;
    fp = configureFailPoint(nodes[0], "hangOplogWriterBeforeWritingOplogBatchRange", {
        beginOffset: 0,
    });
    // 16MiB of buffered payload ensures the byte-based scheduler uses parallel ranges.
    const payload = "a".repeat(256 * 1024);
    for (let i = 0; i < kLoadDocs; ++i) {
        assert.commandWorked(coll.insert({_id: i, x: payload}, {writeConcern: {w: 1}}));
    }
    assert.soon(() => {
        const buffered =
            assert.commandWorked(nodes[0].adminCommand({serverStatus: 1})).metrics.repl.buffer.write;
        return buffered.count - initialBuffered.count >= kLoadDocs &&
            buffered.sizeBytes - initialBuffered.sizeBytes >= kLoadDocs * payload.length;
    }, "writer buffer did not accumulate enough new load entries and bytes");
    holdWriter.off();
    fp.wait({maxTimeMS: 60 * 1000});

    nodes[0].setSecondaryOk();
    const truncatePoint = getTruncateAfterPoint(nodes[0]);
    assert.neq(0, timestampCmp(truncatePoint, Timestamp(0, 0)), "batch did not set the truncate point");

    // hello reports secondary until the elected primary can accept writes.
    assert.soonNoExcept(() => assert.commandWorked(nodes[0].adminCommand({replSetStepUp: 1})));
    assert.soon(() => {
        const status = assert.commandWorked(nodes[0].adminCommand({replSetGetStatus: 1}));
        const hello = assert.commandWorked(nodes[0].adminCommand({hello: 1}));
        return status.myState === ReplSetTest.State.PRIMARY && hello.isWritablePrimary === false;
    }, "node 0 did not enter catch-up or drain");

    rst.stepUp(nodes[1], {awaitReplicationBeforeStepUp: false});
    rst.awaitNodesAgreeOnPrimary(rst.timeoutMS, nodes, nodes[1]);
    assert.soon(() => assert.commandWorked(nodes[0].adminCommand({hello: 1})).secondary);
    checkLog.containsJson(nodes[0], 10727101);

    assert.eq(
        0,
        timestampCmp(getTruncateAfterPoint(nodes[0]), truncatePoint),
        "stepdown cleared the truncate point of an unfinished batch",
    );
} finally {
    if (fp) {
        fp.off();
    }
    holdWriter.off();
    restartServerReplication(nodes[2]);
}
rst.awaitReplication();
assert.soon(() => timestampCmp(getTruncateAfterPoint(nodes[0]), Timestamp(0, 0)) === 0);
rst.checkOplogs();
rst.checkReplicatedDataHashes();
rst.stopSet();
