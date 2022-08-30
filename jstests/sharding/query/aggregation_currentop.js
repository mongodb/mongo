/**
 * Tests that the $currentOp aggregation stage behaves as expected. Specifically:
 * - It must be the first stage in the pipeline.
 * - It can only be run on admin, and the "aggregate" field must be 1.
 * - Only active connections are shown unless {idleConnections: true} is specified.
 * - Specifying {localOps: true} shows the local ops on mongoS rather than shard ops.
 * - A user without the inprog privilege can see their own ops, but no-one else's.
 * - A user with the inprog privilege can see all ops.
 * - Non-local readConcerns are rejected.
 * - Collation rules are respected.
 *
 * Also verifies that the aggregation-backed currentOp command obeys the same rules, where
 * applicable.
 *
 * This test requires replica set configuration and user credentials to persist across a restart.
 * @tags: [requires_persistence, uses_transactions, uses_prepare_transaction]
 */

// Restarts cause issues with authentication for awaiting replication.
TestData.skipAwaitingReplicationOnShardsBeforeCheckingUUIDs = true;
// Restarts shard nodes with no keyFile.
TestData.skipCheckOrphans = true;

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.
load("jstests/libs/namespace_utils.js");  // For getCollectionNameFromFullNamespace.

// Replica set nodes started with --shardsvr do not enable key generation until they are added
// to a sharded cluster and reject commands with gossiped clusterTime from users without the
// advanceClusterTime privilege. This causes ShardingTest setup to fail because the shell
// briefly authenticates as __system and receives clusterTime metadata then will fail trying to
// gossip that time later in setup.
//

const key = "jstests/libs/key1";

// Parameters used to establish the sharded cluster.
const stParams = {
    name: jsTestName(),
    keyFile: key,
    shards: 3,
    rs: {nodes: 1, setParameter: {internalQueryExecYieldIterations: 1}}
};

// Create a new sharded cluster for testing. We set the internalQueryExecYieldIterations
// parameter so that plan execution yields on every iteration. For some tests, we will
// temporarily set yields to hang the mongod so we can capture particular operations in the
// currentOp output.
const st = new ShardingTest(stParams);

// Assign various elements of the cluster. We will use shard rs0 to test replica-set level
// $currentOp behaviour.
let shardConn = st.rs0.getPrimary();
let mongosConn = st.s;
let shardRS = st.rs0;

let clusterTestDB = mongosConn.getDB(jsTestName());
let clusterAdminDB = mongosConn.getDB("admin");
shardConn.waitForClusterTime(60);
let shardTestDB = shardConn.getDB(jsTestName());
let shardAdminDB = shardConn.getDB("admin");

function createUsers(conn) {
    let adminDB = conn.getDB("admin");

    // Create an admin user, one user with the inprog privilege, and one without.
    assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "pwd", roles: ["root"]}));
    assert(adminDB.auth("admin", "pwd"));

    assert.commandWorked(adminDB.runCommand({
        createRole: "role_inprog",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["inprog"]}]
    }));

    assert.commandWorked(adminDB.runCommand(
        {createUser: "user_inprog", pwd: "pwd", roles: ["readWriteAnyDatabase", "role_inprog"]}));

    assert.commandWorked(adminDB.runCommand(
        {createUser: "user_no_inprog", pwd: "pwd", roles: ["readWriteAnyDatabase"]}));
}

// Create necessary users at both cluster and shard-local level.
createUsers(shardConn);
createUsers(mongosConn);

// Create a test database and some dummy data on rs0.
assert(clusterAdminDB.auth("admin", "pwd"));

for (let i = 0; i < 5; i++) {
    assert.commandWorked(clusterTestDB.test.insert({_id: i, a: i}));
}

st.ensurePrimaryShard(clusterTestDB.getName(), shardRS.name);

