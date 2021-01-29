/**
 * Tests that 'ReplSetTest.awaitReplication()' works correctly with keyfile authentication.
 *
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
'use strict';

const rst = new ReplSetTest({
    nodes: 3,
    waitForKeys: false,
    keyFile: "jstests/libs/key1",
});
rst.startSet();
rst.initiateWithHighElectionTimeout();

jsTestLog("Running 'awaitReplication()' while not authenticated in the test");
rst.awaitReplication();

const primary = rst.getPrimary();
const testNS = 'test.a';
const testDoc = {
    _id: 1,
    a: 1,
    str: 'authed_insert'
};

primary.getDB('admin').createUser({user: 'root', pwd: 'root', roles: ['root']}, {w: 3});
primary.getDB("admin").auth("root", "root");
assert.commandWorked(primary.getDB("admin").runCommand({hello: 1}));
assert.commandWorked(primary.getCollection(testNS).insert(testDoc));

jsTestLog("Running 'awaitReplication()', we are now authenticated in the test");
rst.awaitReplication();

// Verify that we correctly waited for the insert to be replicated onto all secondaries.
for (const secondary of rst.getSecondaries()) {
    secondary.getDB("admin").auth("root", "root");
    const findRes = secondary.getCollection(testNS).find(testDoc).toArray();
    assert.eq(1, findRes.length, `result of find on secondary: ${tojson(findRes)}`);
    assert.eq(testDoc, findRes[0], `result of find on secondary: ${tojson(findRes)}`);
}

rst.stopSet();
})();
