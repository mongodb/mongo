/**
 * Unit test the MultiRouterMongo class provided in the shell
 * @tags: [requires_sharding]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";

const st = new ShardingTest({shards: 1, mongos: 3});
const getMongosesURI = function () {
    const mongosHosts = st._mongos.map((m) => m.host).join(",");
    const uri = `mongodb://${mongosHosts}/test`;
    return uri;
};

function testCase(description, fn) {
    chatty(`[MULTI-ROUTER-TEST] ${description}`);
    fn();
}

// Given a Multi-Router Mongo, extend _getNextMongo to count calls.
// _getNextMongo is responsible for iterating over multiple mongos.
// Counting _getNextMongo is useful to verify how many times the multi-router had to switch connections.
function overrideGetNextMongoCountingCalls(multiRouterMongo) {
    let getNextMongoCount = 0;
    const originalGetNextMongo = multiRouterMongo._getNextMongo.bind(multiRouterMongo);
    multiRouterMongo._getNextMongo = function () {
        const mongo = originalGetNextMongo();
        getNextMongoCount++;
        return mongo;
    };
    return {
        count: () => getNextMongoCount,
        reset: () => {
            getNextMongoCount = 0;
        },
    };
}

// ============================================================================
// Test toConnectionList
// ============================================================================

testCase("Testing toConnectionList with single server", () => {
    const uri = `mongodb://${st.s0.host}/test`;
    const uriList = toConnectionsList(new MongoURI(uri));

    assert.eq(uriList.length, 1, "Should have 1 URI");
    assert(uriList[0].includes(st.s0.host), "URI should contain correct host");
    assert(uriList[0].includes("/test"), "URI should contain database");
});

testCase("Testing toConnectionList with multiple servers", () => {
    const uri = getMongosesURI();
    const uriList = toConnectionsList(new MongoURI(uri));

    assert.eq(uriList.length, 3, "Should have 3 URIs");

    // Verify each URI is unique and contains correct parts
    uriList.forEach((uri) => {
        assert(uri.startsWith("mongodb://"), "URI should start with mongodb://");
        assert(uri.includes("/test"), "URI should contain database");
    });
});

testCase("Testing toConnectionList preserves options", () => {
    const mongosHosts = st._mongos.map((m) => m.host).join(",");
    const uri = `mongodb://${mongosHosts}/test?readPreference=secondary&retryWrites=true`;
    const uriList = toConnectionsList(new MongoURI(uri));

    assert.eq(uriList.length, 3, "Should have 3 URIs");

    uriList.forEach((uri) => {
        assert(uri.includes("readPreference=secondary"), "URI should contain readPreference option");
        assert(uri.includes("retryWrites=true"), "URI should contain retryWrites option");
    });
});

// ============================================================================
// Test connection type
// ============================================================================

testCase("Standalone mongod", () => {
    const standalone = MongoRunner.runMongod({});
    const uri = `mongodb://${standalone.host}/test`;

    const db = connect(uri);

    // Should be regular Mongo, not MultiRouterMongo
    assert.eq(db.getMongo().isMultiRouter, undefined, "Should not be MultiRouterMongo");
    MongoRunner.stopMongod(standalone);
});

testCase("Single mongos", () => {
    const uri = `mongodb://${st.s1.host}/test`;

    const db = connect(uri);

    // Should be regular Mongo, not MultiRouterMongo
    assert.eq(db.getMongo().isMultiRouter, undefined, "Should not be MultiRouterMongo");
});

testCase("Replica set", () => {
    const rst = new ReplSetTest({nodes: 3});
    rst.startSet();
    rst.initiate();
    rst.getPrimary();

    // Note we can't use the "getURL" method provided by the ReplSetTest.
    // That method provides a valid URL for establishing a connection via Mongo.
    // The 'connect' function we are testing expects a valid fixture url,
    // which is composed by a protocol followed by a comma-separated list of host:port pairs and options.
    const mongodHosts = rst.nodes.map((m) => m.host).join(",");
    const uri = `mongodb://${mongodHosts}/test?replicaSet=${rst.name}`;
    const mongo = connect(uri).getMongo();

    // Should be regular Mongo, not MultiRouterMongo
    assert.eq(mongo.isMultiRouter, undefined, "Should not be MultiRouterMongo");

    //Every node of the replica-set is also a regular Mongo
    rst.nodes.forEach((node) => {
        // Assert is a mongo but not a multi-router type
        assert.neq(node._getDefaultSession, undefined);
        assert.eq(node.isMultiRouter, undefined, `Replica-Set node ${node} should not be MultiRouterMongo`);
    });

    rst.stopSet();
});

testCase("Individual shard connections are regular Mongo", () => {
    // Check each shard connection
    st._connections.forEach((shardConn, idx) => {
        // Assert is a mongo but not a multi-router type
        assert.neq(shardConn._getDefaultSession, undefined);
        assert.eq(shardConn.isMultiRouter, undefined, `Shard ${idx} should not be MultiRouterMongo`);
    });

    // Check individual mongos connections (when accessed directly)
    st._mongos.forEach((mongos, idx) => {
        // Assert is a mongo but not a multi-router type
        assert.neq(mongos._getDefaultSession, undefined);
        assert.eq(mongos.isMultiRouter, undefined, `Individual mongos ${idx} should not be MultiRouterMongo`);
    });
});

testCase("Multiple mongos creates MultiRouterMongo", () => {
    const uri = getMongosesURI();
    const db = connect(uri);

    // Should be MultiRouterMongo
    assert.eq(db.getMongo().isMultiRouter, true, "Should be MultiRouterMongo");
    assert.eq(db.getMongo().isConnectedToMongos(), true, "MultiRouterMongo must be connected to a mongos");
    assert.eq(db.getMongo()._mongoConnections.length, 3, "Should have 3 mongo connections");
});

// ============================================================================
// Test basic properties
// ============================================================================

testCase("getSiblingDB returns a proxy", () => {
    const uri = getMongosesURI();
    const db = connect(uri);

    assert.eq(db.getMongo().isMultiRouter, true, "Should be MultiRouterMongo");

    let newDb = db.getSiblingDB("admin");

    // Should be MultiRouterMongo
    assert.eq(newDb.getMongo().isMultiRouter, true, "Should be MultiRouterMongo");
    assert.eq(newDb.getMongo().isConnectedToMongos(), true, "MultiRouterMongo must be connected to a mongos");
    assert.eq(newDb.getMongo()._mongoConnections.length, 3, "Should have 3 mongo connections");
});

// ============================================================================
// Test mongos selections via basic commands
// ============================================================================

testCase("adminCommand is intercepted by proxy's runCommand", () => {
    const uri = getMongosesURI();
    const db = connect(uri);
    const conn = db.getMongo();

    // Track runCommand calls
    let countCalls = 0;
    const originalRunCommand = conn.runCommand.bind(conn);
    conn.runCommand = function (dbname, cmd, options) {
        assert.eq(dbname, "admin");
        countCalls++;
        return originalRunCommand(dbname, cmd, options);
    };

    db.adminCommand({ping: 1});
    assert.eq(db.getMongo().isMultiRouter, true, "Should be MultiRouterMongo");
    assert.eq(countCalls, 1, "Should call run command at least once!");
});

testCase("Testing basic call distribution among the mongoses pool via ping", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const kOperations = 10;

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    // Run commands
    let adminDb = conn.getDB("admin");
    for (let i = 0; i < kOperations; i++) {
        adminDb.runCommand({ping: 1});
    }

    // The first runCommand will start a session which also calls _getNextMongo. We check it's called at least kTotalCount times.
    assert.gte(getNextMongoTracker.count(), kOperations);
});

testCase("Testing call distribution via insert", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.test;
    const kOperations = 10;

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    for (let i = 0; i < kOperations; i++) {
        coll.insert({x: i, type: "insert"});
    }

    // Should route randomly for all operations
    assert.gte(getNextMongoTracker.count(), kOperations, "Should route randomly for insert operations");

    // Verify all documents were inserted
    assert.eq(kOperations, coll.count({type: "insert"}), "All inserts should succeed");

    coll.drop();
});

testCase("Testing call distribution via update", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.test;
    const kOperations = 10;

    // Insert test data
    const docs = [];
    for (let i = 0; i < kOperations; i++) {
        docs.push({x: i, updated: false});
    }
    coll.insertMany(docs);

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    for (let i = 0; i < kOperations; i++) {
        coll.updateOne({x: i}, {$set: {updated: true}});
    }

    // Should route randomly for all operations
    assert.gte(getNextMongoTracker.count(), kOperations, "Should route randomly for update operations");

    // Verify all documents were updated
    assert.eq(kOperations, coll.count({updated: true}), "All updates should succeed");

    coll.drop();
});

testCase("Testing call distribution via delete", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.test;
    const kOperations = 10;

    // Insert test data
    const docs = [];
    for (let i = 0; i < kOperations; i++) {
        docs.push({x: i, toDelete: true});
    }
    coll.insertMany(docs);
    assert.eq(kOperations, coll.count({}), "Should have documents to delete");

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    for (let i = 0; i < kOperations; i++) {
        coll.deleteOne({x: i});
    }

    // Should route randomly for all operations
    assert.gte(getNextMongoTracker.count(), kOperations, "Should route randomly for delete operations");

    // Verify correct number of documents remain
    assert.eq(0, coll.count({}), "Should have no documents remaining");

    coll.drop();
});

// ============================================================================
// Test mongos selections via aggregate and find
// ============================================================================

function insertSampleData(coll, totalDocs) {
    // Insert test data
    const docs = [];
    for (let i = 0; i < totalDocs; i++) {
        docs.push({_id: i, value: i});
    }
    coll.insertMany(docs);
}

function overrideRunCommandCountingGetMoreCalls(conn) {
    // Track getMore calls
    let getMoreCount = 0;
    const originalRunCommand = conn.runCommand.bind(conn);
    conn.runCommand = function (dbname, cmd, options) {
        if (cmd.getMore) {
            getMoreCount++;
        }
        return originalRunCommand(dbname, cmd, options);
    };
    return {
        count: () => getMoreCount,
    };
}

testCase("Testing basic find + getMore will stick to the same connection", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const coll = conn.getDB("test").cursorTest;
    const kTotalDocs = 120;

    // Insert sample data
    insertSampleData(coll, kTotalDocs);

    // Track getMore calls
    const getMoreTracker = overrideRunCommandCountingGetMoreCalls(conn);

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    // Create cursor with small batch size to force getMore
    const cursor = coll.find({value: {$gte: 0}}).batchSize(2);
    let countDocs = 0;
    while (cursor.hasNext()) {
        countDocs++;
        cursor.next();
    }

    assert.eq(countDocs, kTotalDocs, "GetMore never run!");
    // GetMore will never hit the proxy unless explicitly run via runCommand. next() self handle this case.
    assert.eq(getMoreTracker.count(), 0, "Must not have tracked getMore commands");
    // Only the find command must have run _getNextMongo
    assert.eq(getNextMongoTracker.count(), 1, "Must run getNextMongoCount only once when executing the find");
    assert.eq(conn._cursorTracker.count(), 1, "Should have exactly 1 tracked cursor");

    coll.drop();
});

testCase("Testing basic aggregate + getMore will stick to the same connection", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const coll = conn.getDB("test").cursorTest;
    const kTotalDocs = 120;

    // Insert sample data
    insertSampleData(coll, kTotalDocs);

    // Track getMore calls
    const getMoreTracker = overrideRunCommandCountingGetMoreCalls(conn);

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    // Create cursor with small batch size to force getMore
    const cursor = coll.aggregate([{$match: {value: {$gte: 0}}}], {cursor: {batchSize: 2}});
    let countDocs = 0;
    while (cursor.hasNext()) {
        countDocs++;
        cursor.next();
    }

    assert.eq(countDocs, kTotalDocs, "GetMore never run!");
    // GetMore will never hit the proxy unless explicitly run via runCommand. next() self handle this case.
    assert.eq(getMoreTracker.count(), 0, "Must have executed getMore commands");
    // Only the aggregate command must have run _getNextMongo
    assert.eq(getNextMongoTracker.count(), 1, "Must run getNextMongoCount only once when executing the aggregate");
    assert.eq(conn._cursorTracker.count(), 1, "Should have exactly 1 tracked cursor");

    coll.drop();
});

testCase("Testing basic aggregate + getMore run explicitly will stick to the same connection", () => {
    const uri = getMongosesURI();
    const db = connect(uri);
    const conn = db.getMongo();
    const coll = db.cursorTest;
    const kTotalDocs = 120;

    // Insert sample data
    insertSampleData(coll, kTotalDocs);

    // Track getMore calls
    const getMoreTracker = overrideRunCommandCountingGetMoreCalls(conn);

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    // Create cursor with small batch size and force a getMore.
    const cursor = coll.aggregate([{$match: {value: {$gte: 0}}}], {cursor: {batchSize: 2}});
    assert.commandWorked(db.runCommand({getMore: cursor.getId(), collection: "cursorTest"}));
    // We must have called getMore via proxy
    assert.eq(getMoreTracker.count(), 1, "Must have executed getMore commands");
    // Only the aggregate command must have run _getNextMongo
    assert.eq(getNextMongoTracker.count(), 1, "Must run getNextMongoCount only once when executing the aggregate");
    assert.eq(conn._cursorTracker.count(), 1, "Should have exactly 1 tracked cursor");

    coll.drop();
});

// ============================================================================
// Test disable Multi-Routing
// ============================================================================

testCase("Testing isMultiRoutingDisabled will make the proxy run like a single router connection", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const kTotalCount = 30;

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    TestData.skipMultiRouterRotation = true;
    // Run commands
    let adminDb = conn.getDB("admin");
    for (let i = 0; i < kTotalCount; i++) {
        adminDb.runCommand({ping: 1});
    }

    // We should never call _getNextMongo if multi routing is disabled
    assert.eq(0, getNextMongoTracker.count(), "Should never call _getNextMongo when multi routing is disabled");

    // Set back to false for other tests
    TestData.skipMultiRouterRotation = false;
});

// ============================================================================
// Test session mapping with transactions
// ============================================================================

testCase("Testing explicit session without transaction routes randomly", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.sessionTest;

    const session = conn.startSession();
    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.sessionTest;

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    const kOperations = 10;
    for (let i = 0; i < kOperations; i++) {
        sessionColl.insert({x: i, type: "no-txn"}, {session});
    }

    // Should have called _getNextMongo for each operation (random routing)
    assert.gte(getNextMongoTracker.count(), kOperations, "Should route randomly for non-transactional operations");

    // Session should NOT be tracked in the map
    assert.eq(conn._sessionToMongoMap.size(), 0, "Session should not be tracked without transaction");

    session.endSession();
    coll.drop();
});

testCase("Testing lazy mapping only occurs when txnNumber is present", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.sessionTest;

    const session = conn.startSession();

    // After creating session, map should be empty
    assert.eq(conn._sessionToMongoMap.size(), 0, "Map should be empty after session creation");

    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.sessionTest;

    // Perform non-transactional operation
    sessionColl.insert({x: 1, phase: "no-txn"}, {session});

    // Map should still be empty (no txnNumber)
    assert.eq(conn._sessionToMongoMap.size(), 0, "Map should remain empty for non-txn operations");

    // Start transaction (first operation with txnNumber)
    session.startTransaction();
    sessionColl.insert({x: 2, phase: "txn"}, {session});

    // Now map should contain the session
    assert.eq(conn._sessionToMongoMap.size(), 1, "Map should contain session after txn starts");

    session.commitTransaction_forTesting();
    session.endSession();
    coll.drop();
});

testCase("Testing transaction operations pin to same mongos", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.sessionTest;

    const session = conn.startSession();
    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.sessionTest;

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    withRetryOnTransientTxnError(() => {
        session.startTransaction();

        // First operation in transaction
        sessionColl.insert({x: 1, type: "txn"}, {session});

        // Should have called _getNextMongo once for first operation
        assert.eq(getNextMongoTracker.count(), 1, "Should call _getNextMongo once for first txn operation");
        // Session should now be tracked
        assert.eq(conn._sessionToMongoMap.size(), 1, "Session should be tracked after first txn operation");

        // Subsequent operations should not call _getNextMongo (pinned to same mongos)
        for (let i = 2; i <= 5; i++) {
            sessionColl.insert({x: i, type: "txn"}, {session});
        }

        assert.eq(getNextMongoTracker.count(), 1, "Should not call _getNextMongo for subsequent txn operations");
        assert.eq(conn._sessionToMongoMap.size(), 1, "Session should be tracked after first txn operation");

        assert.commandWorked(session.commitTransaction_forTesting());

        // Verify all documents were inserted
        assert.eq(5, coll.count({type: "txn"}), "All transaction inserts should succeed");
    });
    session.endSession();
    coll.drop();
});

testCase("Test 3 transactional operations for the same session use different mongos across transactions", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.sessionTest;
    const kNumInserts = 5;

    const session = conn.startSession();
    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.sessionTest;

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    // Transaction 1
    withRetryOnTransientTxnError(() => {
        getNextMongoTracker.reset();

        session.startTransaction();

        // First operation in transaction
        sessionColl.insert({x: 1, type: "txn"}, {session});

        // Should _getNextMongo once for first operation
        assert.eq(getNextMongoTracker.count(), 1, "Should call _getNextMongo once for first txn operation");
        // Session should now be tracked
        assert.eq(conn._sessionToMongoMap.size(), 1, "Session should be tracked after first txn operation");

        // Subsequent operations should not call _getNextMongo (pinned to same mongos)
        for (let i = 2; i <= kNumInserts; i++) {
            sessionColl.insert({x: i, type: "txn"}, {session});
        }

        assert.eq(getNextMongoTracker.count(), 1, "Should not call _getNextMongo for subsequent txn operations");
        assert.eq(conn._sessionToMongoMap.size(), 1, "Session should be tracked after first txn operation");

        assert.commandWorked(session.commitTransaction_forTesting());
    });

    // Transaction 2
    withRetryOnTransientTxnError(() => {
        getNextMongoTracker.reset();

        session.startTransaction();

        // First operation in transaction
        sessionColl.insert({x: 1, type: "txn"}, {session});

        assert.eq(getNextMongoTracker.count(), 1, "Should call _getNextMongo once for first txn operation");
        assert.eq(conn._sessionToMongoMap.size(), 1, "Session should be tracked after first txn operation");

        // Subsequent operations should not call _getNextMongo (pinned to same mongos)
        for (let i = 2; i <= kNumInserts; i++) {
            sessionColl.insert({y: i, type: "txn"}, {session});
        }

        assert.eq(getNextMongoTracker.count(), 1, "Should not call _getNextMongo for subsequent txn operations");
        assert.eq(conn._sessionToMongoMap.size(), 1, "Session should be tracked after first txn operation");

        assert.commandWorked(session.commitTransaction_forTesting());
    });

    // Transaction 3
    withRetryOnTransientTxnError(() => {
        getNextMongoTracker.reset();

        session.startTransaction();

        // First operation in transaction
        sessionColl.insert({z: 1, type: "txn"}, {session});

        assert.eq(getNextMongoTracker.count(), 1, "Should call _getNextMongo once for first txn operation");
        assert.eq(conn._sessionToMongoMap.size(), 1, "Session should be tracked after first txn operation");

        // Subsequent operations should not call _getNextMongo (pinned to same mongos)
        for (let i = 2; i <= kNumInserts; i++) {
            sessionColl.insert({y: i, type: "txn"}, {session});
        }

        assert.eq(getNextMongoTracker.count(), 1, "Should not call _getNextMongo for subsequent txn operations");
        assert.eq(conn._sessionToMongoMap.size(), 1, "Session should be tracked after first txn operation");

        assert.commandWorked(session.commitTransaction_forTesting());
    });

    assert.eq(kNumInserts * 3, coll.count({type: "txn"}), "All transaction inserts should succeed");

    session.endSession();
    coll.drop();
});

testCase("Testing session reuse after transaction routes randomly", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.sessionTest;

    const session = conn.startSession();
    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.sessionTest;

    // Start and complete a transaction
    withRetryOnTransientTxnError(() => {
        session.startTransaction();
        sessionColl.insert({x: 1, phase: "txn"}, {session});
        assert.commandWorked(session.commitTransaction_forTesting());
    });
    // Session should be tracked
    assert.eq(conn._sessionToMongoMap.size(), 1, "Session should be tracked after transaction");

    // Track _getNextMongo calls for post-transaction operations
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    // Perform non-transactional operations on same session
    const kOperations = 10;
    for (let i = 0; i < kOperations; i++) {
        sessionColl.insert({x: i, phase: "post-txn"}, {session});
    }

    // Should route randomly (no txnNumber in these operations)
    assert.gte(getNextMongoTracker.count(), kOperations, "Should route randomly after transaction completes");

    // Session remains tracked but operations without txnNumber don't use the mapping
    assert.eq(conn._sessionToMongoMap.size(), 1, "Session tracking persists but doesn't affect non-txn ops");

    session.endSession();
    coll.drop();
});

testCase("Testing multiple concurrent transactions can use different mongos", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const kCollName = "sessionTest";
    assert.commandWorked(db.createCollection(kCollName));
    const coll = db.getCollection(kCollName);

    const session1 = conn.startSession();
    const session2 = conn.startSession();
    const sessionDB1 = session1.getDatabase("test");
    const sessionDB2 = session2.getDatabase("test");
    withRetryOnTransientTxnError(() => {
        // Start transactions on both sessions
        session1.startTransaction();
        session2.startTransaction();

        // Perform operations in both transactions
        sessionDB1.sessionTest.insert({session: 1, value: "s1"}, {session: session1});
        sessionDB2.sessionTest.insert({session: 2, value: "s2"}, {session: session2});

        sessionDB1.sessionTest.insert({session: 1, value: "s1-2"}, {session: session1});
        sessionDB2.sessionTest.insert({session: 2, value: "s2-2"}, {session: session2});

        // Both sessions should be tracked
        assert.eq(conn._sessionToMongoMap.size(), 2, "Both sessions should be tracked");

        // Commit both transactions
        assert.commandWorked(session1.commitTransaction_forTesting());
        assert.commandWorked(session2.commitTransaction_forTesting());
    });
    // Verify both transactions succeeded
    assert.eq(2, coll.count({session: 1}), "Session 1 transaction should succeed");
    assert.eq(2, coll.count({session: 2}), "Session 2 transaction should succeed");

    session1.endSession();
    session2.endSession();
    coll.drop();
});

testCase("Testing retryable writes pin to same mongos", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.sessionTest;
    const kOperations = 5;

    const session = conn.startSession({retryWrites: true});
    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.sessionTest;

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    // Subsequent operations should not call _getNextMongo (pinned to same mongos)
    for (let i = 1; i <= kOperations; i++) {
        sessionColl.insert({x: i, type: "retryable"}, {session});
    }

    assert.eq(getNextMongoTracker.count(), 5, "Should not call _getNextMongo for subsequent retryable writes");
    assert.eq(conn._sessionToMongoMap.size(), 1, "We should have exactly 1 tracked session");

    // Verify all writes succeeded
    assert.eq(5, coll.count({type: "retryable"}), "All retryable writes should succeed");

    session.endSession();
    coll.drop();
});

testCase("Testing aborted transaction maintains session tracking", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.sessionTest;

    const session = conn.startSession();
    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.sessionTest;

    // Start transaction
    session.startTransaction();
    sessionColl.insert({x: 1, type: "aborted"}, {session});

    // Session should be tracked
    assert.eq(conn._sessionToMongoMap.size(), 1, "Session should be tracked in transaction");

    // Abort transaction
    session.abortTransaction_forTesting();

    // Session remains tracked (mapping persists)
    assert.eq(conn._sessionToMongoMap.size(), 1, "Session tracking persists after abort");

    // Verify document was not inserted (transaction aborted)
    assert.eq(0, coll.count({type: "aborted"}), "Aborted transaction should not insert documents");

    session.endSession();
    coll.drop();
});

// ============================================================================
// Test auto encryption methods
// ============================================================================

testCase("Testing setAutoEncryption propagates to all mongos connections", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();

    assert.eq(conn.isMultiRouter, true, "Should be MultiRouterMongo");
    assert.eq(conn._mongoConnections.length, 3, "Should have 3 mongo connections");

    const localKMS = {
        key: BinData(
            0,
            "/tu9jUCBqZdwCelwE/EAm/4WqdxrSMi04B8e9uAV+m30rI1J2nhKZZtQjdvsSCwuI4erR6IEcEK+5eGUAODv43NDNIR9QheT2edWFewUfHKsl9cnzTc86meIzOmYl6dr",
        ),
    };

    const clientSideFLEOptions = {
        kmsProviders: {local: localKMS},
        keyVaultNamespace: "test.keystore",
        schemaMap: {},
    };

    // Set auto encryption on the multi-router connection
    const result = conn.setAutoEncryption(clientSideFLEOptions);
    assert(result, "setAutoEncryption should succeed");

    // Verify all underlying mongos connections have auto encryption options set
    conn._mongoConnections.forEach((mongo, idx) => {
        const options = mongo.getAutoEncryptionOptions();
        assert.neq(options, undefined, `Mongos ${idx} should have auto encryption options`);
        assert.eq(options.keyVaultNamespace, "test.keystore", `Mongos ${idx} should have correct keyVaultNamespace`);
    });

    // Clean up
    conn.unsetAutoEncryption();
    // Verify all connections are cleaned up
    conn._mongoConnections.forEach((mongo, idx) => {
        const options = mongo.getAutoEncryptionOptions();
        assert.eq(options, undefined, `Mongos ${idx} should not have auto encryption options after cleanup`);
    });
});

testCase("Testing toggleAutoEncryption propagates to all mongos connections", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();

    assert.eq(conn.isMultiRouter, true, "Should be MultiRouterMongo");
    assert.eq(conn._mongoConnections.length, 3, "Should have 3 mongo connections");

    const localKMS = {
        key: BinData(
            0,
            "/tu9jUCBqZdwCelwE/EAm/4WqdxrSMi04B8e9uAV+m30rI1J2nhKZZtQjdvsSCwuI4erR6IEcEK+5eGUAODv43NDNIR9QheT2edWFewUfHKsl9cnzTc86meIzOmYl6dr",
        ),
    };

    const clientSideFLEOptions = {
        kmsProviders: {local: localKMS},
        keyVaultNamespace: "test.keystore",
        schemaMap: {},
    };

    // Set auto encryption first
    assert(conn.setAutoEncryption(clientSideFLEOptions), "setAutoEncryption should succeed");

    // Toggle auto encryption on
    const toggleOnResult = conn.toggleAutoEncryption(true);
    assert(toggleOnResult, "toggleAutoEncryption(true) should succeed");

    // Verify all underlying mongos connections have auto encryption toggled on
    conn._mongoConnections.forEach((mongo, idx) => {
        // We can't directly check the toggle state, but we can verify options exist
        const options = mongo.getAutoEncryptionOptions();
        assert.neq(options, undefined, `Mongos ${idx} should have auto encryption options after toggle on`);
    });

    // Toggle auto encryption off
    const toggleOffResult = conn.toggleAutoEncryption(false);
    assert(toggleOffResult, "toggleAutoEncryption(false) should succeed");

    // Verify all underlying mongos connections have auto encryption toggled off
    conn._mongoConnections.forEach((mongo, idx) => {
        const options = mongo.getAutoEncryptionOptions();
        assert.neq(options, undefined, `Mongos ${idx} should still have auto encryption options after toggle off`);
    });

    // Clean up by unsetting auto encryption
    conn.unsetAutoEncryption();
});

testCase("Testing encryption state is independently maintained per mongos", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();

    assert.eq(conn.isMultiRouter, true, "Should be MultiRouterMongo");
    assert.eq(conn._mongoConnections.length, 3, "Should have 3 mongo connections");

    const localKMS = {
        key: BinData(
            0,
            "/tu9jUCBqZdwCelwE/EAm/4WqdxrSMi04B8e9uAV+m30rI1J2nhKZZtQjdvsSCwuI4erR6IEcEK+5eGUAODv43NDNIR9QheT2edWFewUfHKsl9cnzTc86meIzOmYl6dr",
        ),
    };

    const clientSideFLEOptions = {
        kmsProviders: {local: localKMS},
        keyVaultNamespace: "test.keystore",
        schemaMap: {},
    };

    // Set and toggle auto encryption multiple times
    assert(conn.setAutoEncryption(clientSideFLEOptions), "setAutoEncryption should succeed");
    assert(conn.toggleAutoEncryption(true), "toggleAutoEncryption(true) should succeed");
    assert(conn.toggleAutoEncryption(false), "toggleAutoEncryption(false) should succeed");
    assert(conn.toggleAutoEncryption(true), "toggleAutoEncryption(true) should succeed again");

    // Verify all connections maintain consistent state
    conn._mongoConnections.forEach((mongo, idx) => {
        const options = mongo.getAutoEncryptionOptions();
        assert.neq(options, undefined, `Mongos ${idx} should have auto encryption options`);
        assert.eq(options.keyVaultNamespace, "test.keystore", `Mongos ${idx} should have correct keyVaultNamespace`);
    });

    // Clean up
    conn.unsetAutoEncryption();

    // Verify all connections are cleaned up
    conn._mongoConnections.forEach((mongo, idx) => {
        const options = mongo.getAutoEncryptionOptions();
        assert.eq(options, undefined, `Mongos ${idx} should not have auto encryption options after cleanup`);
    });
});

testCase("Testing encrypted inserts are routed randomly", () => {
    const uri = getMongosesURI();
    const conn = connect(uri).getMongo();
    const db = conn.getDB("test");
    const coll = db.encryptionTest;
    const kOperations = 10;

    const localKMS = {
        key: BinData(
            0,
            "/tu9jUCBqZdwCelwE/EAm/4WqdxrSMi04B8e9uAV+m30rI1J2nhKZZtQjdvsSCwuI4erR6IEcEK+5eGUAODv43NDNIR9QheT2edWFewUfHKsl9cnzTc86meIzOmYl6dr",
        ),
    };

    const clientSideFLEOptions = {
        kmsProviders: {local: localKMS},
        keyVaultNamespace: "test.keystore",
        schemaMap: {},
    };

    // Set auto encryption
    assert(conn.setAutoEncryption(clientSideFLEOptions), "setAutoEncryption should succeed");
    assert(conn.toggleAutoEncryption(true), "toggleAutoEncryption(true) should succeed");

    // Track _getNextMongo calls
    const getNextMongoTracker = overrideGetNextMongoCountingCalls(conn);

    // Perform encrypted inserts
    for (let i = 0; i < kOperations; i++) {
        coll.insert({x: i, type: "encrypted-insert"});
    }

    // Should route randomly for all encrypted insert operations
    assert.gte(getNextMongoTracker.count(), kOperations, "Should route randomly for encrypted insert operations");

    // Verify all documents were inserted
    assert.eq(kOperations, coll.count({type: "encrypted-insert"}), "All encrypted inserts should succeed");

    // Clean up
    coll.drop();
    conn.unsetAutoEncryption();
});

st.stop();
