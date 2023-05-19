/**
 * Ensure that IPs cannot be used as hostnames for split horizon configurations.
 */

(function() {
"use strict";

// Tests that when replSetInitiate/Reconfig are called with one member with the given host name and
// horizon name, with the given mongod startup options, we fail the split horizon IP check if
// expectedReject.
function testConfig(hostName, horizonName, expectedReject, options = {}) {
    const mongod = MongoRunner.runMongod(Object.assign({replSet: "test"}, options));

    const config = {
        _id: "test",
        members: [{_id: 0, host: hostName, horizons: {"horizon_name": horizonName}}]
    };

    // Make sure replSetInitiate fails with correct error
    let output = mongod.adminCommand({replSetInitiate: config});
    jsTestLog("Command result: " + tojson(output));
    assert.eq(output.ok, 0);
    assert.eq(output.errmsg.includes("Found split horizon configuration using IP"), expectedReject);

    // Correctly start up a replset with the mongod as its node, and wait until it becomes primary
    assert.commandWorked(mongod.adminCommand(
        {replSetInitiate: {_id: "test", members: [{_id: 0, host: "localhost:" + mongod.port}]}}));
    assert.soon(() => {
        return assert.commandWorked(mongod.adminCommand({hello: 1})).isWritablePrimary;
    });

    // Make sure replSetReconfig fails with correct error
    config.version = 2;
    output = mongod.adminCommand({"replSetReconfig": config});
    jsTestLog("Command result: " + tojson(output));
    assert.eq(output.ok, 0);
    assert.eq(output.errmsg.includes("Found split horizon configuration using IP"), expectedReject);

    MongoRunner.stopMongod(mongod);
}

testConfig("a", "b", false);

// Hostname being an IP is fine
testConfig("12.34.56.78", "a", false);

// Any valid CIDR will fail the check
testConfig("a", "12.34.56.78", true);
testConfig("a", "12.34.56.78/20", true);

// Make sure setting this parameter disables the check
testConfig("a", "12.34.56.78", false, {setParameter: {disableSplitHorizonIPCheck: true}});
testConfig("a", "12.34.56.78/20", false, {setParameter: {disableSplitHorizonIPCheck: true}});
}());
