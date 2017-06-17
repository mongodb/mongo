/**
 * Tests that the $currentOp aggregation stage behaves as expected. Specifically:
 * - It must be the fist stage in the pipeline.
 * - It can only be run on admin, and the "aggregate" field must be 1.
 * - Only active connections are shown unless {idleConnections: true} is specified.
 * - A user without the inprog privilege can see their own ops, but no-one else's.
 * - A user with the inprog privilege can see all ops.
 * - Non-local readConcerns are rejected.
 * - Collation rules are respected.
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

        assert.commandWorked(adminDB.runCommand(
            {createUser: "user_inprog", pwd: "pwd", roles: ["role_inprog", "readAnyDatabase"]}));

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

    // Run a command on the specified database and return a cursor over the result.
    function cmdCursor(inputDB, cmd) {
        return new DBCommandCursor(inputDB.getMongo(),
                                   assert.commandWorked(inputDB.runCommand(cmd)));
    }

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

        assert.soon(
            function() {
                return cmdCursor(connAdminDB, {
                           aggregate: 1,
                           pipeline: [{$currentOp: curOpOpts}, {$match: currentOpAggFilter}],
                           cursor: {}
                       }).itcount() === 1;
            },
            function() {
                const curOps = cmdCursor(
                    connAdminDB, {aggregate: 1, pipeline: [{$currentOp: curOpOpts}], cursor: {}});

                return "Failed to find operation " + tojson(currentOpAggFilter) +
                    " in $currentOp output: " + tojson(curOps.toArray());
            });
    }

    function waitForParallelShell(conn, awaitShell) {
        assert.commandWorked(conn.getDB("admin").runCommand(
            {configureFailPoint: "setYieldAllLocksHang", mode: "off"}));

        awaitShell();
    }

    // Runs a suite of tests for behaviour that is common to both the replica set and cluster
    // levels.
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

        //
        // Authenticate as user_inprog.
        //
        assert(adminDB.logout());
        assert(adminDB.auth("user_inprog", "pwd"));

        // Test that $currentOp fails when run as {aggregate: 1} on a database other than admin.
        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: 1, pipeline: [{$currentOp: {}}], cursor: {}}),
            ErrorCodes.InvalidNamespace);

        // Test that $currentOp fails when run on admin without {aggregate: 1}.
        assert.commandFailedWithCode(
            adminDB.runCommand({aggregate: "collname", pipeline: [{$currentOp: {}}], cursor: {}}),
            ErrorCodes.InvalidNamespace);

        // Test that $currentOp accepts all numeric types.
        const ones = [1, 1.0, NumberInt(1), NumberLong(1), NumberDecimal(1)];

        for (let one of ones) {
            assert.commandWorked(
                adminDB.runCommand({aggregate: one, pipeline: [{$currentOp: {}}], cursor: {}}));
        }

        // Test that {aggregate: 1} fails when the first stage in the pipeline is not $currentOp.
        assert.commandFailedWithCode(
            adminDB.runCommand({aggregate: 1, pipeline: [{$match: {}}], cursor: {}}),
            ErrorCodes.InvalidNamespace);

        // Test that $currentOp fails when it is not the first stage in the pipeline. We use two
        // $currentOp stages since any other stage in the initial position will trip the {aggregate:
        // 1} namespace check.
        assert.commandFailedWithCode(
            adminDB.runCommand(
                {aggregate: 1, pipeline: [{$currentOp: {}}, {$currentOp: {}}], cursor: {}}),
            ErrorCodes.BadValue);

        // Test that $currentOp with {allUsers: true} succeeds for a user with the "inprog"
        // privilege.
        assert.commandWorked(adminDB.runCommand(
            {aggregate: 1, pipeline: [{$currentOp: {allUsers: true}}], cursor: {}}));

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

        assert.eq(cmdCursor(adminDB, {
                      aggregate: 1,
                      pipeline: [
                          {$currentOp: {allUsers: true, idleConnections: false}},
                          {$match: {"active": false}}
                      ],
                      cursor: {}
                  }).itcount(),
                  0);

        // Test that {idleConnections: true} returns inactive connections.
        assert.gte(cmdCursor(adminDB, {
                       aggregate: 1,
                       pipeline: [
                           {$currentOp: {allUsers: true, idleConnections: true}},
                           {$match: {active: false}}
                       ],
                       cursor: {}
                   }).itcount(),
                   1);

        // Test that collation rules apply to matches on $currentOp output.
        const matchField = (isMongos ? "originatingCommand.comment" : "command.comment");
        const numExpectedMatches = (isMongos ? 3 : 1);

        assert.eq(
            cmdCursor(adminDB, {
                aggregate: 1,
                pipeline: [{$currentOp: {}}, {$match: {[matchField]: "AGG_currént_op_COLLATION"}}],
                collation: {locale: "en_US", strength: 1},  // Case and diacritic insensitive.
                comment: "agg_current_op_collation",
                cursor: {}
            }).itcount(),
            numExpectedMatches);

        // Test that $currentOp output can be processed by $facet subpipelines.
        assert.eq(cmdCursor(adminDB, {
                      aggregate: 1,
                      pipeline: [
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
                      comment: "agg_current_op_facets",
                      cursor: {}
                  })
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

        const expectedStages =
            [{$currentOp: {idleConnections: true, allUsers: false}}, {$match: {desc: "test"}}];

        if (isMongos) {
            assert.eq(explainPlan.splitPipeline.shardsPart, expectedStages);

            for (let i = 0; i < 3; i++) {
                let shardName = st["rs" + i].name;
                assert.eq(explainPlan.shards[shardName].stages, expectedStages);
            }
        } else {
            assert.eq(explainPlan.stages, expectedStages);
        }
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

    // Test that a $currentOp pipeline returns results from all shards.
    assert(clusterAdminDB.logout());
    assert(clusterAdminDB.auth("user_inprog", "pwd"));

    assert.eq(cmdCursor(clusterAdminDB, {
                  aggregate: 1,
                  pipeline: [
                      {$currentOp: {allUsers: true}},
                      {$project: {opid: {$split: ["$opid", ":"]}}},
                      {$group: {_id: {$arrayElemAt: ["$opid", 0]}}},
                      {$sort: {_id: 1}}
                  ],
                  cursor: {}
              }).toArray(),
              [
                {_id: "aggregation_currentop-rs0"},
                {_id: "aggregation_currentop-rs1"},
                {_id: "aggregation_currentop-rs2"}
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
        username: "user_no_inprog",
        password: "pwd"
    });

    assertCurrentOpHasSingleMatchingEntry(
        {conn: shardConn, currentOpAggFilter: {"command.comment": "agg_current_op_allusers_test"}});

    waitForParallelShell(shardConn, awaitShell);

    // Test that $currentOp succeeds with {allUsers: false} for a user without the "inprog"
    // privilege when run on a mongoD.
    assert(shardAdminDB.logout());
    assert(shardAdminDB.auth("user_no_inprog", "pwd"));

    assert.commandWorked(shardAdminDB.runCommand(
        {aggregate: 1, pipeline: [{$currentOp: {allUsers: false}}], cursor: {}}));

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
        username: "user_inprog",
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

    assert.eq(cmdCursor(shardAdminDB, {
                  aggregate: 1,
                  pipeline: [
                      {$currentOp: {allUsers: false}},
                      {$match: {"command.comment": "agg_current_op_allusers_test"}}
                  ],
                  cursor: {}
              }).itcount(),
              0);

    waitForParallelShell(shardConn, awaitShell);

    // Test that the allUsers parameter is ignored when authentication is disabled.
    restartReplSet(shardRS, {shardsvr: null, keyFile: null});

    // Ensure that there is at least one other connection present.
    const otherConn = new Mongo(shardConn.host);

    // Verify that $currentOp displays all operations when auth is disabled regardless of the
    // allUsers parameter, by checking that the output is the same in both cases. We project
    // static fields from each operation so that a thread which becomes active between the two
    // aggregations is still comparable across the output of both.
    let aggCmd = {
        aggregate: 1,
        pipeline: [
            {$currentOp: {allUsers: true, idleConnections: true}},
            {$project: {desc: 1, threadId: 1, connectionId: 1, appName: 1}},
            {$sort: {threadId: 1}}
        ],
        cursor: {}
    };

    const aggAllUsersTrue = cmdCursor(shardAdminDB, aggCmd).toArray();
    aggCmd.pipeline[0].$currentOp.allUsers = false;
    const aggAllUsersFalse = cmdCursor(shardAdminDB, aggCmd).toArray();

    assert.eq(aggAllUsersFalse, aggAllUsersTrue);
})();
