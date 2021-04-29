/**
 * Tests that setting the feature compatibility version during initial sync leads to initial sync
 * restarting. This test also ensures that even if initial sync takes two attempts to complete,
 * that the fCV is reset between attempts.
 *
 * This tests behavior centered around both upgrading and downgrading FCV.
 * @tags: [multiversion_incompatible]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: 2});
rst.startSet();

// We disallow the secondary node from voting so that the primary's featureCompatibilityVersion
// can be modified while the secondary node is still waiting to complete its initial sync.
const replSetConfig = rst.getReplSetConfig();
replSetConfig.members[1].priority = 0;
replSetConfig.members[1].votes = 0;
rst.initiate(replSetConfig);

const primary = rst.getPrimary();
const dbName = 'foo';
const collName = 'bar';
// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

assert.commandWorked(primary.getDB(dbName).getCollection(collName).insert({a: 1}));

function runInitialSync(cmd, initialFCV) {
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: initialFCV}));

    jsTestLog('Testing setting fCV with ' + tojson(cmd));

    const failPointOptions = tojson({
        mode: 'alwaysOn',
        data: {cloner: "DatabaseCloner", stage: "listCollections", database: dbName}
    });
    rst.restart(1, {
        startClean: true,
        setParameter: {
            'failpoint.hangBeforeClonerStage': failPointOptions,
            'failpoint.skipClearInitialSyncState': tojson({mode: 'alwaysOn'}),
            numInitialSyncAttempts: 2
        }
    });
    const secondary = rst.nodes[1];

    // Initial sync clones the 'admin' database first, which will set the fCV on the
    // secondary to initialFCV. We then block the secondary before issuing 'listCollections' on
    // the test database.
    assert.commandWorked(secondary.adminCommand({
        waitForFailPoint: "hangBeforeClonerStage",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    // Initial sync is stopped right before 'listCollections' on the test database. We now run
    // the test command to modify the fCV.
    assert.commandWorked(primary.adminCommand(cmd));

    // Let initial sync finish, making sure that it fails due to the feature compatibility
    // version change.
    assert.commandWorked(
        secondary.adminCommand({configureFailPoint: 'hangBeforeClonerStage', mode: 'off'}));
    checkLog.contains(secondary, 'Applying operation on feature compatibility version document');

    jsTestLog('Wait for both nodes to be up-to-date');
    rst.awaitSecondaryNodes();
    rst.awaitReplication();

    let res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
    assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 1);

    // We check oplogs and data hashes before we restart the second node.
    rst.checkOplogs();
    rst.checkReplicatedDataHashes();
}

// Ensure that attempting to downgrade the featureCompatibilityVersion during initial sync
// fails.
runInitialSync({setFeatureCompatibilityVersion: lastLTSFCV}, /*initialFCV*/ latestFCV);

// Ensure that attempting to upgrade the featureCompatibilityVersion during initial sync fails.
runInitialSync({setFeatureCompatibilityVersion: latestFCV}, /*initialFCV*/ lastLTSFCV);

// Modifications to the featureCompatibilityVersion document during initial sync should be
// caught and cause initial sync to fail.
runInitialSync({
    update: 'system.version',
    updates: [{q: {_id: 'featureCompatibilityVersion'}, u: {'version': lastLTSFCV}}]
},
               /*initialFCV*/ latestFCV);

rst.stopSet();
})();