// Restarts a replset with a different set of parameters. Explicitly set the keyFile to null,
// since if ReplSetTest#stopSet sees a keyFile property, it attempts to auth before dbhash
// checks.
function restartReplSet(replSet, newOpts) {
    const numNodes = replSet.nodeList().length;
    for (let n = 0; n < numNodes; n++) {
        replSet.restart(n, newOpts);
    }
    replSet.keyFile = newOpts.keyFile;
    return replSet.getPrimary();
}
// Restarts a cluster with a different set of parameters.
function restartCluster(st, newOpts) {
    restartReplSet(st.configRS, newOpts);
    for (let i = 0; i < stParams.shards; i++) {
        restartReplSet(st[`rs${i}`], newOpts);
    }
    st.restartMongos(0, Object.assign(newOpts, {restart: true}));
    st.keyFile = newOpts.keyFile;
    // Re-link the cluster components.
    shardConn = st.rs0.getPrimary();
    mongosConn = st.s;
    shardRS = st.rs0;
    clusterTestDB = mongosConn.getDB(jsTestName());
    clusterAdminDB = mongosConn.getDB("admin");
    shardTestDB = shardConn.getDB(jsTestName());
    shardAdminDB = shardConn.getDB("admin");
}

function runCommandOnAllPrimaries({dbName, cmdObj, username, password}) {
    for (let i = 0; i < stParams.shards; i++) {
        const rsAdminDB = st[`rs${i}`].getPrimary().getDB("admin");
        rsAdminDB.auth(username, password);
        assert.commandWorked(rsAdminDB.getSiblingDB(dbName).runCommand(cmdObj));
    }
}

// Functions to support running an operation in a parallel shell for testing allUsers behaviour.
function runInParallelShell({conn, testfunc, username, password}) {
    TestData.aggCurOpTest = testfunc;
    TestData.aggCurOpUser = username;
    TestData.aggCurOpPwd = password;

    runCommandOnAllPrimaries({
        dbName: "admin",
        username: username,
        password: password,
        cmdObj: {configureFailPoint: "setYieldAllLocksHang", mode: "alwaysOn"}
    });

    testfunc = function() {
        db.getSiblingDB("admin").auth(TestData.aggCurOpUser, TestData.aggCurOpPwd);
        TestData.aggCurOpTest();
        db.getSiblingDB("admin").logout();
    };

    return startParallelShell(testfunc, conn.port);
}

function assertCurrentOpHasSingleMatchingEntry({conn, currentOpAggFilter, curOpSpec}) {
    curOpSpec = (curOpSpec || {allUsers: true});

    const connAdminDB = conn.getDB("admin");

    let curOpResult;

    assert.soon(
        function() {
            curOpResult =
                connAdminDB.aggregate([{$currentOp: curOpSpec}, {$match: currentOpAggFilter}])
                    .toArray();

            return curOpResult.length === 1;
        },
        function() {
            return "Failed to find operation " + tojson(currentOpAggFilter) +
                " in $currentOp output: " + tojson(curOpResult);
        });

    return curOpResult[0];
}

function waitForParallelShell({conn, username, password, awaitShell}) {
    runCommandOnAllPrimaries({
        dbName: "admin",
        username: username,
        password: password,
        cmdObj: {configureFailPoint: "setYieldAllLocksHang", mode: "off"}
    });

    awaitShell();
}

// Generic function for running getMore on a $currentOp aggregation cursor and returning the
// command response.
function getMoreTest({conn, curOpSpec, getMoreBatchSize}) {
    // Ensure that there are some other connections present so that the result set is larger
    // than 1 $currentOp entry.
    const otherConns = [new Mongo(conn.host), new Mongo(conn.host)];
    curOpSpec = Object.assign({idleConnections: true}, (curOpSpec || {}));

    // Log the other connections in as user_no_inprog so that they will show up for user_inprog
    // with {allUsers: true} and user_no_inprog with {allUsers: false}.
    for (let otherConn of otherConns) {
        assert(otherConn.getDB("admin").auth("user_no_inprog", "pwd"));
    }

    const connAdminDB = conn.getDB("admin");

    const aggCmdRes = assert.commandWorked(connAdminDB.runCommand(
        {aggregate: 1, pipeline: [{$currentOp: curOpSpec}], cursor: {batchSize: 0}}));
    assert.neq(aggCmdRes.cursor.id, 0);

    return connAdminDB.runCommand({
        getMore: aggCmdRes.cursor.id,
        collection: getCollectionNameFromFullNamespace(aggCmdRes.cursor.ns),
        batchSize: (getMoreBatchSize || 100)
    });
}

