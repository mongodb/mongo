/**
 *  @tags: [requires_fcv_63]
 *
 * Tests that when the max number of connections mongod will accept has been reached (i.e.
 * maxConns), serverStatus labels subsequent connection attempts as rejected connections.
 */

(function() {
'use strict';

// This value must be equivalent to the value specified in the config yaml file.
const configuredMaxConns = 5;

let conn = MongoRunner.runMongod({
    config: "jstests/noPassthrough/libs/net.max_incoming_connections.yaml",
});
let db = conn.getDB("test");

function getStats() {
    return assert.commandWorked(conn.getDB("admin").runCommand({serverStatus: 1}));
}

function verifyStats({expectedCurrentCount, expectedRejectedCount}) {
    // Verify that we have updated serverStatus.
    jsTestLog(`calling verifyStats with expectedCurrentCount: ${
        expectedCurrentCount}, expectedRejectedCount: ${expectedRejectedCount}`);
    let serverStatus = getStats();

    assert.soon(
        () => {
            const actualCurrentCount = serverStatus.connections.current;
            const actualRejectedCount = serverStatus.connections.rejected;

            jsTestLog(`expectedCurrentCount: ${expectedCurrentCount}, expectedRejectedCount: ${
                expectedRejectedCount}, actualCurrentCount: ${
                actualCurrentCount}, actualRejectedCount: ${actualRejectedCount}`);
            return expectedCurrentCount == actualCurrentCount &&
                expectedRejectedCount == actualRejectedCount;
        },
        "Failed to verify initial conditions. serverStatus.connections: " +
            tojson(serverStatus.connections),
        10000);

    assert.eq(serverStatus.connections["available"], configuredMaxConns - expectedCurrentCount);
}

let conns = [];
let expectedCurrentCount = 1;
let expectedRejectedCount = 0;

for (var i = 0; i < configuredMaxConns * 2; i++) {
    try {
        conns.push(new Mongo(db.getMongo().host));
        ++expectedCurrentCount;
    } catch (e) {
        jsTestLog(e);
        assert(i + 1 >= configuredMaxConns);
        ++expectedRejectedCount;
    }
    verifyStats(
        {expectedCurrentCount: expectedCurrentCount, expectedRejectedCount: expectedRejectedCount});
}

MongoRunner.stopMongod(conn);
})();
