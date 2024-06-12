/*
 * Tests pre-image truncation behavior across rollback-to-stable doesn't cause data corruption.
 *
 * @tags: [
 *   requires_replication,
 *   requires_mongobridge,
 * ]
 */
import {getPreImagesCollection} from "jstests/libs/change_stream_util.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const nodeOptions = {
    setParameter: {
        preImagesCollectionTruncateMarkersMinBytes: 1,
        expiredChangeStreamPreImageRemovalJobSleepSecs: 1,
    }
};
const dbName = jsTestName();

// Get's pre-images in their $natural order. First ordered by nsUUID, then their 'ts' field.
function getPreImages(node) {
    return getPreImagesCollection(node).find().hint({$natural: 1}).toArray();
}

const getPreImagesForAllNodes = (nodes) => {
    const res = {};
    for (let node of nodes) {
        res[node.host] = getPreImages(node);
    }
    return res;
};

const setExpireAfterSeconds = (node, expireAfterSeconds) => {
    assert.commandWorked(node.getDB("admin").runCommand({
        setClusterParameter:
            {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: expireAfterSeconds}}}
    }));
};

const baseDocs = [
    {baseDoc: 0},
    {baseDoc: 1},
];

// Operations that will be present on both nodes, before the common point.
//
// The common point includes the creation of 'commonColl' and its first set of pre-images.
// Sets 'expireAfterSeconds' once the common pre-images are inserted and
// 'collWithoutCommonPreImages' is created.
const CommonOps = (node, commonCollName, collWithoutCommonPreImagesName, expireAfterSeconds) => {
    jsTestLog(`Running common ops`);
    const testDB = node.getDB(dbName);
    const commonColl = assertDropAndRecreateCollection(
        testDB,
        commonCollName,
        {changeStreamPreAndPostImages: {enabled: true}},
    );
    assert.commandWorked(commonColl.insert(baseDocs));
    assert.commandWorked(commonColl.update({}, {$set: {collName: commonCollName}}, {multi: true}));
    assert.soon(() => getPreImages(node).length == baseDocs.length);

    // Create 'collWithoutCommonPreImages' but don't generate pre-images for it until node histories
    // diverge.
    const collWithoutCommonPreImages = assertDropAndRecreateCollection(
        testDB,
        collWithoutCommonPreImagesName,
        {changeStreamPreAndPostImages: {enabled: true}},
    );

    setExpireAfterSeconds(node, expireAfterSeconds);
};

// Operations that will be performed on the rollback node past the common point.
//
// Generates pre-images eventually rolled back for 'commonColl'.
let RollbackOps = (node, commonCollName) => {
    jsTestLog(`Running rollback ops`);
    const testDB = node.getDB(dbName);
    const commonColl = testDB[commonCollName];
    for (let i = 0; i < 2; i++) {
        assert.commandWorked(commonColl.update({}, {$set: {rollBackOps: i}}, {multi: true}));
    }
};

// Performs operations only on the sync source.
//
// Generates pre-images for 'commonColl' and 'collWithoutCommonPreImages' that will eventually be
// replayed onto the rolled back node.
const SyncSourceOps = (node, commonCollName, collWithoutCommonPreImagesName) => {
    jsTestLog(`Running sync source ops`);
    const testDB = node.getDB(dbName);

    const commonColl = testDB[commonCollName];
    for (let i = 0; i < 2; i++) {
        assert.commandWorked(commonColl.update({}, {$set: {syncSourceOps: i}}, {multi: true}));
    }

    const collWithoutCommonPreImages = testDB[collWithoutCommonPreImagesName];
    assert.commandWorked(collWithoutCommonPreImages.insert(baseDocs));
    assert.commandWorked(collWithoutCommonPreImages.update(
        {}, {$set: {collName: collWithoutCommonPreImagesName}}, {multi: true}));
};

const runTestCase = (expireAfterSeconds) => {
    jsTest.log(
        `Running rollback test case with 'expireAfterSeconds' ${tojson(expireAfterSeconds)}`);

    const rollbackTest = new RollbackTest(jsTestName(), undefined, nodeOptions);
    let primary = rollbackTest.getPrimary();
    const commonCollName = `commonCollExpirySecs-${expireAfterSeconds}`;
    const collWithoutCommonPreImagesName =
        `collWithoutCommonPreImagesExpirySecs-${expireAfterSeconds}`;
    CommonOps(primary, commonCollName, collWithoutCommonPreImagesName, expireAfterSeconds);

    const rollbackNode = rollbackTest.transitionToRollbackOperations();
    RollbackOps(rollbackNode, commonCollName);

    const syncSourceNode = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    SyncSourceOps(syncSourceNode, commonCollName, collWithoutCommonPreImagesName);

    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    const nodes = rollbackTest.getTestFixture().nodes;

    jsTest.log(`Pre-images for all nodes ${tojson(getPreImagesForAllNodes(nodes))}`);

    if (expireAfterSeconds != "off") {
        // All pre-images should eventually expire.
        //
        // When 'expireAfterSeconds' is off, rely on the data consistency check to validate that
        // the pre-images collection is consistent across nodes.
        nodes.forEach((node) => {
            assert.soon(
                () => {
                    return getPreImages(node).length == 0;
                },
                `Unexpected number of pre-images on node ${node.port}. Found pre-images ${
                    tojson(getPreImagesForAllNodes(nodes))}`);
        });
    }
    rollbackTest.stop({}, false /* skipDataConsistencyCheck */);
};

runTestCase("off");
// TODO SERVER-90705: Re-enable testing all pre-images are eventually truncated.
// runTestCase(12);
