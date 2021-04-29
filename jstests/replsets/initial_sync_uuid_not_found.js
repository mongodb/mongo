/**
 * Test dropping collections during initial sync, before using the 'find' and 'count'
 * commands using UUIDs instead of namespaces. This verifies initial sync behavior in
 * cases where using UUIDs results in NamespaceNotFound while using namespace strings
 * results in an empty result or zero count.
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const basename = 'initial_sync_rename_collection';

jsTestLog('Bring up set');
const rst = new ReplSetTest(
    {name: basename, nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB('d');
const primaryColl = primaryDB.coll;

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

jsTestLog('Create a collection (with a UUID) and insert a document.');
assert.commandWorked(primaryColl.insert({_id: 0}));

const collInfo = primaryDB.getCollectionInfos({name: primaryColl.getName()})[0];
assert(collInfo.info.uuid, 'newly created collection expected to have a UUID: ' + tojson(collInfo));

jsTestLog('Make sure synced');
rst.awaitReplication();

jsTestLog('Resync the secondary enabling failpoint');
function ResyncWithFailpoint(failpointName, failpointData) {
    let setParameter = {numInitialSyncAttempts: 1};
    setParameter['failpoint.' + failpointName] = tojson({mode: 'alwaysOn', data: failpointData});
    rst.restart(1, {startClean: true, setParameter});
    const secondary = rst.nodes[1];
    assert.eq(primary, rst.getPrimary(), 'Primary changed after reconfig');

    jsTestLog('Wait for new node to start cloning');
    secondary.setSecondaryOk();
    const secondaryDB = secondary.getDB(primaryDB.getName());
    const secondaryColl = secondaryDB[primaryColl.getName()];

    rst.reInitiate();
    assert.commandWorked(secondary.adminCommand({
        waitForFailPoint: failpointName,
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    jsTestLog('Remove collection on the primary and insert a new document, recreating it.');
    assert(primaryColl.drop());
    assert.commandWorked(primaryColl.insert({_id: 0}, {writeConcern: {w: 'majority'}}));
    const newCollInfo = primaryDB.getCollectionInfos({name: primaryColl.getName()})[0];
    assert(collInfo.info.uuid, 'recreated collection expected to have a UUID: ' + tojson(collInfo));
    assert.neq(collInfo.info.uuid,
               newCollInfo.info.uuid,
               'recreated collection expected to have different UUID');

    jsTestLog('Disable failpoint and resume initial sync');
    assert.commandWorked(secondary.adminCommand({configureFailPoint: failpointName, mode: 'off'}));

    jsTestLog('Wait for both nodes to be up-to-date');
    rst.awaitSecondaryNodes();
    rst.awaitReplication();

    jsTestLog('Check consistency and shut down replica-set');
    rst.checkReplicatedDataHashes();
}
ResyncWithFailpoint(
    'hangBeforeClonerStage',
    {cloner: 'CollectionCloner', stage: 'count', namespace: primaryColl.getFullName()});
ResyncWithFailpoint(
    'hangAfterClonerStage',
    {cloner: 'DatabaseCloner', stage: 'listCollections', database: primaryDB.getName()});
rst.stopSet();
})();
