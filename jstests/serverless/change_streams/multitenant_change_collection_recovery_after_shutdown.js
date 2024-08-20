// Tests the startup recovery behavior of change collection entries in a multitenant environment.
// Ensures the periodic change collection entry remover is disabled for the duration of the test to
// isolate startup recovery behavior.
// @tags: [
//  requires_fcv_72,
//  # Not suitable for inMemory variants given data must persist across shutdowns.
//  requires_persistence,
// ]

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    ChangeStreamMultitenantReplicaSetTest
} from "jstests/serverless/libs/change_collection_util.js";

const replSetTest = new ChangeStreamMultitenantReplicaSetTest({
    nodes: 2,
    // Disable the periodic remover.
    setParameter: {
        'failpoint.hangBeforeRemovingExpiredChanges': tojson({'mode': 'alwaysOn'}),
    }
});
let primary = replSetTest.getPrimary();
const primaryHost = primary.host;

// It's imperative node restart utilizes these parameters to prevent side effects from the
// periodic remover and ensure consistent server parameters.
const setParameters = ChangeStreamMultitenantReplicaSetTest.multitenancyParameters();
setParameters["failpoint.hangBeforeRemovingExpiredChanges"] = tojson({'mode': 'alwaysOn'});

// Hard code tenants ids such that a particular tenant can be identified deterministically.
const firstTenantId = ObjectId("6303b6bb84305d2266d0b779");
const secondTenantId = ObjectId("7303b6bb84305d2266d0b779");

const testDBName = 'test';
const stockPriceCollName = 'stockPrice';
const stockPriceNs = `${testDBName}.${stockPriceCollName}`;

const kExpireAfterSeconds = 1;

const establishTenant = function(tenantId, host = primaryHost) {
    // Explicitly pass the 'user' to getTenantConnection to avoid creating a user for each call.
    // This is specially important given that we call this function in standalone mode, which would
    // introduce inconsistencies in 'system.users'.
    return ChangeStreamMultitenantReplicaSetTest.getTenantConnection(host, tenantId, tenantId.str);
};

// Fetches the tenant's change collection entries tied to the 'stockPrice' collection.
const getChangeCollectionEntries = function(tenantConn) {
    const tenantChangeColl = tenantConn.getDB("config").system.change_collection;
    return tenantChangeColl.find({"ns": stockPriceNs}).toArray();
};

// Given the 'originalConn' of a previously shutdown node, restarts the node as the primary in the
// replica set and returns the new connection.
const restartNodeAsPrimary = function(originalConn) {
    const newPrimary =
        replSetTest.start(originalConn, {setParameter: setParameters, serverless: true}, true);
    assert.soonNoExcept(function() {
        const nodeState = assert.commandWorked(newPrimary.adminCommand("replSetGetStatus")).myState;
        return nodeState == ReplSetTest.State.SECONDARY;
    });
    newPrimary.setSecondaryOk();
    assert.soonNoExcept(() => {
        const res = newPrimary.adminCommand({replSetStepUp: 1});
        if (!res.ok) {
            jsTestLog(`Failed to step up with ${res}`);
        }
        return res.ok;
    }, "Failed to step up");
    jsTestLog(`Forced step up to ${newPrimary}`);
    assert.soon(() => newPrimary.adminCommand('hello').isWritablePrimary);
    return newPrimary;
};

const setupTenantChangeCollections = function(firstTenantConn, secondTenantConn) {
    const firstTenantTestDb = firstTenantConn.getDB(testDBName);
    const secondTenantTestDb = secondTenantConn.getDB(testDBName);

    assertDropAndRecreateCollection(firstTenantTestDb, stockPriceCollName, {});
    assertDropAndRecreateCollection(secondTenantTestDb, stockPriceCollName, {});

    // Reset the change collections to start as empty for each tenant.
    replSetTest.setChangeStreamState(firstTenantConn, false);
    replSetTest.setChangeStreamState(firstTenantConn, true);

    replSetTest.setChangeStreamState(secondTenantConn, false);
    replSetTest.setChangeStreamState(secondTenantConn, true);

    const firstTenantDocs =
        [{_id: "mdb", price: 350}, {_id: "goog", price: 2000}, {_id: "nflx", price: 220}];
    const secondTenantDocs =
        [{_id: "amzn", price: 3000}, {_id: "tsla", price: 750}, {_id: "aapl", price: 160}];

    const firstTenantStockPriceColl = firstTenantTestDb[stockPriceCollName];
    const secondTenantStockPriceColl = secondTenantTestDb[stockPriceCollName];

    assert.commandWorked(secondTenantStockPriceColl.insert(secondTenantDocs[0]));
    assert.commandWorked(firstTenantStockPriceColl.insert(firstTenantDocs[0]));
    assert.commandWorked(firstTenantStockPriceColl.insert(firstTenantDocs[1]));
    assert.commandWorked(secondTenantStockPriceColl.insert(secondTenantDocs[1]));
    assert.commandWorked(secondTenantStockPriceColl.insert(secondTenantDocs[2]));
    assert.commandWorked(firstTenantStockPriceColl.insert(firstTenantDocs[2]));

    replSetTest.awaitReplication();
};