//
// Common tests.
//

// Runs a suite of tests for behaviour common to both the replica set and cluster levels.
function runCommonTests(conn, curOpSpec) {
    const testDB = conn.getDB(jsTestName());
    const adminDB = conn.getDB("admin");
    curOpSpec = (curOpSpec || {});

    function addToSpec(spec) {
        return Object.assign({}, curOpSpec, spec);
    }

    const isLocalMongosCurOp = (conn == mongosConn && curOpSpec.localOps);
    const isRemoteShardCurOp = (conn == mongosConn && !curOpSpec.localOps);

    // Test that an unauthenticated connection cannot run $currentOp even with {allUsers:
    // false}.
    assert(adminDB.logout());

    assert.commandFailedWithCode(
        adminDB.runCommand(
            {aggregate: 1, pipeline: [{$currentOp: addToSpec({allUsers: false})}], cursor: {}}),
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
            {aggregate: 1, pipeline: [{$currentOp: addToSpec({allUsers: true})}], cursor: {}}),
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
            {aggregate: 1, pipeline: [{$currentOp: {}}, {$currentOp: curOpSpec}], cursor: {}}),
        40602);

    // Test that $currentOp fails when run on admin without {aggregate: 1}.
    assert.commandFailedWithCode(
        adminDB.runCommand(
            {aggregate: "collname", pipeline: [{$currentOp: curOpSpec}], cursor: {}}),
        ErrorCodes.InvalidNamespace);

    // Test that $currentOp fails when run as {aggregate: 1} on a database other than admin.
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$currentOp: curOpSpec}], cursor: {}}),
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
            adminDB.runCommand({aggregate: one, pipeline: [{$currentOp: curOpSpec}], cursor: {}}));

        assert.commandWorked(adminDB.runCommand({currentOp: one, $ownOps: true}));
    }

    // Test that $currentOp with {allUsers: true} succeeds for a user with the "inprog"
    // privilege.
    assert.commandWorked(adminDB.runCommand(
        {aggregate: 1, pipeline: [{$currentOp: addToSpec({allUsers: true})}], cursor: {}}));

    // Test that the currentOp command with {$ownOps: false} succeeds for a user with the
    // "inprog" privilege.
    assert.commandWorked(adminDB.currentOp({$ownOps: false}));

    // Test that $currentOp succeeds if local readConcern is specified.
    assert.commandWorked(adminDB.runCommand({
        aggregate: 1,
        pipeline: [{$currentOp: curOpSpec}],
        readConcern: {level: "local"},
        cursor: {}
    }));

    // Test that $currentOp fails if a non-local readConcern is specified for any data-bearing
    // target.
    const linearizableAggCmd = {
        aggregate: 1,
        pipeline: [{$currentOp: curOpSpec}],
        readConcern: {level: "linearizable"},
        cursor: {}
    };
    assert.commandFailedWithCode(adminDB.runCommand(linearizableAggCmd), ErrorCodes.InvalidOptions);

    // Test that {idleConnections: false} returns only active connections.
    const idleConn = new Mongo(conn.host);

    const activeConns = adminDB
                            .aggregate([
                                {$currentOp: addToSpec({allUsers: true, idleConnections: false})},
                                {$match: {active: false}}
                            ])
                            .toArray();
    assert.eq(activeConns.length,
              0,
              "$currentOp should report 0 active connections but found:\n" + tojson(activeConns));

    // Test that the currentOp command with {$all: false} returns only active connections.
    const result = adminDB.currentOp({$ownOps: false, $all: false, active: false});
    assert.eq(result.inprog.length, 0, result);

    // Test that {idleConnections: true} returns inactive connections.
    assert.gte(adminDB
                   .aggregate([
                       {$currentOp: addToSpec({allUsers: true, idleConnections: true})},
                       {$match: {active: false}}
                   ])
                   .itcount(),
               1);

    // Test that the currentOp command with {$all: true} returns inactive connections.
    assert.gte(adminDB.currentOp({$ownOps: false, $all: true, active: false}).inprog.length, 1);

    // Test that collation rules apply to matches on $currentOp output.
    const matchField =
        (isRemoteShardCurOp ? "cursor.originatingCommand.comment" : "command.comment");
    const numExpectedMatches = (isRemoteShardCurOp ? stParams.shards : 1);

    assert.eq(
        adminDB
            .aggregate(
                [{$currentOp: curOpSpec}, {$match: {[matchField]: "AGG_currént_op_COLLATION"}}], {
                    collation: {locale: "en_US", strength: 1},  // Case and diacritic insensitive.
                    comment: "agg_current_op_collation"
                })
            .itcount(),
        numExpectedMatches);

    // Test that $currentOp output can be processed by $facet subpipelines.
    assert.eq(adminDB
                  .aggregate(
                      [
                          {$currentOp: curOpSpec},
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
        pipeline: [
            {$currentOp: addToSpec({idleConnections: true, allUsers: false})},
            {$match: {desc: "test"}}
        ],
        explain: true
    }));

    let expectedStages = [{$currentOp: {idleConnections: true}}, {$match: {desc: {$eq: "test"}}}];

    if (isRemoteShardCurOp) {
        assert.docEq(explainPlan.splitPipeline.shardsPart, expectedStages);
        for (let i = 0; i < stParams.shards; i++) {
            let shardName = st["rs" + i].name;
            assert.docEq(explainPlan.shards[shardName].stages, expectedStages);
        }
    } else if (isLocalMongosCurOp) {
        expectedStages[0].$currentOp.localOps = true;
        assert.docEq(explainPlan.mongos.stages, expectedStages);
    } else {
        assert.docEq(explainPlan.stages, expectedStages);
    }

    // Test that a user with the inprog privilege can run getMore on a $currentOp aggregation
    // cursor which they created with {allUsers: true}.
    let getMoreCmdRes = assert.commandWorked(
        getMoreTest({conn: conn, curOpSpec: {allUsers: true}, getMoreBatchSize: 1}));

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

