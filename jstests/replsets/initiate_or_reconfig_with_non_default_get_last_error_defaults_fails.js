/*
 * Test that initiating and reconfiguring when settings.getLastErrorDefaults is set
 * and not {w:1, wtimeout: 0} will fail.
 * @tags: [requires_fcv_50]
 */
(function() {
"use strict";

function testInitiate(gleDefaults) {
    const replTest = new ReplSetTest({name: jsTestName(), nodes: 1});
    clearRawMongoProgramOutput();
    const conns = replTest.startSet();
    const admin = conns[0].getDB("admin");

    const conf = replTest.getReplSetConfig();
    conf.settings = gleDefaults;
    assert.soon(
        function() {
            try {
                admin.runCommand({replSetInitiate: conf});
                return false;
            } catch (ex) {
                return true;
            }
        },
        "Node should fail when initiating with a non-default getLastErrorDefaults field",
        ReplSetTest.kDefaultTimeoutMS);

    assert.soon(
        function() {
            return rawMongoProgramOutput().search(/Fatal assertion.*5624101/) >= 0;
        },
        "Node should have fasserted when initiating with a non-default getLastErrorDefaults field",
        ReplSetTest.kDefaultTimeoutMS);

    replTest.stop(conns[0], undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
    replTest.stopSet(undefined, undefined, {skipValidation: true});
}

function testReconfig(gleDefaults) {
    const replTest = new ReplSetTest({name: jsTestName(), nodes: 1});
    const conns = replTest.startSet();
    const admin = conns[0].getDB("admin");

    replTest.initiate();
    const conf = admin.runCommand({replSetGetConfig: 1}).config;
    conf.settings = gleDefaults;
    conf.version++;

    jsTestLog("Node should fail to reconfig with a non-default getLastErrorDefaults field.");
    assert.commandFailedWithCode(admin.runCommand({replSetReconfig: conf}), 5624102);
    assert.commandFailedWithCode(admin.runCommand({replSetReconfig: conf, force: true}), 5624102);

    replTest.stopSet();
}

function runTest(gleDefaults) {
    testInitiate(gleDefaults);
    testReconfig(gleDefaults);
}

jsTestLog("Testing getLastErrorDefaults with {w: 'majority'}");
runTest({getLastErrorDefaults: {w: 'majority', wtimeout: 0}});

jsTestLog("Testing getLastErrorDefaults with {w:1, wtimeout: 1}");
runTest({getLastErrorDefaults: {w: 1, wtimeout: 1}});

jsTestLog("Testing getLastErrorDefaults with {w:1, wtimeout: 0, j: true}");
runTest({getLastErrorDefaults: {w: 1, wtimeout: 0, j: true}});

jsTestLog("Testing getLastErrorDefaults with {w:1, wtimeout: 0, fsync: true}");
runTest({getLastErrorDefaults: {w: 1, wtimeout: 0, fsync: true}});

jsTestLog("Testing getLastErrorDefaults with {w:1, wtimeout: 0, j: false}");
runTest({getLastErrorDefaults: {w: 1, wtimeout: 0, j: false}});
}());
