// This test verifies that particular code paths exit early (I.e. are killed) or not by:
//
// 1. Set a fail point that will hang the code path
// 2. Open a new client with sockettimeoutms set (to force the disconnect) and a special appname
//    (to allow easy checking for the specific connection)
// 3. Run the tested command on the special connection and wait for it to timeout
// 4. Use an existing client to check current op for that special appname and check if it's
//    still there at the end of a timeout
// 5. Disable the fail point
//
// It also ensures that we correctly record metrics counting the number of operations killed
// due to client disconnect, and the number of completed operations that couldn't return data
// due to client disconnect.
//
// @tags: [requires_sharding, requires_scripting]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kTestName = jsTestName();

// Used to generate unique appnames
let id = 0;

function getCurOpCount(client, id) {
    return client.getDB("admin")
        .aggregate([
            {$currentOp: {localOps: true}},
            {$match: {appName: kTestName + id}},
        ])
        .itcount();
}

// client - A client connection for curop (and that holds the hostname)
// pre - A callback to run with the timing out socket
// post - A callback to run after everything else has resolved (cleanup)
// expectCleanUp - whether or not to expect the operation to be cleaned up on network failure.
//
// Returns false if the op was gone from current op
function check(client, pre, post, {expectCleanUp}) {
    const interval = 200;
    const timeout = 10000;
    const socketTimeout = 5000;

    const host = client.host;

    const msg = "operation " + (expectCleanUp ? "" : "not ") + "cleaned up on client disconnect.";

    // Make a socket which will timeout
    id++;
    let conn =
        new Mongo(`mongodb://${host}/?socketTimeoutMS=${socketTimeout}&appName=${kTestName}${id}`);

    // Make sure it works at all
    assert.commandWorked(conn.adminCommand({ping: 1}));

    try {
        // Make sure that whatever operation we ran had a network error
        assert.throws(function() {
            try {
                pre(conn);
            } catch (e) {
                if (isNetworkError(e)) {
                    throw e;
                }
            }
        }, [], "error doing query: failed: network error while attempting");

        if (expectCleanUp) {
            assert.soon(() => {
                return (getCurOpCount(client, id) == 0);
            }, msg, timeout, interval);
        } else {
            sleep(timeout);
            assert.gt(getCurOpCount(client, id), 0);
        }
    } finally {
        post();
    }
}

function runWithCuropFailPointEnabled(client, failPointName) {
    return function(entry) {
        entry[0](client,
                 function(client) {
                     configureFailPoint(client, failPointName, {shouldCheckForInterrupt: true});
                     entry[1](client);
                 },
                 function() {
                     configureFailPoint(client, failPointName, {}, "off");
                 });
    };
}

function runWithCmdFailPointEnabled(client) {
    return function(entry) {
        const failPointName = "waitInCommandMarkKillOnClientDisconnect";

        entry[0](client,
                 function(client) {
                     configureFailPoint(client, failPointName, {appName: kTestName + id});
                     entry[1](client);
                 },
                 function() {
                     configureFailPoint(client, failPointName, {}, "off");
                 });
    };
}

function checkClosedEarly(client, pre, post) {
    check(client, pre, post, {expectCleanUp: true});
}

function checkNotClosedEarly(client, pre, post) {
    check(client, pre, post, {expectCleanUp: false});
}

function runCommand(cmd) {
    return function(client) {
        assert.commandWorked(client.getDB(kTestName).runCommand(cmd));
    };
}

// Test that an operation that completes but whose client disconnects before the
// response is sent is recorded properly.
function testUnsendableCompletedResponsesCounted(client) {
    let beforeUnsendableResponsesCount =
        client.adminCommand({serverStatus: 1}).metrics.operation.unsendableCompletedResponses;
    id++;
    const host = client.host;
    let conn = new Mongo(`mongodb://${host}/?appName=${kTestName}${id}`);
    // Make sure the new connection works.
    assert.commandWorked(conn.adminCommand({ping: 1}));

    // Ensure that after the next command completes, the server fails to send the response.
    let fp = configureFailPoint(
        client, "sessionWorkflowDelayOrFailSendMessage", {appName: kTestName + id});

    // Should fail because the server will close the connection after receiving a simulated network
    // error sinking the response to the client.
    const error = assert.throws(() => conn.adminCommand({hello: 1}));
    assert(isNetworkError(error), error);

    // Ensure we counted the unsendable completed response.
    let afterUnsendableResponsesCount =
        client.adminCommand({serverStatus: 1}).metrics.operation.unsendableCompletedResponses;

    assert.eq(beforeUnsendableResponsesCount + 1, afterUnsendableResponsesCount);
}