// Run the common tests on a shard, through mongoS, and on mongoS with 'localOps' enabled.
runCommonTests(shardConn);
runCommonTests(mongosConn);
runCommonTests(mongosConn, {localOps: true});

//
// mongoS specific tests.
//

// Test that a user without the inprog privilege cannot run non-local $currentOp via mongoS even
// if allUsers is false.
assert(clusterAdminDB.logout());
assert(clusterAdminDB.auth("user_no_inprog", "pwd"));

assert.commandFailedWithCode(
    clusterAdminDB.runCommand(
        {aggregate: 1, pipeline: [{$currentOp: {allUsers: false}}], cursor: {}}),
    ErrorCodes.Unauthorized);

// Test that a user without the inprog privilege cannot run non-local currentOp command via
// mongoS even if $ownOps is true.
assert.commandFailedWithCode(clusterAdminDB.currentOp({$ownOps: true}), ErrorCodes.Unauthorized);

// Test that a non-local $currentOp pipeline via mongoS returns results from all shards, and
// includes both the shard and host names.
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

// Test that a $currentOp pipeline with {localOps:true} returns operations from the mongoS
// itself rather than the shards.
assert.eq(clusterAdminDB
              .aggregate(
                  [
                      {$currentOp: {localOps: true}},
                      {
                          $match: {
                              $expr: {$eq: ["$host", "$clientMetadata.mongos.host"]},
                              "command.comment": "mongos_currentop_localOps"
                          }
                      }
                  ],
                  {comment: "mongos_currentop_localOps"})
              .itcount(),
          1);

