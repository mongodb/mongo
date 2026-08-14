/**
 * Tests that cross-database renameCollection is idempotent when the local rename succeeds but its
 * participant phase is not checkpointed. The retry must converge both when the source collection
 * still needs to be dropped and when it was already dropped.
 *
 * @tags: [
 *   requires_replication,
 *   uses_rename,
 * ]
 */
import {
    createCollectionAndInsertDocuments,
    setupTestDatabase,
    verifyCollectionTrackingState,
} from "jstests/libs/sharded_cluster_fixture_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const kSourceDbName = `${jsTestName()}_source`;
const kTargetDbName = `${jsTestName()}_target`;
const kSourceCollName = "source";
const kTargetCollName = "target";
const kSourceDocuments = [
    {_id: 0, value: "source-0"},
    {_id: 1, value: "source-1"},
];
const kOldTargetDocuments = [{_id: -1, value: "old-target"}];
const kBeforeSourceDropFailpoint = "failRenameAfterFinalizeButBeforeSourceDrop";
const kAfterSourceDropFailpoint = "failRenameAfterFinalizeAndAfterSourceDrop";

function dropTestDatabases() {
    assert.commandWorked(st.s.getDB(kSourceDbName).dropDatabase());
    assert.commandWorked(st.s.getDB(kTargetDbName).dropDatabase());
}

function setupTestCase(shardingTest, collectionType, targetExists) {
    const sourceDb = setupTestDatabase(
        shardingTest.s.getDB("admin"),
        kSourceDbName,
        shardingTest.shard0.shardName,
    );
    const targetDb = setupTestDatabase(
        shardingTest.s.getDB("admin"),
        kTargetDbName,
        shardingTest.shard0.shardName,
    );
    const sourceDataShard =
        collectionType === "unsplittable" ? shardingTest.shard1 : shardingTest.shard0;
    const sourceNss = `${kSourceDbName}.${kSourceCollName}`;
    const targetNss = `${kTargetDbName}.${kTargetCollName}`;

    createCollectionAndInsertDocuments(
        collectionType,
        sourceDb,
        kSourceCollName,
        kSourceDocuments,
        sourceDataShard,
    );
    if (targetExists) {
        createCollectionAndInsertDocuments(
            collectionType,
            targetDb,
            kTargetCollName,
            kOldTargetDocuments,
            shardingTest.shard0,
        );
    }

    return {
        sourceDb,
        targetDb,
        sourceNss,
        targetNss,
        sourceDataPrimary: sourceDataShard.rs.getPrimary(),
        oldTargetUUID: targetExists ? getUUIDFromListCollections(targetDb, kTargetCollName)
                                    : undefined,
    };
}

function assertRenameSuccessful(shardingTest, collectionType, testCase) {
    assert(!testCase.sourceDb[kSourceCollName].exists(),
            `source collection still exists: ${testCase.sourceNss}`);
    assertArrayEq({
        actual: testCase.targetDb[kTargetCollName].find().toArray(),
        expected: kSourceDocuments,
    });

    if (testCase.oldTargetUUID) {
        const newTargetUUID =
            getUUIDFromListCollections(testCase.targetDb, kTargetCollName);
        assert.neq(
            testCase.oldTargetUUID,
            newTargetUUID,
            `pre-existing target collection was not replaced by the rename: ${testCase.targetNss}`,
        );
    }

    if (collectionType === "unsplittable") {
        verifyCollectionTrackingState(shardingTest.s.getDB("admin"), testCase.sourceNss, false);
        verifyCollectionTrackingState(
            shardingTest.s.getDB("admin"),
            testCase.targetNss,
            true,
            true,
        );
    }
}

function runRenameCollectionAcrossDBs(
    shardingTest,
    {collectionType, failpointName, dropTarget, targetExists},
) {
    const testCase = setupTestCase(shardingTest, collectionType, targetExists);
    const fp = configureFailPoint(testCase.sourceDataPrimary, failpointName, {}, {times: 1});

    try {
        assert.commandWorked(
            shardingTest.s.adminCommand({
                renameCollection: testCase.sourceNss,
                to: testCase.targetNss,
                dropTarget,
            }),
        );
        // The rename only returns after the participant has failed once and retried to
        // completion, so the wait is trivially satisfied here. It still guards against the
        // failpoint being configured on a node the rename never reaches, in which case the
        // command would succeed without a retry and this wait would time out.
        fp.wait();
    } finally {
        fp.off();
    }

    assertRenameSuccessful(shardingTest, collectionType, testCase);
}