// Tests change collection behavior upon startup recovery when there are 2 tenants. Startup recovery
// is expected to remove expired change collection entries after an unclean shutdown, and leave
// change collection entries as-is otherwise.
//
// 'testInStandalone': True forces the shutdown node to restart in standalone mode before re-joining
// the replica set.
const runChangeCollectionStartupRecoveryTest = function(uncleanShutdown, testInStandalone) {
    jsTestLog(`Running startup recovery test. uncleanShutdown=${
        uncleanShutdown}, testInStandalone=${testInStandalone}`);

    let firstTenantConn = establishTenant(firstTenantId);
    let secondTenantConn = establishTenant(secondTenantId);
    setupTenantChangeCollections(firstTenantConn, secondTenantConn);

    // Only the first tenant will have expired entries in the test.
    assert.commandWorked(firstTenantConn.getDB("admin").runCommand(
        {setClusterParameter: {changeStreams: {expireAfterSeconds: kExpireAfterSeconds}}}));
    replSetTest.awaitReplication();

    // Sleep for 1 second past 'kExpireAfterSeconds' to ensure entries are expired.
    sleep((kExpireAfterSeconds + 1) * 1000);

    // No change collection entries, albeit some expired, should be removed with the periodic
    // remover disabled.
    const firstTenantPreShutdownEntries = getChangeCollectionEntries(firstTenantConn);
    const secondTenantPreShutdownEntries = getChangeCollectionEntries(secondTenantConn);
    assert.gt(firstTenantPreShutdownEntries.length, 0);
    assert.gt(secondTenantPreShutdownEntries.length, 0);

    // Prepare for shutdown by forcing a checkpoint.
    assert.commandWorked(primary.adminCommand({fsync: 1}));
    if (uncleanShutdown) {
        jsTest.log(`Forcing an unclean shutdown of ${primaryHost}`);
        replSetTest.stop(
            primary, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
    } else {
        jsTest.log(`Forcing a clean shutdown of ${primaryHost}`);
        replSetTest.stop(primary);
    }

    // Only the first tenant has expired entries. Expired entries are removed by startup recovery
    // only after an unclean shutdown.
    const firstTenantEntriesExpected = uncleanShutdown ? [] : firstTenantPreShutdownEntries;
    const secondTenantEntriesExpected = secondTenantPreShutdownEntries;

    if (testInStandalone) {
        // Startup the node in standalone and confirm the expected change collection entries.
        const standaloneConn = MongoRunner.runMongod({
            setParameter: setParameters,
            dbpath: primary.dbpath,
            noReplSet: true,
            noCleanData: true
        });
        assert.neq(null, standaloneConn, "Fail to restart the node as standalone");

        firstTenantConn = establishTenant(firstTenantId, standaloneConn.host);
        secondTenantConn = establishTenant(secondTenantId, standaloneConn.host);

        jsTest.log(`Confirming change collection entries of node originally at ${
            primaryHost} as standalone post shutdown (uncleanShutdown = ${uncleanShutdown})`);

        const firstTenantEntriesInStandalone = getChangeCollectionEntries(firstTenantConn);
        const secondTenantEntriesInStandalone = getChangeCollectionEntries(secondTenantConn);
        assert.eq(firstTenantEntriesInStandalone, firstTenantEntriesExpected);
        assert.eq(secondTenantEntriesInStandalone, secondTenantEntriesExpected);

        MongoRunner.stopMongod(
            standaloneConn, null, {noCleanData: true, skipValidation: true, wait: true});
    }

    primary = restartNodeAsPrimary(primary);
    firstTenantConn = establishTenant(firstTenantId);
    secondTenantConn = establishTenant(secondTenantId);

    const firstTenantEntriesPostRestart = getChangeCollectionEntries(firstTenantConn);
    const secondTenantEntriesPostRestart = getChangeCollectionEntries(secondTenantConn);
    assert.eq(firstTenantEntriesPostRestart, firstTenantEntriesExpected);
    assert.eq(secondTenantEntriesPostRestart, secondTenantEntriesExpected);
};

runChangeCollectionStartupRecoveryTest(true /** uncleanShutdown **/,
                                       false /** testInStandalone **/);
runChangeCollectionStartupRecoveryTest(false /** uncleanShutdown **/,
                                       false /** testInStandalone **/);

runChangeCollectionStartupRecoveryTest(true /** uncleanShutdown **/, true /** testInStandalone **/);
runChangeCollectionStartupRecoveryTest(false /** uncleanShutdown **/,
                                       true /** testInStandalone **/);

replSetTest.stopSet();
