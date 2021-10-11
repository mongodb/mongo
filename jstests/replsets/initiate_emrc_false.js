/*
 * Test that there is no side effects of failing replSetInitiate with eMRC=false, SERVER-58780.
 * @tags: [requires_persistence, multiversion_incompatible]
 */

(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");  // getOldestRequiredTimestampForCrashRecovery()

function runTest({cleanShutdown}) {
    const rst = new ReplSetTest({
        name: jsTestName(),
        nodes: 2,
        nodeOptions: {
            enableMajorityReadConcern: "false",
        }
    });
    const nodes = rst.startSet();

    // Run replSetInitiate with an invalid config and expect it to fail.
    jsTestLog("Running invalid replSetInitiate");
    assert.commandFailedWithCode(nodes[0].adminCommand({replSetInitiate: {foo: "bar"}}),
                                 ErrorCodes.InvalidReplicaSetConfig);
    assert.commandFailedWithCode(nodes[1].adminCommand({replSetInitiate: {foo: "bar"}}),
                                 ErrorCodes.InvalidReplicaSetConfig);

    rst.initiateWithHighElectionTimeout();

    // Do some dummy writes.
    const primary = rst.getPrimary();
    const testDB = primary.getDB("test");
    assert.commandWorked(testDB.runCommand({
        insert: "coll",
        documents: [...Array(100).keys()].map(x => ({_id: x})),
        writeConcern: {w: "majority"}
    }));
    rst.awaitSecondaryNodes();

    const secondary = rst.getSecondary();

    // Wait until the secondary has an initial checkpoint.
    assert.soon(() => {
        const oldestRequiredTimestampForCrashRecovery =
            PrepareHelpers.getOldestRequiredTimestampForCrashRecovery(secondary.getDB("admin"));
        jsTestLog("Secondary oldestRequiredTimestampForCrashRecovery: " +
                  tojson(oldestRequiredTimestampForCrashRecovery));
        return oldestRequiredTimestampForCrashRecovery &&
            timestampCmp(oldestRequiredTimestampForCrashRecovery, Timestamp(0, 0)) > 0;
    }, "Timeout waiting for the initial checkpoint", ReplSetTest.kDefaultTimeoutMS, 1000);

    if (cleanShutdown) {
        jsTestLog("Restarting secondary node from clean shutdown");
        rst.stop(secondary, 0, undefined, {forRestart: true});
    } else {
        jsTestLog("Restarting secondary node from unclean shutdown");
        rst.stop(secondary, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
    }

    // Restarting from shutdown shouldn't need to go through initial sync again. Set a failpoint to
    // hang initial sync so that if the node decides to do initial sync again, the test will fail.
    rst.start(secondary,
              {
                  setParameter: {
                      'failpoint.initialSyncHangAfterGettingBeginFetchingTimestamp':
                          tojson({mode: 'alwaysOn'}),
                  }
              },
              true /* restart */);

    rst.awaitSecondaryNodes();

    rst.stopSet();
}

runTest({cleanShutdown: true});
runTest({cleanShutdown: false});
})();
