/**
 * Tests the "requireApiVersion" mongod/mongos parameter.
 *
 * This test is incompatible with parallel and passthrough suites; concurrent jobs fail while
 * requireApiVersion is true.
 *
 * @tags: [requires_journaling]
 */

(function() {
"use strict";

function runTest(db) {
    assert.commandWorked(db.runCommand({setParameter: 1, requireApiVersion: true}));
    assert.commandFailedWithCode(db.runCommand({ping: 1}), 498870, "command without apiVersion");
    assert.commandWorked(db.runCommand({ping: 1, apiVersion: "1"}));
    assert.commandFailed(db.runCommand({ping: 1, apiVersion: "not a real API version"}));
    assert.commandWorked(
        db.runCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"}));
    assert.commandWorked(db.runCommand({ping: 1}));
}

const mongod = MongoRunner.runMongod();
runTest(mongod.getDB("admin"));
MongoRunner.stopMongod(mongod);

const st = new ShardingTest({});
runTest(st.s0.getDB("admin"));
st.stop();
}());