//
// localOps tests.
//

// Runs a suite of tests for behaviour common to both replica sets and mongoS with
// {localOps:true}.
function runLocalOpsTests(conn) {
    // The 'localOps' parameter is not supported by the currentOp command, so we limit its
    // testing to the replica set in certain cases.
    const connAdminDB = conn.getDB("admin");
    const isMongos = FixtureHelpers.isMongos(connAdminDB);

    // Test that a user with the inprog privilege can see another user's ops with
    // {allUsers:true}.
    assert(connAdminDB.logout());
    assert(connAdminDB.auth("user_inprog", "pwd"));

    let awaitShell = runInParallelShell({
        testfunc: function() {
            assert.eq(db.getSiblingDB(jsTestName())
                          .test.find({})
                          .comment("agg_current_op_allusers_test")
                          .itcount(),
                      5);
        },
        conn: conn,
        username: "admin",
        password: "pwd"
    });

    assertCurrentOpHasSingleMatchingEntry({
        conn: conn,
        currentOpAggFilter: {"command.comment": "agg_current_op_allusers_test"},
        curOpSpec: {allUsers: true, localOps: true}
    });

    // Test that the currentOp command can see another user's operations with {$ownOps: false}.
    // Only test on a replica set since 'localOps' isn't supported by the currentOp command.
    if (!isMongos) {
        assert.eq(
            connAdminDB
                .currentOp({$ownOps: false, "command.comment": "agg_current_op_allusers_test"})
                .inprog.length,
            1);
    }

    // Test that $currentOp succeeds with {allUsers: false} for a user without the "inprog"
    // privilege.
    assert(connAdminDB.logout());
    assert(connAdminDB.auth("user_no_inprog", "pwd"));

    assert.commandWorked(connAdminDB.runCommand(
        {aggregate: 1, pipeline: [{$currentOp: {allUsers: false, localOps: true}}], cursor: {}}));

    // Test that the currentOp command succeeds with {$ownOps: true} for a user without the
    // "inprog" privilege. Because currentOp does not support the 'localOps' parameter, we only
    // perform this test in the replica set case.
    if (!isMongos) {
        assert.commandWorked(connAdminDB.currentOp({$ownOps: true}));
    }

    // Test that a user without the inprog privilege cannot see another user's operations.
    assert.eq(connAdminDB
                  .aggregate([
                      {$currentOp: {allUsers: false, localOps: true}},
                      {$match: {"command.comment": "agg_current_op_allusers_test"}}
                  ])
                  .itcount(),
              0);

    // Test that a user without the inprog privilege cannot see another user's operations via
    // the currentOp command. Limit this test to the replica set case due to the absence of a
    // 'localOps' parameter for the currentOp command.
    if (!isMongos) {
        assert.eq(connAdminDB
                      .currentOp({$ownOps: true, "command.comment": "agg_current_op_allusers_test"})
                      .inprog.length,
                  0);
    }

    // Release the failpoint and wait for the parallel shell to complete.
    waitForParallelShell({conn: conn, username: "admin", password: "pwd", awaitShell: awaitShell});

    // Test that a user without the inprog privilege can run getMore on a $currentOp cursor
    // which they created with {allUsers: false}.
    assert.commandWorked(getMoreTest({conn: conn, curOpSpec: {allUsers: false, localOps: true}}));
}

// Run the localOps tests for both replset and mongoS.
runLocalOpsTests(mongosConn);
runLocalOpsTests(shardConn);

let sessionDBs = [];
let sessions = [];

