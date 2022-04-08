// Test setUserWriteBlockMode command.
//
// @tags: [
//   creates_and_authenticates_user,
//   requires_auth,
//   requires_fcv_60,
//   requires_non_retryable_commands,
//   requires_persistence,
//   requires_replication,
//   featureFlagUserWriteBlocking,
// ]

(function() {
'use strict';

load("jstests/noPassthrough/libs/user_write_blocking.js");
load("jstests/libs/fail_point_util.js");  // For configureFailPoint

const {
    WriteBlockState,
    ShardingFixture,
    ReplicaFixture,
    bypassUser,
    noBypassUser,
    password,
    keyfile
} = UserWriteBlockHelpers;

// For this test to work, we expect the state of the connection to be maintained as:
// One db: "testSetUserWriteBlockMode1"
// Two collections: db.coll1, db.coll2
// One index: "index" w/ pattern {"b": 1} on db.coll1
// One document: {a: 0, b: 0} on db.coll1
const dbName = "testSetUserWriteBlockMode1";
const coll1Name = "coll1";
const coll2Name = "coll2";
const indexName = "index";

function setupForTesting(conn) {
    const db = conn.getDB(dbName);
    assert.commandWorked(db.createCollection(coll1Name));
    assert.commandWorked(db.createCollection(coll2Name));
    const coll1 = db[coll1Name];
    coll1.insert({a: 0, b: 0});
    coll1.createIndex({"b": 1}, {"name": indexName});
}

function testCheckedOps(conn, shouldSucceed, expectedFailure) {
    const transientDbName = "transientDB";
    const transientCollNames = ["tc0", "tc1", "tc2"];
    const transientIndexName = "transientIndex";

    const db = conn.getDB(dbName);
    const coll1 = db[coll1Name];
    const coll2 = db[coll2Name];

    // Ensure we successfully maintained state from last run.
    function assertState() {
        assert(Array.contains(conn.getDBNames(), db.getName()));
        assert(!Array.contains(conn.getDBNames(), transientDbName));

        assert(Array.contains(db.getCollectionNames(), coll1.getName()));
        assert(Array.contains(db.getCollectionNames(), coll2.getName()));
        for (let tName of transientCollNames) {
            assert(!Array.contains(db.getCollectionNames(), tName));
        }

        const indexes = coll1.getIndexes();
        assert.eq(undefined, indexes.find(i => i.name === transientIndexName));
        assert.neq(undefined, indexes.find(i => i.name === indexName));

        assert.eq(1, coll1.find({a: 0, b: 0}).count());
        assert.eq(0, coll1.find({a: 1}).count());
    }
    assertState();

    if (shouldSucceed) {
        // Test CUD
        assert.commandWorked(coll1.insert({a: 1}));
        assert.eq(1, coll1.find({a: 1}).count());
        assert.commandWorked(coll1.update({a: 1}, {a: 1, b: 2}));
        assert.eq(1, coll1.find({a: 1, b: 2}).count());
        assert.commandWorked(coll1.remove({a: 1}));

        // Test create index on empty and non-empty colls, collMod, drop index.
        assert.commandWorked(coll1.createIndex({"a": 1}, {"name": transientIndexName}));
        assert.commandWorked(db.runCommand(
            {collMod: coll1Name, "index": {"keyPattern": {"a": 1}, expireAfterSeconds: 200}}));
        assert.commandWorked(coll1.dropIndex({"a": 1}));
        assert.commandWorked(coll2.createIndex({"a": 1}, {"name": transientIndexName}));
        assert.commandWorked(coll2.dropIndex({"a": 1}));

        // Test create, rename (both to a non-existent and an existing target), drop collection.
        assert.commandWorked(db.createCollection(transientCollNames[0]));
        assert.commandWorked(db.createCollection(transientCollNames[1]));
        assert.commandWorked(db[transientCollNames[0]].renameCollection(transientCollNames[2]));
        assert.commandWorked(
            db[transientCollNames[2]].renameCollection(transientCollNames[1], true));
        assert(db[transientCollNames[1]].drop());

        // Test dropping a (non-empty) database.
        const transientDb = conn.getDB(transientDbName);
        assert.commandWorked(transientDb.createCollection("coll"));
        assert.commandWorked(transientDb.dropDatabase());
    } else {
        // Test CUD
        assert.commandFailedWithCode(coll1.insert({a: 1}), expectedFailure);
        assert.commandFailedWithCode(coll1.update({a: 0, b: 0}, {a: 1}), expectedFailure);
        assert.commandFailedWithCode(coll1.remove({a: 0, b: 0}), expectedFailure);

        // Test create, collMod, drop index.
        assert.commandFailedWithCode(coll1.createIndex({"a": 1}, {"name": transientIndexName}),
                                     expectedFailure);
        assert.commandFailedWithCode(
            db.runCommand(
                {collMod: coll1Name, "index": {"keyPattern": {"b": 1}, expireAfterSeconds: 200}}),
            expectedFailure);
        assert.commandFailedWithCode(coll1.dropIndex({"b": 1}), expectedFailure);
        assert.commandFailedWithCode(coll2.createIndex({"a": 1}, {"name": transientIndexName}),
                                     expectedFailure);

        // Test create, rename (both to a non-existent and an existing target), drop collection.
        assert.commandFailedWithCode(db.createCollection(transientCollNames[0]), expectedFailure);
        assert.commandFailedWithCode(coll2.renameCollection(transientCollNames[1]),
                                     expectedFailure);
        assert.commandFailedWithCode(coll2.renameCollection(coll1Name, true), expectedFailure);
        assert.commandFailedWithCode(db.runCommand({drop: coll2Name}), expectedFailure);

        // Test dropping a database.
        assert.commandFailedWithCode(db.dropDatabase(), expectedFailure);
    }

    // Ensure we successfully maintained state on this run.
    assertState();
}

function runTest(fixture) {
    fixture.asAdmin(({conn}) => setupForTesting(conn));

    fixture.assertWriteBlockMode(WriteBlockState.DISABLED);

    // Ensure that without setUserWriteBlockMode, both users are privileged for CUD ops
    fixture.asAdmin(({conn}) => testCheckedOps(conn, true));

    fixture.asUser(({conn}) => {
        testCheckedOps(conn, true);

        // Ensure that the non-privileged user cannot run setUserWriteBlockMode
        assert.commandFailedWithCode(
            conn.getDB('admin').runCommand({setUserWriteBlockMode: 1, global: true}),
            ErrorCodes.Unauthorized);
    });

    fixture.assertWriteBlockMode(WriteBlockState.DISABLED);
    fixture.enableWriteBlockMode();
    fixture.assertWriteBlockMode(WriteBlockState.ENABLED);

    // Now with setUserWriteBlockMode enabled, ensure that only the bypassUser can CUD
    fixture.asAdmin(({conn}) => testCheckedOps(conn, true));
    fixture.asUser(({conn}) => testCheckedOps(conn, false, ErrorCodes.UserWritesBlocked));

    // Restarting the cluster has no impact, as write block state is durable
    fixture.restart();

    fixture.assertWriteBlockMode(WriteBlockState.ENABLED);

    fixture.asAdmin(({conn}) => {
        testCheckedOps(conn, true);
    });
    fixture.asUser(({conn}) => {
        testCheckedOps(conn, false, ErrorCodes.UserWritesBlocked);
    });

    // Now disable userWriteBlockMode and ensure both users can CUD again
    fixture.disableWriteBlockMode();
    fixture.assertWriteBlockMode(WriteBlockState.DISABLED);

    fixture.asAdmin(({conn}) => testCheckedOps(conn, true));
    fixture.asUser(({conn}) => testCheckedOps(conn, true));

    // Test that enabling write blocking while there is an active index build will cause the index
    // build to fail.
    fixture.asUser(({conn}) => {
        const db = conn.getDB(jsTestName());
        assert.commandWorked(db.createCollection("test"));
        assert.commandWorked(db.test.insert({"a": 2}));
    });

    const fp = fixture.setFailPoint('hangAfterInitializingIndexBuild');
    // This createIndex should hang at setup, and when it resumes, userWriteBlockMode will be
    // enabled and it should eventually fail.
    const parallelShell = fixture.runInParallelShell(false /* asAdmin */,
                                                     `({conn}) => { 
            assert.commandFailedWithCode(
                conn.getDB(jsTestName()).test.createIndex({"a": 1}, {"name": "index"}),
                ErrorCodes.UserWritesBlocked);
        }`);

    // Let index build progress to the point where it hits the failpoint.
    fp.wait();
    fixture.enableWriteBlockMode();
    fp.off();
    parallelShell();

    // Ensure index was not created.
    fixture.asAdmin(
        ({conn}) => assert.eq(
            undefined, conn.getDB(jsTestName()).test.getIndexes().find(i => i.name === "index")));

    if (fixture.takeGlobalLock) {
        // Test that serverStatus will produce WriteBlockState.UNKNOWN when the global lock is held.
        let globalLock = fixture.takeGlobalLock();
        try {
            fixture.assertWriteBlockMode(WriteBlockState.UNKNOWN);
        } finally {
            globalLock.unlock();
        }
    }
}

{
    // Validate that setting user write blocking fails on standalones
    const conn = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
    const admin = conn.getDB("admin");
    assert.commandWorked(admin.runCommand(
        {createUser: "root", pwd: "root", roles: [{role: "__system", db: "admin"}]}));
    assert(admin.auth("root", "root"));

    assert.commandFailedWithCode(admin.runCommand({setUserWriteBlockMode: 1, global: true}),
                                 ErrorCodes.IllegalOperation);
    MongoRunner.stopMongod(conn);
}

// Test on a replset
const rst = new ReplicaFixture();
runTest(rst);
rst.stop();

// Test on a sharded cluster
const st = new ShardingFixture();
runTest(st);
st.stop();
})();
