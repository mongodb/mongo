/**
 * Tests that the $currentOp aggregation stage behaves as expected. Specifically:
 * - It must be the first stage in the pipeline.
 * - It can only be run on admin, and the "aggregate" field must be 1.
 * - Only active connections are shown unless {idleConnections: true} is specified.
 * - A user without the inprog privilege can see their own ops, but no-one else's.
 * - A user with the inprog privilege can see all ops.
 * - Non-local readConcerns are rejected.
 * - Collation rules are respected.
 *
 * Also verifies that the aggregation-backed currentOp command obeys the same rules, where
 * applicable.
 *
 * This test requires replica set configuration and user credentials to persist across a restart.
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    const key = "jstests/libs/key1";

    // Create a new sharded cluster for testing. We set the internalQueryExecYieldIterations
    // parameter so that plan execution yields on every iteration. For some tests, we will
    // temporarily set yields to hang the mongod so we can capture particular operations in the
    // currentOp output.
    const st = new ShardingTest({
        name: jsTestName(),
        keyFile: key,
        shards: 3,
        rs: {
            nodes: [
                {rsConfig: {priority: 1}},
                {rsConfig: {priority: 0}},
                {rsConfig: {arbiterOnly: true}}
            ],
            setParameter: {internalQueryExecYieldIterations: 1}
        }
    });

    // Assign various elements of the cluster. We will use shard rs0 to test replica-set level
    // $currentOp behaviour.
    let shardConn = st.rs0.getPrimary();
    const mongosConn = st.s;
    const shardRS = st.rs0;

    const clusterTestDB = mongosConn.getDB(jsTestName());
    const clusterAdminDB = mongosConn.getDB("admin");
    let shardAdminDB = shardConn.getDB("admin");

    function createUsers(conn) {
        let adminDB = conn.getDB("admin");

        // Create an admin user, one user with the inprog privilege, and one without.
        assert.commandWorked(
            adminDB.runCommand({createUser: "admin", pwd: "pwd", roles: ["root"]}));
        assert(adminDB.auth("admin", "pwd"));

        assert.commandWorked(adminDB.runCommand({
            createRole: "role_inprog",
            roles: [],
            privileges: [{resource: {cluster: true}, actions: ["inprog"]}]
        }));

        assert.commandWorked(
            adminDB.runCommand({createUser: "user_inprog", pwd: "pwd", roles: ["role_inprog"]}));

        assert.commandWorked(adminDB.runCommand(
            {createUser: "user_no_inprog", pwd: "pwd", roles: ["readAnyDatabase"]}));
    }

    // Create necessary users at both cluster and shard-local level.
    createUsers(shardConn);
    createUsers(mongosConn);

    // Create a test database and some dummy data on rs0.
    assert(clusterAdminDB.auth("admin", "pwd"));

    for (let i = 0; i < 5; i++) {
        assert.writeOK(clusterTestDB.test.insert({_id: i, a: i}));
    }

    st.ensurePrimaryShard(clusterTestDB.getName(), shardRS.name);

    // Restarts a replica set with additional parameters, and optionally re-authenticates.
    function restartReplSet(replSet, newOpts, user, pwd) {
        const numNodes = replSet.nodeList().length;

        for (let n = 0; n < numNodes; n++) {
            replSet.restart(n, newOpts);
        }

        shardConn = replSet.getPrimary();
        replSet.awaitSecondaryNodes();

        shardAdminDB = shardConn.getDB("admin");

        if (user && pwd) {
            shardAdminDB.auth(user, pwd);
        }
    }

    // Functions to support running an operation in a parallel shell for testing allUsers behaviour.
    function runInParallelShell({conn, testfunc, username, password}) {
        TestData.aggCurOpTest = testfunc;
        TestData.aggCurOpUser = username;
        TestData.aggCurOpPwd = password;

        assert.commandWorked(conn.getDB("admin").runCommand(
            {configureFailPoint: "setYieldAllLocksHang", mode: "alwaysOn"}));

        testfunc = function() {
            db.getSiblingDB("admin").auth(TestData.aggCurOpUser, TestData.aggCurOpPwd);
            TestData.aggCurOpTest();
            db.getSiblingDB("admin").logout();
        };

        return startParallelShell(testfunc, conn.port);
    }

    function assertCurrentOpHasSingleMatchingEntry({conn, currentOpAggFilter, curOpOpts}) {
        curOpOpts = (curOpOpts || {allUsers: true});

        const connAdminDB = conn.getDB("admin");

        let curOpResult;

        assert.soon(
            function() {
                curOpResult =
                    connAdminDB.aggregate([{$currentOp: curOpOpts}, {$match: currentOpAggFilter}])
                        .toArray();

                return curOpResult.length === 1;
            },
            function() {
                return "Failed to find operation " + tojson(currentOpAggFilter) +
                    " in $currentOp output: " + tojson(curOpResult);
            });

        return curOpResult[0];
    }

    function waitForParallelShell(conn, awaitShell) {
        assert.commandWorked(conn.getDB("admin").runCommand(
            {configureFailPoint: "setYieldAllLocksHang", mode: "off"}));

        awaitShell();
    }

    function getCollectionNameFromFullNamespace(ns) {
        return ns.split(/\.(.+)/)[1];
    }

    // Generic function for running getMore on a $currentOp aggregation cursor and returning the
    // command response.
    function getMoreTest({conn, showAllUsers, getMoreBatchSize}) {
        // Ensure that there are some other connections present so that the result set is larger
        // than 1 $currentOp entry.
        const otherConns = [new Mongo(conn.host), new Mongo(conn.host)];

        // Log the other connections in as user_no_inprog so that they will show up for user_inprog
        // with {allUsers: true} and user_no_inprog with {allUsers: false}.
        for (let otherConn of otherConns) {
            assert(otherConn.getDB("admin").auth("user_no_inprog", "pwd"));
        }

        const connAdminDB = conn.getDB("admin");

        const aggCmdRes = assert.commandWorked(connAdminDB.runCommand({
            aggregate: 1,
            pipeline: [{$currentOp: {allUsers: showAllUsers, idleConnections: true}}],
            cursor: {batchSize: 0}
        }));
        assert.neq(aggCmdRes.cursor.id, 0);

        return connAdminDB.runCommand({
            getMore: aggCmdRes.cursor.id,
            collection: getCollectionNameFromFullNamespace(aggCmdRes.cursor.ns),
            batchSize: (getMoreBatchSize || 100)
        });
    }

    // Runs a suite of tests for behaviour common to both the replica set and cluster levels.
    function runCommonTests(conn) {
        const testDB = conn.getDB(jsTestName());
        const adminDB = conn.getDB("admin");

        const isMongos = (conn == mongosConn);

        // Test that an unauthenticated connection cannot run $currentOp even with {allUsers:
        // false}.
        assert(adminDB.logout());

        assert.commandFailedWithCode(
            adminDB.runCommand(
                {aggregate: 1, pipeline: [{$currentOp: {allUsers: false}}], cursor: {}}),
            ErrorCodes.Unauthorized);

        // Test that an unauthenticated connection cannot run the currentOp command even with
        // {$ownOps: true}.
        assert.commandFailedWithCode(adminDB.currentOp({$ownOps: true}), ErrorCodes.Unauthorized);

        //
        // Authenticate as user_no_inprog.
        //
        assert(adminDB.logout());
        assert(adminDB.auth("user_no_inprog", "pwd"));

        // Test that $currentOp fails with {allUsers: true} for a user without the "inprog"
        // privilege.
        assert.commandFailedWithCode(
            adminDB.runCommand(
                {aggregate: 1, pipeline: [{$currentOp: {allUsers: true}}], cursor: {}}),
            ErrorCodes.Unauthorized);

        // Test that the currentOp command fails with {ownOps: false} for a user without the
        // "inprog" privilege.
        assert.commandFailedWithCode(adminDB.currentOp({$ownOps: false}), ErrorCodes.Unauthorized);

        // Test that {aggregate: 1} fails when the first stage in the pipeline is not $currentOp.
        assert.commandFailedWithCode(
            adminDB.runCommand({aggregate: 1, pipeline: [{$match: {}}], cursor: {}}),
            ErrorCodes.InvalidNamespace);

        //
        // Authenticate as user_inprog.
        //
        assert(adminDB.logout());
        assert(adminDB.auth("user_inprog", "pwd"));

        // Test that $currentOp fails when it is not the first stage in the pipeline. We use two
        // $currentOp stages since any other stage in the initial position will trip the {aggregate:
        // 1} namespace check.
        assert.commandFailedWithCode(
            adminDB.runCommand(
                {aggregate: 1, pipeline: [{$currentOp: {}}, {$currentOp: {}}], cursor: {}}),
            40602);

        // Test that $currentOp fails when run on admin without {aggregate: 1}.
        assert.commandFailedWithCode(
            adminDB.runCommand({aggregate: "collname", pipeline: [{$currentOp: {}}], cursor: {}}),
            ErrorCodes.InvalidNamespace);

        // Test that $currentOp fails when run as {aggregate: 1} on a database other than admin.
        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: 1, pipeline: [{$currentOp: {}}], cursor: {}}),
            ErrorCodes.InvalidNamespace);

        // Test that the currentOp command fails when run directly on a database other than admin.
        assert.commandFailedWithCode(testDB.runCommand({currentOp: 1}), ErrorCodes.Unauthorized);

        // Test that the currentOp command helper succeeds when run on a database other than admin.
        // This is because the currentOp shell helper redirects the command to the admin database.
        assert.commandWorked(testDB.currentOp());

        // Test that $currentOp and the currentOp command accept all numeric types.
        const ones = [1, 1.0, NumberInt(1), NumberLong(1), NumberDecimal(1)];

        for (let one of ones) {
            assert.commandWorked(
                adminDB.runCommand({aggregate: one, pipeline: [{$currentOp: {}}], cursor: {}}));

            assert.commandWorked(adminDB.runCommand({currentOp: one, $ownOps: true}));
        }

        // Test that $currentOp with {allUsers: true} succeeds for a user with the "inprog"
        // privilege.
        assert.commandWorked(adminDB.runCommand(
            {aggregate: 1, pipeline: [{$currentOp: {allUsers: true}}], cursor: {}}));

        // Test that the currentOp command with {$ownOps: false} succeeds for a user with the
        // "inprog" privilege.
        assert.commandWorked(adminDB.currentOp({$ownOps: false}));

        // Test that $currentOp succeeds if local readConcern is specified.
        assert.commandWorked(adminDB.runCommand({
            aggregate: 1,
            pipeline: [{$currentOp: {}}],
            readConcern: {level: "local"},
            cursor: {}
        }));

        // Test that $currentOp fails if a non-local readConcern is specified.
        assert.commandFailedWithCode(adminDB.runCommand({
            aggregate: 1,
            pipeline: [{$currentOp: {}}],
            readConcern: {level: "linearizable"},
            cursor: {}
        }),
                                     ErrorCodes.InvalidOptions);

        // Test that {idleConnections: false} returns only active connections.
        const idleConn = new Mongo(conn.host);

        assert.eq(adminDB
                      .aggregate([
                          {$currentOp: {allUsers: true, idleConnections: false}},
                          {$match: {active: false}}
                      ])
                      .itcount(),
                  0);

        // Test that the currentOp command with {$all: false} returns only active connections.
        assert.eq(adminDB.currentOp({$ownOps: false, $all: false, active: false}).inprog.length, 0);

        // Test that {idleConnections: true} returns inactive connections.
        assert.gte(adminDB
                       .aggregate([
                           {$currentOp: {allUsers: true, idleConnections: true}},
                           {$match: {active: false}}
                       ])
                       .itcount(),
                   1);

        // Test that the currentOp command with {$all: true} returns inactive connections.
        assert.gte(adminDB.currentOp({$ownOps: false, $all: true, active: false}).inprog.length, 1);

        // Test that collation rules apply to matches on $currentOp output.
        const matchField = (isMongos ? "originatingCommand.comment" : "command.comment");
        const numExpectedMatches = (isMongos ? 3 : 1);

        assert.eq(
            adminDB
                .aggregate(
                    [{$currentOp: {}}, {$match: {[matchField]: "AGG_currént_op_COLLATION"}}],
                    {
                      collation: {locale: "en_US", strength: 1},  // Case and diacritic insensitive.
                      comment: "agg_current_op_collation"
                    })
                .itcount(),
            numExpectedMatches);

        // Test that $currentOp output can be processed by $facet subpipelines.
        assert.eq(adminDB
                      .aggregate(
                          [
                            {$currentOp: {}},
                            {
                              $facet: {
                                  testFacet: [
                                      {$match: {[matchField]: "agg_current_op_facets"}},
                                      {$count: "count"}
                                  ]
                              }
                            },
                            {$unwind: "$testFacet"},
                            {$replaceRoot: {newRoot: "$testFacet"}}
                          ],
                          {comment: "agg_current_op_facets"})
                      .next()
                      .count,
                  numExpectedMatches);

        // Test that $currentOp is explainable.
        const explainPlan = assert.commandWorked(adminDB.runCommand({
            aggregate: 1,
            pipeline:
                [{$currentOp: {idleConnections: true, allUsers: false}}, {$match: {desc: "test"}}],
            explain: true
        }));

        const expectedStages = [
            {$currentOp: {idleConnections: true, allUsers: false, truncateOps: false}},
            {$match: {desc: {$eq: "test"}}}
        ];

        if (isMongos) {
            assert.eq(explainPlan.splitPipeline.shardsPart, expectedStages);

            for (let i = 0; i < 3; i++) {
                let shardName = st["rs" + i].name;
                assert.eq(explainPlan.shards[shardName].stages, expectedStages);
            }
        } else {
            assert.eq(explainPlan.stages, expectedStages);
        }

        // Test that a user with the inprog privilege can run getMore on a $currentOp aggregation
        // cursor which they created with {allUsers: true}.
        let getMoreCmdRes = assert.commandWorked(
            getMoreTest({conn: conn, showAllUsers: true, getMoreBatchSize: 1}));

        // Test that a user without the inprog privilege cannot run getMore on a $currentOp
        // aggregation cursor created by a user with {allUsers: true}.
        assert(adminDB.logout());
        assert(adminDB.auth("user_no_inprog", "pwd"));

        assert.neq(getMoreCmdRes.cursor.id, 0);
        assert.commandFailedWithCode(adminDB.runCommand({
            getMore: getMoreCmdRes.cursor.id,
            collection: getCollectionNameFromFullNamespace(getMoreCmdRes.cursor.ns),
            batchSize: 100
        }),
                                     ErrorCodes.Unauthorized);
    }

    runCommonTests(shardConn);
    runCommonTests(mongosConn);

    //
    // mongoS specific tests.
    //

    // Test that a user without the inprog privilege cannot run cluster $currentOp via mongoS even
    // if allUsers is false.
    assert(clusterAdminDB.logout());
    assert(clusterAdminDB.auth("user_no_inprog", "pwd"));

    assert.commandFailedWithCode(
        clusterAdminDB.runCommand(
            {aggregate: 1, pipeline: [{$currentOp: {allUsers: false}}], cursor: {}}),
        ErrorCodes.Unauthorized);

    // Test that a user without the inprog privilege cannot run the currentOp command via mongoS
    // even if $ownOps is true.
    assert.commandFailedWithCode(clusterAdminDB.currentOp({$ownOps: true}),
                                 ErrorCodes.Unauthorized);

    // Test that a $currentOp pipeline returns results from all shards, and includes both the shard
    // and host names.
    assert(clusterAdminDB.logout());
    assert(clusterAdminDB.auth("user_inprog", "pwd"));

    assert.eq(clusterAdminDB
                  .aggregate([
                      {$currentOp: {allUsers: true, idleConnections: true}},
                      {$group: {_id: {shard: "$shard", host: "$host"}}},
                      {$sort: {_id: 1}}
                  ])
                  .toArray(),
              [
                {_id: {shard: "aggregation_currentop-rs0", host: st.rs0.getPrimary().host}},
                {_id: {shard: "aggregation_currentop-rs1", host: st.rs1.getPrimary().host}},
                {_id: {shard: "aggregation_currentop-rs2", host: st.rs2.getPrimary().host}}
              ]);

    //
    // ReplSet specific tests.
    //

    // Test that a user with the inprog privilege can see another user's operations with {allUsers:
    // true} when run on a mongoD.
    assert(shardAdminDB.logout());
    assert(shardAdminDB.auth("user_inprog", "pwd"));

    let awaitShell = runInParallelShell({
        testfunc: function() {
            assert.eq(db.getSiblingDB(jsTestName())
                          .test.find({})
                          .comment("agg_current_op_allusers_test")
                          .itcount(),
                      5);
        },
        conn: shardConn,
        username: "admin",
        password: "pwd"
    });

    assertCurrentOpHasSingleMatchingEntry(
        {conn: shardConn, currentOpAggFilter: {"command.comment": "agg_current_op_allusers_test"}});

    // Test that the currentOp command can see another user's operations with {$ownOps: false}.
    assert.eq(
        shardAdminDB.currentOp({$ownOps: false, "command.comment": "agg_current_op_allusers_test"})
            .inprog.length,
        1);

    // Allow the op to complete.
    waitForParallelShell(shardConn, awaitShell);

    // Test that $currentOp succeeds with {allUsers: false} for a user without the "inprog"
    // privilege when run on a mongoD.
    assert(shardAdminDB.logout());
    assert(shardAdminDB.auth("user_no_inprog", "pwd"));

    assert.commandWorked(shardAdminDB.runCommand(
        {aggregate: 1, pipeline: [{$currentOp: {allUsers: false}}], cursor: {}}));

    // Test that the currentOp command succeeds with {$ownOps: true} for a user without the "inprog"
    // privilege when run on a mongoD.
    assert.commandWorked(shardAdminDB.currentOp({$ownOps: true}));

    // Test that a user without the inprog privilege cannot see another user's operations.
    // Temporarily log in as 'user_inprog' to validate that the op is present in $currentOp output.
    assert(shardAdminDB.logout());
    assert(shardAdminDB.auth("user_inprog", "pwd"));

    awaitShell = runInParallelShell({
        testfunc: function() {
            assert.eq(db.getSiblingDB(jsTestName())
                          .test.find({})
                          .comment("agg_current_op_allusers_test")
                          .itcount(),
                      5);
        },
        conn: shardConn,
        username: "admin",
        password: "pwd"
    });

    assertCurrentOpHasSingleMatchingEntry({
        currentOpAggFilter: {"command.comment": "agg_current_op_allusers_test"},
        curOpOpts: {allUsers: true},
        conn: shardConn
    });

    // Log back in as 'user_no_inprog' and validate that the user cannot see the op.
    assert(shardAdminDB.logout());
    assert(shardAdminDB.auth("user_no_inprog", "pwd"));

    assert.eq(shardAdminDB
                  .aggregate([
                      {$currentOp: {allUsers: false}},
                      {$match: {"command.comment": "agg_current_op_allusers_test"}}
                  ])
                  .itcount(),
              0);

    // Test that a user without the inprog privilege cannot see another user's operations via the
    // currentOp command.
    assert.eq(
        shardAdminDB.currentOp({$ownOps: true, "command.comment": "agg_current_op_allusers_test"})
            .inprog.length,
        0);

    waitForParallelShell(shardConn, awaitShell);

    // Test that a user without the inprog privilege can run getMore on a $currentOp cursor which
    // they created with {allUsers: false}.
    assert.commandWorked(getMoreTest({conn: shardConn, showAllUsers: false}));

    // Test that the allUsers parameter is ignored when authentication is disabled.
    restartReplSet(shardRS, {shardsvr: null, keyFile: null});
    // Explicitly set the keyFile to null. If ReplSetTest#stopSet sees a keyFile property, it
    // attempts to auth before dbhash checks.
    shardRS.keyFile = null;

    // Ensure that there is at least one other connection present.
    const otherConn = new Mongo(shardConn.host);

    // Verify that $currentOp displays all operations when auth is disabled regardless of the
    // allUsers parameter, by confirming that we can see non-client system operations when
    // {allUsers: false} is specified.
    assert.gte(shardAdminDB
                   .aggregate([
                       {$currentOp: {allUsers: false, idleConnections: true}},
                       {$match: {connectionId: {$exists: false}}}
                   ])
                   .itcount(),
               1);

    // Verify that the currentOp command displays all operations when auth is disabled regardless of
    // the $ownOps parameter, by confirming that we can see non-client system operations when
    // {$ownOps: true} is specified.
    assert.gte(shardAdminDB.currentOp({$ownOps: true, $all: true, connectionId: {$exists: false}})
                   .inprog.length,
               1);

    // Test that a user can run getMore on a $currentOp cursor when authentication is disabled.
    assert.commandWorked(getMoreTest({conn: shardConn, showAllUsers: true}));

    // Test that the host field is present and the shard field is absent when run on mongoD.
    assert.eq(shardAdminDB
                  .aggregate([
                      {$currentOp: {allUsers: true, idleConnections: true}},
                      {$group: {_id: {shard: "$shard", host: "$host"}}}
                  ])
                  .toArray(),
              [
                {_id: {host: shardConn.host}},
              ]);

    // Test that attempting to 'spoof' a sharded request on non-shardsvr mongoD fails.
    assert.commandFailedWithCode(
        shardAdminDB.runCommand(
            {aggregate: 1, pipeline: [{$currentOp: {}}], fromMongos: true, cursor: {}}),
        40465);

    // Test that an operation which is at the BSON user size limit does not throw an error when the
    // currentOp metadata is added to the output document.
    const bsonUserSizeLimit = assert.commandWorked(shardAdminDB.isMaster()).maxBsonObjectSize;

    let aggPipeline = [
        {$currentOp: {}},
        {
          $match: {
              $or: [
                  {
                    "command.comment": "agg_current_op_bson_limit_test",
                    "command.$truncated": {$exists: false}
                  },
                  {padding: ""}
              ]
          }
        }
    ];

    aggPipeline[1].$match.$or[1].padding =
        "a".repeat(bsonUserSizeLimit - Object.bsonsize(aggPipeline));

    assert.eq(Object.bsonsize(aggPipeline), bsonUserSizeLimit);

    assert.eq(
        shardAdminDB.aggregate(aggPipeline, {comment: "agg_current_op_bson_limit_test"}).itcount(),
        1);

    // Test that $currentOp can run while the mongoD is write-locked.
    awaitShell = startParallelShell(function() {
        assert.commandFailedWithCode(db.adminCommand({sleep: 1, lock: "w", secs: 300}),
                                     ErrorCodes.Interrupted);
    }, shardConn.port);

    const op = assertCurrentOpHasSingleMatchingEntry(
        {conn: shardConn, currentOpAggFilter: {"command.sleep": 1, active: true}});

    assert.commandWorked(shardAdminDB.killOp(op.opid));

    awaitShell();
    st.stop();
})();