// Returns a set of predicates that filter $currentOp for all stashed transactions.
function sessionFilter() {
    return {
        type: "idleSession",
        active: false,
        opid: {$exists: false},
        desc: "inactive transaction",
        "lsid.id": {$in: sessions.map((session) => session.getSessionId().id)},
        "transaction.parameters.txnNumber": {$gte: 0, $lt: sessions.length},
    };
}

//
// Idle sessions tests
//

// Runs a suite of tests to verify idle session behavior with transactions.
// 1. For the mongos connection, verifies that idle transactions are only shown with
// 'idleSessions' and 'localOps' set to true.
// 2. For the shard connection, verifies that stashed transaction locks are displayed only if
// 'idleSessions' is set to true.
function runIdleSessionsTests(conn, adminDB, txnDB, useLocalOps) {
    // Test that $currentOp will display idle transactions if 'idleSessions' is true, and will
    // only permit a user to view other users' sessions if the caller possesses the 'inprog'
    // privilege and 'allUsers' is true.
    const userNames = ["user_inprog", "admin", "user_no_inprog"];

    sessionDBs = [];
    sessions = [];

    for (let i in userNames) {
        adminDB.logout();
        assert(adminDB.auth(userNames[i], "pwd"));

        // Create a session for this user.
        const session = adminDB.getMongo().startSession();

        // For each session, start but do not complete a transaction.
        const sessionDB = session.getDatabase(txnDB.getName());
        assert.commandWorked(sessionDB.runCommand({
            insert: "test",
            documents: [{_id: `txn-insert-${conn}-${userNames[i]}-${i}`}],
            txnNumber: NumberLong(i),
            startTransaction: true,
            autocommit: false
        }));
        sessionDBs.push(sessionDB);
        sessions.push(session);

        // Use $currentOp to confirm that each user can only view their own sessions with
        // 'allUsers:false'.
        assert.eq(
            adminDB
                .aggregate([
                    {$currentOp: {allUsers: false, idleSessions: true, localOps: useLocalOps}},
                    {$match: sessionFilter()}
                ])
                .itcount(),
            1);
    }

    // Log in as 'user_no_inprog' to verify that the user cannot view other users' sessions via
    // 'allUsers:true'.
    adminDB.logout();
    assert(adminDB.auth("user_no_inprog", "pwd"));

    assert.commandFailedWithCode(adminDB.runCommand({
        aggregate: 1,
        cursor: {},
        pipeline: [
            {$currentOp: {allUsers: true, idleSessions: true, localOps: useLocalOps}},
            {$match: sessionFilter()}
        ]
    }),
                                 ErrorCodes.Unauthorized);

    // Log in as 'user_inprog' to confirm that a user with the 'inprog' privilege can see all
    // three idle/stashed transactions with 'allUsers:true'.
    adminDB.logout();
    assert(adminDB.auth("user_inprog", "pwd"));

    assert.eq(adminDB
                  .aggregate([
                      {$currentOp: {allUsers: true, idleSessions: true, localOps: useLocalOps}},
                      {$match: sessionFilter()}
                  ])
                  .itcount(),
              3);

    // Confirm that the 'idleSessions' parameter defaults to true.
    assert.eq(
        adminDB
            .aggregate(
                [{$currentOp: {allUsers: true, localOps: useLocalOps}}, {$match: sessionFilter()}])
            .itcount(),
        3);

    // Confirm that idleSessions:false omits the idle/stashed transactions from the report.
    assert.eq(adminDB
                  .aggregate([
                      {$currentOp: {allUsers: true, idleSessions: false, localOps: useLocalOps}},
                      {$match: sessionFilter()}
                  ])
                  .itcount(),
              0);
    adminDB.logout();

    // Cancel all transactions and close the associated sessions.
    for (let i in userNames) {
        assert(adminDB.auth(userNames[i], "pwd"));
        assert.commandWorked(sessionDBs[i].adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(i),
            autocommit: false,
            writeConcern: {w: 'majority'}
        }));
        sessions[i].endSession();
        adminDB.logout();
    }
}

