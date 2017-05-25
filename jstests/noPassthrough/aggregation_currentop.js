/**
 * Tests that the $currentOp aggregation stage behaves as expected. Specifically:
 * - It must be the fist stage in the pipeline.
 * - It can only be run on admin, and the "aggregate" field must be 1.
 * - Only active connections are shown unless {idleConnections: true} is specified.
 * - A user without the inprog privilege can see their own ops, but no-one else's.
 * - A user with the inprog privilege can see all ops.
 * - Non-local readConcerns are rejected.
 * - Collation rules are respected.
 */
(function() {
    "use strict";

    const key = "jstests/libs/key1";

    // Create a new replica set for testing. We set the internalQueryExecYieldIterations parameter
    // so that plan execution yields on every iteration. For some tests, we will temporarily set
    // yields to hang the mongod so we can capture particular operations in the currentOp output.
    const rst = new ReplSetTest({
        name: jsTestName(),
        nodes: 3,
        keyFile: key,
        nodeOptions: {setParameter: {internalQueryExecYieldIterations: 1}}
    });

    const nodes = rst.nodeList();

    rst.startSet();
    rst.initiate({
        _id: jsTestName(),
        members: [
            {_id: 0, host: nodes[0], priority: 1},
            {_id: 1, host: nodes[1], priority: 0},
            {_id: 2, host: nodes[2], arbiterOnly: true}
        ],
    });

    let primary = rst.getPrimary();

    let testDB = primary.getDB(jsTestName());
    let adminDB = primary.getDB("admin");

    // Create an admin user, one user with the inprog privilege, and one without.
    assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "pwd", roles: ["root"]}));
    assert(adminDB.auth("admin", "pwd"));

    assert.commandWorked(adminDB.runCommand({
        createRole: "role_inprog",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["inprog"]}]
    }));

    assert.commandWorked(adminDB.runCommand(
        {createUser: "user_inprog", pwd: "pwd", roles: ["role_inprog", "readAnyDatabase"]}));

    assert.commandWorked(
        adminDB.runCommand({createUser: "user_no_inprog", pwd: "pwd", roles: ["readAnyDatabase"]}));

    // Create some dummy test data.
    testDB.test.drop();

    for (let i = 0; i < 5; i++) {
        assert.writeOK(testDB.test.insert({_id: i, a: i}));
    }

    // Functions to support running an operation in a parallel shell for testing allUsers behaviour.
    function runInParallelShell({testfunc, username, password}) {
        TestData.aggCurOpTest = testfunc;
        TestData.aggCurOpUser = username;
        TestData.aggCurOpPwd = password;

        assert.commandWorked(
            adminDB.runCommand({configureFailPoint: "setYieldAllLocksHang", mode: "alwaysOn"}));

        testfunc = function() {
            db.getSiblingDB("admin").auth(TestData.aggCurOpUser, TestData.aggCurOpPwd);
            TestData.aggCurOpTest();
            db.getSiblingDB("admin").logout();
        };

        return startParallelShell(testfunc, primary.port);
    }

    function assertCurrentOpHasSingleMatchingEntry({currentOpAggFilter, curOpOpts}) {
        curOpOpts = (curOpOpts || {allUsers: true});

        let result = null;

        assert.soon(
            function() {
                result = adminDB.runCommand({
                    aggregate: 1,
                    pipeline: [{$currentOp: curOpOpts}, {$match: currentOpAggFilter}],
                    cursor: {}
                });

                assert.commandWorked(result);

                return (result.cursor.firstBatch.length === 1);
            },
            function() {
                return "Failed to find operation in $currentOp output: " + tojson(result);
            });
    }

    function waitForParallelShell(awaitShell) {
        assert.commandWorked(
            adminDB.runCommand({configureFailPoint: "setYieldAllLocksHang", mode: "off"}));

        awaitShell();
    }

    /**
     * Restarts a replica set with additional parameters, and optionally re-authenticates.
     */
    function restartReplSet(replSet, newOpts, user, pwd) {
        const numNodes = replSet.nodeList().length;

        for (let n = 0; n < numNodes; n++) {
            replSet.restart(n, newOpts);
        }

        primary = replSet.getPrimary();
        replSet.awaitSecondaryNodes();

        testDB = primary.getDB(jsTestName());
        adminDB = primary.getDB("admin");

        if (user && pwd) {
            adminDB.auth(user, pwd);
        }
    }

    //
    // Authenticate as user_no_inprog.
    //
    assert(adminDB.logout());
    assert(adminDB.auth("user_no_inprog", "pwd"));

    // Test that $currentOp fails with {allUsers: true} for a user without the "inprog" privilege.
    assert.commandFailedWithCode(
        adminDB.runCommand({aggregate: 1, pipeline: [{$currentOp: {allUsers: true}}], cursor: {}}),
        ErrorCodes.Unauthorized);

    // Test that $currentOp succeeds with {allUsers: false} for a user without the "inprog"
    // privilege.
    assert.commandWorked(adminDB.runCommand(
        {aggregate: 1, pipeline: [{$currentOp: {allUsers: false}}], cursor: {}}));

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
    // $currentOp stages since any other stage in the initial position will trip the {aggregate: 1}
    // namespace check.
    assert.commandFailedWithCode(
        adminDB.runCommand(
            {aggregate: 1, pipeline: [{$currentOp: {}}, {$currentOp: {}}], cursor: {}}),
        ErrorCodes.BadValue);

    // Test that $currentOp succeeds if local readConcern is specified.
    assert.commandWorked(adminDB.runCommand(
        {aggregate: 1, pipeline: [{$currentOp: {}}], readConcern: {level: "local"}, cursor: {}}));

    // Test that $currentOp fails if a non-local readConcern is specified.
    assert.commandFailedWithCode(adminDB.runCommand({
        aggregate: 1,
        pipeline: [{$currentOp: {}}],
        readConcern: {level: "linearizable"},
        cursor: {}
    }),
                                 ErrorCodes.InvalidOptions);

    // Test that a user without the inprog privilege cannot see another user's operations.
    // Temporarily log in as 'user_inprog' to validate that the op is present in $currentOp output.
    assert(adminDB.logout());
    assert(adminDB.auth("user_inprog", "pwd"));

    let awaitShell = runInParallelShell({
        testfunc: function() {
            assert.eq(db.getSiblingDB(jsTestName())
                          .test.find({})
                          .comment("agg_current_op_allusers_test")
                          .itcount(),
                      5);
        },
        username: "user_inprog",
        password: "pwd"
    });

    assertCurrentOpHasSingleMatchingEntry({
        currentOpAggFilter: {"command.comment": "agg_current_op_allusers_test"},
        curOpOpts: {allUsers: true}
    });

    // Log back in as 'user_no_inprog' and validate that the user cannot see the op.
    assert(adminDB.logout());
    assert(adminDB.auth("user_no_inprog", "pwd"));

    assert.eq(adminDB
                  .runCommand({
                      aggregate: 1,
                      pipeline: [
                          {$currentOp: {allUsers: false}},
                          {$match: {"command.comment": "agg_current_op_allusers_test"}}
                      ],
                      cursor: {}
                  })
                  .cursor.firstBatch.length,
              0);

    waitForParallelShell(awaitShell);

    //
    // Authenticate as user_inprog.
    //
    assert(adminDB.logout());
    assert(adminDB.auth("user_inprog", "pwd"));

    // Test that $currentOp with {allUsers: true} succeeds for a user with the "inprog"
    // privilege.
    assert.commandWorked(
        adminDB.runCommand({aggregate: 1, pipeline: [{$currentOp: {allUsers: true}}], cursor: {}}));

    // Test that {idleConnections: false} returns only active connections.
    assert.eq(adminDB
                  .runCommand({
                      aggregate: 1,
                      pipeline: [
                          {$currentOp: {allUsers: true, idleConnections: false}},
                          {$match: {"active": false}}
                      ],
                      cursor: {}
                  })
                  .cursor.firstBatch.length,
              0);

    // Test that {idleConnections: true} returns inactive connections.
    const idleConn = new Mongo(primary.host);

    assert.gte(adminDB
                   .runCommand({
                       aggregate: 1,
                       pipeline: [
                           {$currentOp: {allUsers: true, idleConnections: true}},
                           {$match: {active: false}}
                       ],
                       cursor: {}
                   })
                   .cursor.firstBatch.length,
               1);

    // Test that a user with the inprog privilege can see another user's operations with
    // {allUsers: true}
    awaitShell = runInParallelShell({
        testfunc: function() {
            assert.eq(db.getSiblingDB(jsTestName())
                          .test.find({})
                          .comment("agg_current_op_allusers_test")
                          .itcount(),
                      5);
        },
        username: "user_no_inprog",
        password: "pwd"
    });

    assertCurrentOpHasSingleMatchingEntry(
        {currentOpAggFilter: {"command.comment": "agg_current_op_allusers_test"}});

    waitForParallelShell(awaitShell);

    // Test that collation rules apply to matches on $currentOp output.
    assert.eq(
        adminDB
            .runCommand({
                aggregate: 1,
                pipeline:
                    [{$currentOp: {}}, {$match: {"command.comment": "AGG_currént_op_COLLATION"}}],
                collation: {locale: "en_US", strength: 1},  // Case and diacritic insensitive.
                comment: "agg_current_op_collation",
                cursor: {}
            })
            .cursor.firstBatch.length,
        1);

    // Test that $currentOp is explainable.
    const explainPlan = assert.commandWorked(adminDB.runCommand({
        aggregate: 1,
        pipeline:
            [{$currentOp: {idleConnections: true, allUsers: false}}, {$match: {desc: "test"}}],
        explain: true
    }));

    const expectedStages =
        [{$currentOp: {idleConnections: true, allUsers: false}}, {$match: {desc: "test"}}];

    assert.eq(explainPlan.stages, expectedStages);

    // Test that the allUsers parameter is ignored when authentication is disabled.
    restartReplSet(rst, {keyFile: null});

    // Ensure that there is at least one other connection present.
    const otherConn = new Mongo(primary.host);

    // Verify that $currentOp displays all operations when auth is disabled regardless of the
    // allUsers parameter, by checking that the output is the same in both cases. We project static
    // fields from each operation so that a thread which becomes active between the two aggregations
    // is still comparable across the output of both.
    let aggCmd = {
        aggregate: 1,
        pipeline: [
            {$currentOp: {allUsers: true, idleConnections: true}},
            {$project: {desc: 1, threadId: 1, connectionId: 1, appName: 1}},
            {$sort: {threadId: 1}}
        ],
        cursor: {}
    };

    const aggAllUsersTrue = assert.commandWorked(adminDB.runCommand(aggCmd));
    aggCmd.pipeline[0].$currentOp.allUsers = false;
    const aggAllUsersFalse = assert.commandWorked(adminDB.runCommand(aggCmd));

    assert.eq(aggAllUsersFalse.cursor.firstBatch, aggAllUsersTrue.cursor.firstBatch);
})();
