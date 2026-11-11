/**
 * killOp does not interrupt the oplog write tasks run by the oplogWriterThreadCount pool.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {setParameter: {oplogWriterThreadCount: 4}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

if (!FeatureFlagUtil.isEnabled(primary, "ReduceMajorityWriteLatency")) {
    jsTest.log.info("Skipping test because featureFlagReduceMajorityWriteLatency is disabled");
    rst.stopSet();
    quit();
}

const coll = primary.getDB("test").coll;
assert.commandWorked(coll.insert({_id: "seed"}, {writeConcern: {w: 2}}));
rst.awaitReplication();
secondary.setSecondaryOk();

assert.commandWorked(
    secondary.adminCommand({setParameter: 1, logComponentVerbosity: {command: {verbosity: 3}}}),
);
const holdWriter = configureFailPoint(secondary, "rsSyncApplyStop");
let fp;
const kLoadDocs = 64;
try {
    holdWriter.wait({maxTimeMS: 60 * 1000});
    const initialBuffered =
        assert.commandWorked(secondary.adminCommand({serverStatus: 1})).metrics.repl.buffer.write;
    fp = configureFailPoint(secondary, "hangOplogWriterBeforeWritingOplogBatchRange", {
        beginOffset: 0,
    });
    // 16MiB of buffered payload ensures the byte-based scheduler uses parallel ranges.
    const payload = "a".repeat(256 * 1024);
    for (let i = 0; i < kLoadDocs; ++i) {
        assert.commandWorked(coll.insert({_id: i, x: payload}, {writeConcern: {w: 1}}));
    }
    assert.soon(() => {
        const buffered =
            assert.commandWorked(secondary.adminCommand({serverStatus: 1})).metrics.repl.buffer.write;
        return buffered.count - initialBuffered.count >= kLoadDocs &&
            buffered.sizeBytes - initialBuffered.sizeBytes >= kLoadDocs * payload.length;
    }, "writer buffer did not accumulate enough new load entries and bytes");
    holdWriter.off();
    fp.wait({maxTimeMS: 60 * 1000});

    let writeOps = [];
    assert.soon(() => {
        writeOps = secondary
            .getDB("admin")
            .aggregate([{$currentOp: {allUsers: true, idleConnections: true}}])
            .toArray()
            .filter((op) => op.desc && op.desc.startsWith("OplogWriterWorker-") && op.opid);
        return writeOps.length > 0;
    }, "no OplogWriterWorker operation found");

    clearRawMongoProgramOutput();
    const opId = writeOps[0].opid;
    assert.commandWorked(secondary.getDB("admin").killOp(opId));
    assert.soon(
        () => rawMongoProgramOutput("11227300.*Not killing exempt op").match('"opId":' + opId),
        "killOp was not ignored for opId " + opId,
    );
} finally {
    if (fp) {
        fp.off();
    }
    holdWriter.off();
}
rst.awaitReplication();
assert.commandWorked(secondary.adminCommand({ping: 1}));
assert.eq(kLoadDocs + 1, secondary.getDB("test").coll.find().readConcern("local").itcount());

rst.stopSet();