runIdleSessionsTests(mongosConn, clusterAdminDB, clusterTestDB, true);
runIdleSessionsTests(shardConn, shardAdminDB, shardTestDB, false);

//
// No-auth tests.
//

// Restart the cluster with auth disabled.
restartCluster(st, {keyFile: null});

// Test that $currentOp will display all stashed transaction locks by default if auth is
// disabled, even with 'allUsers:false'.
const session = shardAdminDB.getMongo().startSession();

// Run an operation prior to starting the transaction and save its operation time.
const sessionDB = session.getDatabase(shardTestDB.getName());
var operationTime = undefined;
assert.soonNoExcept(() => {
    const res = assert.commandWorked(sessionDB.runCommand({insert: "test", documents: [{x: 1}]}));
    operationTime = res.operationTime;
    return true;
});

// Set and save the transaction's lifetime. We will use this later to assert that our
// transaction's expiry time is equal to its start time + lifetime.
const transactionLifeTime = 10;
assert.commandWorked(sessionDB.adminCommand(
    {setParameter: 1, transactionLifetimeLimitSeconds: transactionLifeTime}));

// Start but do not complete a transaction.
assert.commandWorked(sessionDB.runCommand({
    insert: "test",
    documents: [{_id: `txn-insert-no-auth`}],
    readConcern: {level: "snapshot"},
    txnNumber: NumberLong(0),
    startTransaction: true,
    autocommit: false
}));
sessionDBs = [sessionDB];
sessions = [session];

const timeAfterTransactionStarts = new ISODate();

// Use $currentOp to confirm that the incomplete transaction has stashed its locks.
assert.eq(
    shardAdminDB.aggregate([{$currentOp: {allUsers: false}}, {$match: sessionFilter()}]).itcount(),
    1);

// Confirm that idleSessions:false omits the stashed locks from the report.
assert.eq(shardAdminDB
              .aggregate(
                  [{$currentOp: {allUsers: false, idleSessions: false}}, {$match: sessionFilter()}])
              .itcount(),
          0);

// Prepare the transaction and ensure the prepareTimestamp is valid.
const prepareRes = assert.commandWorked(sessionDB.adminCommand({
    prepareTransaction: 1,
    txnNumber: NumberLong(0),
    autocommit: false,
    writeConcern: {w: "majority"}
}));
assert(prepareRes.prepareTimestamp,
       "prepareTransaction did not return a 'prepareTimestamp': " + tojson(prepareRes));
assert(prepareRes.prepareTimestamp instanceof Timestamp,
       'prepareTimestamp was not a Timestamp: ' + tojson(prepareRes));
assert.neq(prepareRes.prepareTimestamp,
           Timestamp(0, 0),
           "prepareTimestamp cannot be null: " + tojson(prepareRes));

const timeBeforeCurrentOp = new ISODate();

// Check that the currentOp's transaction subdocument's fields align with our expectations.
let currentOp =
    shardAdminDB.aggregate([{$currentOp: {allUsers: false}}, {$match: sessionFilter()}]).toArray();
let transactionDocument = currentOp[0].transaction;
assert.eq(transactionDocument.parameters.autocommit, false);
assert.eq(transactionDocument.parameters.readConcern.level, "snapshot");
assert.gte(transactionDocument.readTimestamp, operationTime);
// We round timeOpenMicros up to the nearest multiple of 1000 to avoid occasional assertion
// failures caused by timeOpenMicros having microsecond precision while
// timeBeforeCurrentOp/timeAfterTransactionStarts only have millisecond precision.
assert.gte(Math.ceil(transactionDocument.timeOpenMicros / 1000) * 1000,
           (timeBeforeCurrentOp - timeAfterTransactionStarts) * 1000);