function runTests(client) {
    let admin = client.getDB("admin");

    // set timeout for js function execution to 100 ms to speed up tests that run inf loop.
    assert.commandWorked(client.getDB(kTestName).adminCommand(
        {setParameter: 1, internalQueryJavaScriptFnTimeoutMillis: 100}));
    assert.commandWorked(client.getDB(kTestName).test.insert({x: 1}));
    assert.commandWorked(client.getDB(kTestName).test.insert({x: 2}));
    assert.commandWorked(client.getDB(kTestName).test.insert({x: 3}));

    [[checkClosedEarly, runCommand({find: "test", filter: {}})],
     [
         checkClosedEarly,
         runCommand({
             find: "test",
             filter: {
                 $where: function() {
                     sleep(100000);
                 }
             }
         })
     ],
     [
         checkClosedEarly,
         runCommand({
             find: "test",
             filter: {
                 $where: function() {
                     while (true) {
                     }
                 }
             }
         })
     ],
    ].forEach(runWithCuropFailPointEnabled(client, "waitInFindBeforeMakingBatch"));

    // After SERVER-39475, re-enable these tests and add negative testing for $out cursors.
    const serverSupportsEarlyDisconnectOnGetMore = false;
    if (serverSupportsEarlyDisconnectOnGetMore) {
        [[
            checkClosedEarly,
            function(client) {
                let result = assert.commandWorked(
                    client.getDB(kTestName).runCommand({find: "test", filter: {}, batchSize: 0}));
                assert.commandWorked(client.getDB(kTestName).runCommand(
                    {getMore: result.cursor.id, collection: "test"}));
            }
        ]].forEach(runWithCuropFailPointEnabled(client,
                                                "waitAfterPinningCursorBeforeGetMoreBatch"));
    }

    [[checkClosedEarly, runCommand({aggregate: "test", pipeline: [], cursor: {}})],
     [checkNotClosedEarly, runCommand({aggregate: "test", pipeline: [{$out: "out"}], cursor: {}})],
    ].forEach(runWithCmdFailPointEnabled(client));

    [[checkClosedEarly, runCommand({count: "test"})],
     [checkClosedEarly, runCommand({distinct: "test", key: "x"})],
     [checkClosedEarly, runCommand({hello: 1})],
     [checkClosedEarly, runCommand({listCollections: 1})],
     [checkClosedEarly, runCommand({listIndexes: "test"})],
    ].forEach(runWithCmdFailPointEnabled(client));

    testUnsendableCompletedResponsesCounted(client);
}

// Just counts the # of commands we send in an exection of runTests that should be
// killed due to disconnected client.
const numThatShouldCloseEarly = 9;

{
    let proc = MongoRunner.runMongod();
    let admin = proc.getDB("admin");
    let beforeServerStatusMetrics = admin.runCommand({serverStatus: 1}).metrics;
    assert.neq(proc, null);
    runTests(proc);
    let afterServerStatusMetrics = admin.runCommand({serverStatus: 1}).metrics;
    // Ensure that we report the operations killed due to client disconnect in serverStatus.
    assert.eq(
        beforeServerStatusMetrics.operation.killedDueToClientDisconnect + numThatShouldCloseEarly,
        afterServerStatusMetrics.operation.killedDueToClientDisconnect);
    MongoRunner.stopMongod(proc);
}

{
    let st = new ShardingTest({mongo: 1, config: 1, shards: 1});
    let admin = st.s0.getDB("admin");
    let beforeServerStatusMetrics = admin.runCommand({serverStatus: 1}).metrics;
    runTests(st.s0);
    let afterServerStatusMetrics = admin.runCommand({serverStatus: 1}).metrics;
    // Ensure that we report the operations killed due to client disconnect in serverStatus on
    // mongos.
    assert.eq(
        beforeServerStatusMetrics.operation.killedDueToClientDisconnect + numThatShouldCloseEarly,
        afterServerStatusMetrics.operation.killedDueToClientDisconnect);
    st.stop();
}
