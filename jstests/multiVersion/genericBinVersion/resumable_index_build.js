/**
 * Tests that resumable index builds can resume after restart with a different bin version
 * @tags: [
 *   requires_persistence,
 *   requires_replication
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

const dbName = "test";

// Create large enough docs so the index build will spill to disk.
const docCount = 200;
const docs = [];
const bigStr = new Array(1024 * 1024).join('x');
for (let i = 0; i < docCount; ++i) {
    docs.push({a: i, b: -i, padding: bigStr});
}

const failPoint =
    [{name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386}];
const failPointIterations = docCount - 5;

function runTest(initialBinVersion, binVersionAfterRestart, fcv) {
    jsTestLog("Running test " + jsTestName() + " for bin versions: " + initialBinVersion + " -> " +
              binVersionAfterRestart + " fcv: " + fcv);
    const rst = new ReplSetTest({nodes: 1, nodeOptions: {binVersion: initialBinVersion}});
    rst.startSet();
    rst.initiate();

    const db = rst.getPrimary().getDB(dbName);
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: fcv, confirm: true}));

    const coll = db.getCollection(jsTestName());
    assert.commandWorked(coll.insertMany(docs));

    rst.awaitReplication();
    assert.commandWorked(db.adminCommand({fsync: 1}));

    const indexSpecs = [[{a: 1}]];

    ResumableIndexBuildTest.run(rst,
                                dbName,
                                coll.getName(),
                                indexSpecs,
                                failPoint,
                                failPointIterations,
                                ["collection scan"],
                                [{numScannedAfterResume: docCount - failPointIterations}],
                                [],
                                [],
                                {binVersion: binVersionAfterRestart});

    rst.stopSet();
}

runTest("last-lts", "latest", lastLTSFCV);
runTest("latest", "last-lts", lastLTSFCV);
