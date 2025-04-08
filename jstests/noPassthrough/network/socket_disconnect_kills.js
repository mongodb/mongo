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
// TODO: SERVER-95600 reenable this test with gRPC.
// @tags: [requires_sharding, requires_scripting, grpc_incompatible]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kTestName = jsTestName();

// Counter for genAppName().
let id = 0;

// Generate a unique appName.
function genAppName() {
    return kTestName + id++;
}

function getCurOpCount(client, appName) {
    return client.getDB("admin")
        .aggregate([
            {$currentOp: {localOps: true}},
            {$match: {appName: {$eq: appName}}},
        ])
        .itcount();
}

// client - A client connection for curop (and that holds the hostname)
// pre - A callback to run with the timing out socket
// post - A callback to run after everything else has resolved (cleanup)
// expectCleanUp - whether or not to expect the operation to be cleaned up on network failure.
//
// Returns false if the op was gone from current op
function check(client, pre, post, {appName, expectCleanUp, expectedNumKilled}) {
    const interval = 200;
    const timeout = 10000;
    const socketTimeout = 5000;

    const host = client.host;

    const msg = "operation " + (expectCleanUp ? "" : "not ") + "cleaned up on client disconnect.";

    // Make a socket which will timeout
    let conn = new Mongo(`mongodb://${host}/?socketTimeoutMS=${socketTimeout}&appName=${appName}`);

    // Make sure it works at all
    assert.commandWorked(conn.adminCommand({ping: 1}));

    // Measure 'killedDueToClientDisconnect' before and after.
    const getNumKilled = () => client.getDB("admin")
                                   .runCommand({serverStatus: 1})
                                   .metrics.operation.killedDueToClientDisconnect;
    const numKilledBefore = getNumKilled();

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
                return (getCurOpCount(client, appName) == 0);
            }, msg, timeout, interval);
        } else {
            sleep(timeout);
            assert.gt(getCurOpCount(client, appName), 0);
        }
    } finally {
        post();
    }

    const numKilledAfter = getNumKilled();
    const actualNumKilled = numKilledAfter - numKilledBefore;
    assert.eq(
        actualNumKilled,
        expectedNumKilled,
        `Expected to see ${
            expectedNumKilled} operation(s) recorded as killedDueToClientDisconnect, but got ${
            actualNumKilled}`);
}

function runCommandWithFailPointEnabled({
    failPointName,
    failPointData,
    client,
    appName,
    command,
    expectClosedEarly,
    expectedNumKilled
}) {
    check(client,
          function(client) {
              configureFailPoint(client, failPointName, failPointData);
              if (typeof command === 'function') {
                  command();
              } else {
                  assert.commandWorked(client.getDB(kTestName).runCommand(command));
              }
          },
          function() {
              configureFailPoint(client, failPointName, {}, "off");
          },
          {
              appName,
              expectCleanUp: expectClosedEarly,
              expectedNumKilled,
          });
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

    runCommandWithFailPointEnabled({
        failPointName: "waitInFindBeforeMakingBatch",
        failPointData: {shouldCheckForInterrupt: true},
        client,
        appName: genAppName(),
        expectClosedEarly: true,
        expectedNumKilled: 1,
        command: {find: "test", filter: {}},
    });
    runCommandWithFailPointEnabled({
        failPointName: "waitInFindBeforeMakingBatch",
        failPointData: {shouldCheckForInterrupt: true},
        client,
        appName: genAppName(),
        expectClosedEarly: true,
        expectedNumKilled: 1,
        command: {
            find: "test",
            filter: {
                $where: function() {
                    sleep(100000);
                }
            }
        },
    });
    runCommandWithFailPointEnabled({
        failPointName: "waitInFindBeforeMakingBatch",
        failPointData: {shouldCheckForInterrupt: true},
        client,
        appName: genAppName(),
        expectClosedEarly: true,
        expectedNumKilled: 1,
        command: {
            find: "test",
            filter: {
                $where: function() {
                    while (true) {
                    }
                }
            }
        }
    });

    // After SERVER-39475, re-enable these tests and add negative testing for $out cursors.
    const serverSupportsEarlyDisconnectOnGetMore = false;
    if (serverSupportsEarlyDisconnectOnGetMore) {
        runCommandWithFailPointEnabled({
            failPointName: "waitAfterPinningCursorBeforeGetMoreBatch",
            failPointData: {shouldCheckForInterrupt: true},
            client,
            appName: genAppName(),
            expectClosedEarly: true,
            expectedNumKilled: 1,
            command: (client) => {
                const testDB = client.getDB(kTestName);
                const result = assert.commandWorked(
                    testDB.runCommand({find: "test", filter: {}, batchSize: 0}));
                assert.commandWorked(
                    testDB.runCommand({getMore: result.cursor.id, collection: "test"}));
            },
        });
    }

    {
        const appName = genAppName();
        runCommandWithFailPointEnabled({
            failPointName: "waitInCommandMarkKillOnClientDisconnect",
            failPointData: {appName},
            client,
            appName,
            expectClosedEarly: true,
            expectedNumKilled: 1,
            command: {aggregate: "test", pipeline: [], cursor: {}},
        });
    }
    {
        const appName = genAppName();
        runCommandWithFailPointEnabled({
            failPointName: "waitInCommandMarkKillOnClientDisconnect",
            failPointData: {appName},
            client,
            appName,
            expectClosedEarly: false,
            expectedNumKilled: 0,
            command: {aggregate: "test", pipeline: [{$out: "out"}], cursor: {}},
        });
    }
    {
        const appName = genAppName();
        runCommandWithFailPointEnabled({
            failPointName: "waitInCommandMarkKillOnClientDisconnect",
            failPointData: {appName},
            client,
            appName,
            expectClosedEarly: true,
            expectedNumKilled: 1,
            command: {count: "test"},
        });
    }
    {
        const appName = genAppName();
        runCommandWithFailPointEnabled({
            failPointName: "waitInCommandMarkKillOnClientDisconnect",
            failPointData: {appName},
            client,
            appName,
            expectClosedEarly: true,
            expectedNumKilled: 1,
            command: {distinct: "test", key: "x"},
        });
    }
    {
        const appName = genAppName();
        runCommandWithFailPointEnabled({
            failPointName: "waitInCommandMarkKillOnClientDisconnect",
            failPointData: {appName},
            client,
            appName,
            expectClosedEarly: true,
            expectedNumKilled: 1,
            command: {hello: 1},
        });
    }
    {
        const appName = genAppName();
        runCommandWithFailPointEnabled({
            failPointName: "waitInCommandMarkKillOnClientDisconnect",
            failPointData: {appName},
            client,
            appName,
            expectClosedEarly: true,
            expectedNumKilled: 1,
            command: {listCollections: 1},
        });
    }
    {
        const appName = genAppName();
        runCommandWithFailPointEnabled({
            failPointName: "waitInCommandMarkKillOnClientDisconnect",
            failPointData: {appName},
            client,
            appName,
            expectClosedEarly: true,
            expectedNumKilled: 1,
            command: {listIndexes: "test"},
        });
    }

    testUnsendableCompletedResponsesCounted(client);
}

{
    let proc = MongoRunner.runMongod();
    assert.neq(proc, null);
    runTests(proc);
    MongoRunner.stopMongod(proc);
}

{
    let st = new ShardingTest({mongo: 1, config: 1, shards: 1});
    runTests(st.s0);
    st.stop();
}
