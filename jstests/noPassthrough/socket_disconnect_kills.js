// This test verifies that particular code paths exit early (I.e. are killed) or not by:
//
// 1. Set a fail point that will hang the code path
// 2. Open a new client with sockettimeoutms set (to force the disconnect) and a special appname
//    (to allow easy checking for the specific connection)
// 3. Run the tested command on the special connection and wait for it to timeout
// 4. Use an existing client to check current op for that special appname.  Return true if it's
//    still there at the end of a timeout
// 5. Disable the fail point

(function() {
    "use strict";

    const testName = "socket_disconnect_kills";

    // Used to generate unique appnames
    let id = 0;

    // client - A client connection for curop (and that holds the hostname)
    // pre - A callback to run with the timing out socket
    // post - A callback to run after everything else has resolved (cleanup)
    //
    // Returns false if the op was gone from current op
    function check(client, pre, post) {
        const interval = 200;
        const timeout = 2000;
        const socketTimeout = 1000;

        const host = client.host;

        // Make a socket which will timeout
        id++;
        let conn = new Mongo(
            `mongodb://${host}/?socketTimeoutMS=${socketTimeout}&appName=${testName}${id}`);

        // Make sure it works at all
        assert.commandWorked(conn.adminCommand({ping: 1}));

        try {
            // Make sure that whatever operation we ran had a network error
            assert.throws(function() {
                try {
                    pre(conn);
                } catch (e) {
                    throw e;
                }
            }, [], "error doing query: failed: network error while attempting");

            // Spin until the op leaves currentop, or timeout passes
            const start = new Date();

            while (1) {
                if (!client.getDB("admin")
                         .aggregate([
                             {$currentOp: {localOps: true}},
                             {$match: {appName: testName + id}},
                         ])
                         .itcount()) {
                    return false;
                }

                if (((new Date()).getTime() - start.getTime()) > timeout) {
                    return true;
                }

                sleep(interval);
            }
        } finally {
            post();
        }
    }

    function runWithCuropFailPointEnabled(client, failPointName) {
        return function(entry) {
            entry[0](client,
                     function(client) {
                         assert.commandWorked(client.adminCommand({
                             configureFailPoint: failPointName,
                             mode: "alwaysOn",
                             data: {shouldCheckForInterrupt: true},
                         }));

                         entry[1](client);
                     },
                     function() {
                         assert.commandWorked(
                             client.adminCommand({configureFailPoint: failPointName, mode: "off"}));
                     });
        };
    }

    function runWithCmdFailPointEnabled(client) {
        return function(entry) {
            const failPointName = "waitInCommandMarkKillOnClientDisconnect";

            entry[0](client,
                     function(client) {
                         assert.commandWorked(client.adminCommand({
                             configureFailPoint: failPointName,
                             mode: "alwaysOn",
                             data: {appName: testName + id},
                         }));

                         entry[1](client);
                     },
                     function() {
                         assert.commandWorked(
                             client.adminCommand({configureFailPoint: failPointName, mode: "off"}));
                     });
        };
    }

    function checkClosedEarly(client, pre, post) {
        assert(!check(client, pre, post), "operation killed on socket disconnect");
    }

    function checkNotClosedEarly(client, pre, post) {
        assert(check(client, pre, post), "operation not killed on socket disconnect");
    }

    function runCommand(cmd) {
        return function(client) {
            assert.commandWorked(client.getDB(testName).runCommand(cmd));
        };
    }

    function runTests(client) {
        let admin = client.getDB("admin");

        assert.writeOK(client.getDB(testName).test.insert({x: 1}));
        assert.writeOK(client.getDB(testName).test.insert({x: 2}));
        assert.writeOK(client.getDB(testName).test.insert({x: 3}));

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
         [
           checkClosedEarly,
           function(client) {
               client.forceReadMode("legacy");
               assert(client.getDB(testName).test.findOne({}));
           }
         ],
        ].forEach(runWithCuropFailPointEnabled(client, "waitInFindBeforeMakingBatch"));

        // After SERVER-39475, re-enable these tests and add negative testing for $out cursors.
        const serverSupportsEarlyDisconnectOnGetMore = false;
        if (serverSupportsEarlyDisconnectOnGetMore) {
            [[
               checkClosedEarly,
               function(client) {
                   let result = assert.commandWorked(
                       client.getDB(testName).runCommand({find: "test", filter: {}, batchSize: 0}));
                   assert.commandWorked(client.getDB(testName).runCommand(
                       {getMore: result.cursor.id, collection: "test"}));
               }
            ],
             [
               checkClosedEarly,
               function(client) {
                   client.forceReadMode("legacy");
                   var cursor = client.getDB(testName).test.find({}).batchSize(2);
                   assert(cursor.next());
                   assert(cursor.next());
                   assert(cursor.next());
               }
             ],
            ].forEach(runWithCuropFailPointEnabled(client,
                                                   "waitAfterPinningCursorBeforeGetMoreBatch"));
        }

        [[checkClosedEarly, runCommand({aggregate: "test", pipeline: [], cursor: {}})],
         [
           checkNotClosedEarly,
           runCommand({aggregate: "test", pipeline: [{$out: "out"}], cursor: {}})
         ],
        ].forEach(runWithCmdFailPointEnabled(client));

        [[checkClosedEarly, runCommand({count: "test"})],
         [checkClosedEarly, runCommand({distinct: "test", key: "x"})],
         [checkClosedEarly, runCommand({authenticate: "test", user: "x", pass: "y"})],
         [checkClosedEarly, runCommand({getnonce: 1})],
         [checkClosedEarly, runCommand({saslStart: 1})],
         [checkClosedEarly, runCommand({saslContinue: 1})],
         [checkClosedEarly, runCommand({ismaster: 1})],
         [checkClosedEarly, runCommand({listCollections: 1})],
         [checkClosedEarly, runCommand({listDatabases: 1})],
         [checkClosedEarly, runCommand({listIndexes: "test"})],
        ].forEach(runWithCmdFailPointEnabled(client));
    }

    {
        let proc = MongoRunner.runMongod();
        assert.neq(proc, null);
        runTests(proc);
        MongoRunner.stopMongod(proc);
    }

    {
        let st = ShardingTest({mongo: 1, config: 1, shards: 1});
        runTests(st.s0);
        st.stop();
    }
})();
