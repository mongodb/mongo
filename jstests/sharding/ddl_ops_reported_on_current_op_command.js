/**
 * Checks that DDL command that use step-down resilient coordinators are shown when calling the
 * currentOp command.
 *
 * @tags: [
 *   requires_fcv_50,
 *   featureFlagShardingFullDDLSupport
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');

const kDbName = 'db';
const kCollectionName = 'test';
const nss = kDbName + '.' + kCollectionName;
const toNss = nss + '_renamed';

let st = new ShardingTest({shards: 1});

st.s.adminCommand({enableSharding: kDbName});

st.s.getDB(kDbName).getCollection(kCollectionName).insert({x: 1});

let getCurrentOpOfDDL = (ddlOpThread, desc) => {
    let ddlCoordinatorFailPoint =
        configureFailPoint(st.rs0.getPrimary(), 'hangBeforeRunningCoordinatorInstance');

    ddlOpThread.start();
    ddlCoordinatorFailPoint.wait();
    let currOp = st.s.getDB('admin')
                     .aggregate([{$currentOp: {allUsers: true}}, {$match: {desc: desc}}])
                     .toArray();

    ddlCoordinatorFailPoint.off();
    ddlOpThread.join();

    return currOp;
};

{
    jsTestLog('Check create collection shows in current op');

    let shardKey = {_id: 1};

    let ddlOpThread = new Thread((mongosConnString, nss, shardKey) => {
        let mongos = new Mongo(mongosConnString);
        mongos.adminCommand({shardCollection: nss, key: shardKey});
    }, st.s0.host, nss, shardKey);

    let currOp = getCurrentOpOfDDL(ddlOpThread, 'CreateCollectionCoordinator');

    // There must be one operation running with the appropiate ns.
    assert.eq(1, currOp.length);
    assert.eq(nss, currOp[0].ns);
    // It must have at least the shardKey.
    assert(currOp[0].hasOwnProperty('command'));
    assert(currOp[0].command.hasOwnProperty('shardKey'));
    assert.eq(shardKey, currOp[0].command.shardKey);
}

{
    jsTestLog('Check set allow migrations shows in current op');

    let ddlOpThread = new Thread((mongosConnString, nss) => {
        let mongos = new Mongo(mongosConnString);
        mongos.adminCommand({setAllowMigrations: nss, allowMigrations: true});
    }, st.s0.host, nss);

    let currOp = getCurrentOpOfDDL(ddlOpThread, 'SetAllowMigrationsCoordinator');

    // There must be one operation running with the appropiate ns.
    assert.eq(1, currOp.length);
    assert.eq(nss, currOp[0].ns);
    assert(currOp[0].hasOwnProperty('command'));
    assert(currOp[0].command.hasOwnProperty('allowMigrations'));
    assert.eq(true, currOp[0].command.allowMigrations);
}

{
    jsTestLog('Check rename collection shows in current op');

    let ddlOpThread = new Thread((mongosConnString, fromNss, toNss) => {
        let mongos = new Mongo(mongosConnString);
        mongos.getCollection(fromNss).renameCollection(toNss.split('.')[1], true);
    }, st.s0.host, nss, toNss);

    let currOp = getCurrentOpOfDDL(ddlOpThread, 'RenameCollectionCoordinator');

    // There must be one operation running with the appropiate ns.
    assert.eq(1, currOp.length);
    assert.eq(nss, currOp[0].ns);

    // It must have the target collection.
    assert(currOp[0].hasOwnProperty('command'));
    assert.docEq({to: toNss, dropTarget: true, stayTemp: false}, currOp[0].command);
}

{
    jsTestLog('Check drop collection shows in current op');

    let ddlOpThread = new Thread((mongosConnString, nss) => {
        let mongos = new Mongo(mongosConnString);
        mongos.getCollection(nss).drop();
    }, st.s0.host, toNss);

    let currOp = getCurrentOpOfDDL(ddlOpThread, 'DropCollectionCoordinator');

    // There must be one operation running with the appropiate ns.
    assert.eq(1, currOp.length);
    assert.eq(toNss, currOp[0].ns);
}

{
    jsTestLog('Check drop database shows in current op');

    let ddlOpThread = new Thread((mongosConnString, dbName) => {
        let mongos = new Mongo(mongosConnString);
        mongos.getDB(dbName).dropDatabase();
    }, st.s0.host, kDbName);

    let currOp = getCurrentOpOfDDL(ddlOpThread, 'DropDatabaseCoordinator');

    // There must be one operation running with the appropiate ns.
    assert.eq(1, currOp.length);
    assert.eq(kDbName, currOp[0].ns.split('.')[0]);
}

st.stop();
})();