function runExistingTargetWithoutDropTargetCase(shardingTest, collectionType) {
    const testCase = setupTestCase(shardingTest, collectionType, true);

    assert.commandFailedWithCode(
        shardingTest.s.adminCommand({
            renameCollection: testCase.sourceNss,
            to: testCase.targetNss,
            dropTarget: false,
        }),
        ErrorCodes.NamespaceExists,
    );

    assertArrayEq({
        actual: testCase.sourceDb[kSourceCollName].find().toArray(),
        expected: kSourceDocuments,
    });
    assertArrayEq({
        actual: testCase.targetDb[kTargetCollName].find().toArray(),
        expected: kOldTargetDocuments,
    });
}

const kRetryCases = [
    {
        collectionType: "untracked",
        failpoint: kBeforeSourceDropFailpoint,
        dropTarget: false,
        targetExists: false,
        skip: true,
    },
    {
        collectionType: "untracked",
        failpoint: kBeforeSourceDropFailpoint,
        dropTarget: true,
        targetExists: false,
        skip: true,
    },
    {
        collectionType: "untracked",
        failpoint: kBeforeSourceDropFailpoint,
        dropTarget: true,
        targetExists: true,
        skip: true,
    },
    {
        collectionType: "untracked",
        failpoint: kAfterSourceDropFailpoint,
        dropTarget: false,
        targetExists: false,
    },
    {
        collectionType: "untracked",
        failpoint: kAfterSourceDropFailpoint,
        dropTarget: true,
        targetExists: false,
    },
    {
        collectionType: "untracked",
        failpoint: kAfterSourceDropFailpoint,
        dropTarget: true,
        targetExists: true,
    },
    {
        collectionType: "unsplittable",
        failpoint: kBeforeSourceDropFailpoint,
        dropTarget: false,
        targetExists: false,
        skip: true,
    },
    {
        collectionType: "unsplittable",
        failpoint: kBeforeSourceDropFailpoint,
        dropTarget: true,
        targetExists: false,
        skip: true,
    },
    {
        collectionType: "unsplittable",
        failpoint: kBeforeSourceDropFailpoint,
        dropTarget: true,
        targetExists: true,
        skip: true,
    },
    {
        collectionType: "unsplittable",
        failpoint: kAfterSourceDropFailpoint,
        dropTarget: false,
        targetExists: false,
    },
    {
        collectionType: "unsplittable",
        failpoint: kAfterSourceDropFailpoint,
        dropTarget: true,
        targetExists: false,
    },
    {
        collectionType: "unsplittable",
        failpoint: kAfterSourceDropFailpoint,
        dropTarget: true,
        targetExists: true,
    },
];
for (const testCase of kRetryCases) {
    const {collectionType, failpoint, dropTarget, targetExists, skip} = testCase;
    const label = `${collectionType}, ${failpoint}, dropTarget=${dropTarget}, targetExists=${targetExists}`;
    if (skip == true) {
        // TODO SERVER-130940: Re-Enable skipped tests
        jsTestLog(`skipping test: ${label}`);
        continue;
    }
    jsTestLog(`retries successfully: ${label}`);
    runRenameCollectionAcrossDBs(st, {
        collectionType,
        failpointName: failpoint,
        dropTarget,
        targetExists,
    });
    dropTestDatabases();
}

jsTestLog("rejects an untracked rename over an existing target with dropTarget=false");
runExistingTargetWithoutDropTargetCase(st, "untracked");
dropTestDatabases();

jsTestLog("rejects an unsplittable rename over an existing target with dropTarget=false");
runExistingTargetWithoutDropTargetCase(st, "unsplittable");
dropTestDatabases();

st.stop();
