/**
 * Tests that resumable index builds can resume after restart when sorter checksum version was
 * changed.
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   requires_fcv_73
 * ]
 */

import {setUpServerForColumnStoreIndexTest} from "jstests/libs/columnstore_util.js";
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

for (let initialFeatureFlagValue of [false, true]) {
    const rst = new ReplSetTest({
        nodes: 1,
        nodeOptions: {setParameter: {featureFlagUseSorterChecksumV2: initialFeatureFlagValue}}
    });
    rst.startSet();
    rst.initiate();

    const columnstoreEnabled = setUpServerForColumnStoreIndexTest(rst.getPrimary().getDB(dbName));

    const indexSpecs = [[{a: 1}]];
    if (columnstoreEnabled) {
        indexSpecs.push([{"$**": "columnstore"}]);
    }

    const coll = rst.getPrimary().getDB(dbName).getCollection(jsTestName());
    assert.commandWorked(coll.insertMany(docs));

    rst.awaitReplication();
    assert.commandWorked(rst.getPrimary().adminCommand({fsync: 1}));

    ResumableIndexBuildTest.run(
        rst,
        dbName,
        coll.getName(),
        indexSpecs,
        failPoint,
        failPointIterations,
        ["collection scan"],
        [{numScannedAfterResume: docCount - failPointIterations}],
        [],
        [],
        {setParameter: {featureFlagUseSorterChecksumV2: !initialFeatureFlagValue}});

    rst.stopSet();
}