assert.gte(transactionDocument.timeActiveMicros, 0);
assert.gte(transactionDocument.timeInactiveMicros, 0);
assert.gte(transactionDocument.timePreparedMicros, 0);
// Not worried about its specific value, validate that in general we return some non-zero &
// valid time greater than epoch time.
assert.gt(ISODate(transactionDocument.startWallClockTime), ISODate("1970-01-01T00:00:00.000Z"));
assert.eq(ISODate(transactionDocument.expiryTime).getTime(),
          ISODate(transactionDocument.startWallClockTime).getTime() + transactionLifeTime * 1000);

// Allow the transactions to complete and close the session. We must commit prepared
// transactions at a timestamp greater than the prepare timestamp.
const commitTimestamp =
    Timestamp(prepareRes.prepareTimestamp.getTime(), prepareRes.prepareTimestamp.getInc() + 1);
assert.commandWorked(sessionDB.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(0),
    autocommit: false,
    writeConcern: {w: 'majority'},
    commitTimestamp: commitTimestamp
}));
session.endSession();

// Run a set of tests of behaviour common to replset and mongoS when auth is disabled.
function runNoAuthTests(conn, curOpSpec) {
    // Test that the allUsers parameter is ignored when authentication is disabled.
    // Ensure that there is at least one other connection present.
    const connAdminDB = conn.getDB("admin");
    const otherConn = new Mongo(conn.host);
    curOpSpec = Object.assign({localOps: false}, (curOpSpec || {}));

    // Verify that $currentOp displays all operations when auth is disabled regardless of the
    // allUsers parameter, by confirming that we can see non-client system operations when
    // {allUsers: false} is specified.
    assert.gte(
        connAdminDB
            .aggregate([
                {
                    $currentOp:
                        {allUsers: false, idleConnections: true, localOps: curOpSpec.localOps}
                },
                {$match: {connectionId: {$exists: false}}}
            ])
            .itcount(),
        1);

    // Verify that the currentOp command displays all operations when auth is disabled
    // regardless of
    // the $ownOps parameter, by confirming that we can see non-client system operations when
    // {$ownOps: true} is specified.
    assert.gte(connAdminDB.currentOp({$ownOps: true, $all: true, connectionId: {$exists: false}})
                   .inprog.length,
               1);

    // Test that a user can run getMore on a $currentOp cursor when authentication is disabled.
    assert.commandWorked(
        getMoreTest({conn: conn, curOpSpec: {allUsers: true, localOps: curOpSpec.localOps}}));
}

runNoAuthTests(shardConn);
runNoAuthTests(mongosConn);
runNoAuthTests(mongosConn, {localOps: true});

//
// Replset specific tests.
//

// Take the replica set out of the cluster.
shardConn = restartReplSet(st.rs0, {shardsvr: null});
shardTestDB = shardConn.getDB(jsTestName());
shardAdminDB = shardConn.getDB("admin");

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
const bsonUserSizeLimit = assert.commandWorked(shardAdminDB.hello()).maxBsonObjectSize;

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

aggPipeline[1].$match.$or[1].padding = "a".repeat(bsonUserSizeLimit - Object.bsonsize(aggPipeline));

assert.eq(Object.bsonsize(aggPipeline), bsonUserSizeLimit);

assert.eq(
    shardAdminDB.aggregate(aggPipeline, {comment: "agg_current_op_bson_limit_test"}).itcount(), 1);

// Test that $currentOp can run while the mongoD is write-locked.
let awaitShell = startParallelShell(function() {
    assert.commandFailedWithCode(db.adminCommand({sleep: 1, lock: "w", secs: 300}),
                                 ErrorCodes.Interrupted);
}, shardConn.port);

const op = assertCurrentOpHasSingleMatchingEntry(
    {conn: shardConn, currentOpAggFilter: {"command.sleep": 1, active: true}});

assert.commandWorked(shardAdminDB.killOp(op.opid));

awaitShell();

// Add the shard back into the replset so that it can be validated by st.stop().
shardConn = restartReplSet(st.rs0, {shardsvr: ""});
st.stop();
})();
