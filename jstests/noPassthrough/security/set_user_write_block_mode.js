// Test setUserWriteBlockMode command.
//
// @tags: [
//   creates_and_authenticates_user,
//   requires_auth,
//   requires_fcv_60,
//   requires_non_retryable_commands,
//   requires_persistence,
//   requires_replication,
// ]

import {UserWriteBlockHelpers} from "jstests/noPassthrough/libs/user_write_blocking.js";

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
// Three collections:
//       * set_user_write_block_mode_coll1
//       * set_user_write_block_mode_coll2
//       * set_user_write_block_mode_coll3
// One index: "index" w/ pattern {"b": 1} on db.coll1
// One document: {a: 0, b: 0} on db.coll1
const dbName = "testSetUserWriteBlockMode1";
const coll1Name = jsTestName() + "_coll1";
const coll2Name = jsTestName() + "_coll2";
const coll3Name = jsTestName() + "_coll3";
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

// Checks that an unprivileged user's operations can be logged on the profiling collection.
function testProfiling(fixture) {
    const collName = 'foo';
    fixture.asAdmin(({db}) => {
        assert.commandWorked(db[collName].insert({x: 1}));
    });

    // Enable profiling.
    const prevProfilingLevel = fixture.setProfilingLevel(2).was;

    // Perform a find() as an unprivileged user.
    const comment = UUID();
    fixture.asUser(({db}) => {
        db[collName].find().comment(comment).itcount();
    });

    // Check that the find() was logged on the profiling collection.
    fixture.asAdmin(({db}) => {
        assert.eq(1, db.system.profile.find({'command.comment': comment}).itcount());
    });

    // Restore the original profiling level.
    fixture.setProfilingLevel(prevProfilingLevel);
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

    // Ensure that attempting to enabling write blocking again is a no-op under various
    // circumstances
    fixture.enableWriteBlockMode();
    fixture.assertWriteBlockMode(WriteBlockState.ENABLED);
    fixture.stepDown();
    fixture.enableWriteBlockMode();
    fixture.assertWriteBlockMode(WriteBlockState.ENABLED);

    // Ensure that profiling works while user writes are blocked.
    testProfiling(fixture);

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

    // Test that enabling write blocking while there is an active index build on a user collection
    // (i.e. non-internal) will cause the index build to fail.
    fixture.asUser(({conn}) => {
        const db = conn.getDB(jsTestName());
        assert.commandWorked(db.createCollection(coll3Name));
        assert.commandWorked(db[coll3Name].insert({"a": 2}));
    });

    fixture.asAdmin(({conn}) => {
        // We use config.system.sessions because it is a collection in an internal DB (config) which
        // is sharded, meaning index builds will be handled by the shard servers. Indexes on
        // non-sharded collections in internal DBs are built by the config server, which doesn't
        // have the UserWriteBlockModeOpObserver installed.
        const config = conn.getDB('config');
        assert.commandWorked(config.createCollection("system.sessions"));
        assert.commandWorked(config.system.sessions.insert({"a": 2}));
    });

    const testParallelShellWithFailpoint = makeParallelShell => {
        const fp = fixture.setFailPoint('hangAfterInitializingIndexBuild');
        const shell = makeParallelShell();
        fp.wait();
        fixture.enableWriteBlockMode();
        fp.off();
        shell();
        fixture.disableWriteBlockMode();
    };

    const indexName = "testIndex";

    // Test that index builds on user collections spawned by both non-privileged and privileged
    // users will be aborted on enableWriteBlockMode.
    testParallelShellWithFailpoint(() => fixture.runInParallelShell(false /* asAdmin */,
                                                                    `({conn}) => { 
        assert.commandFailedWithCode(
            conn.getDB(jsTestName()).${coll3Name}.createIndex({"a": 1}, {"name": "${indexName}"}),
            ErrorCodes.IndexBuildAborted);
    }`));
    testParallelShellWithFailpoint(() => fixture.runInParallelShell(true /* asAdmin */,
                                                                    `({conn}) => {
        assert.commandFailedWithCode(
            conn.getDB(jsTestName()).${coll3Name}.createIndex({"a": 1}, {"name": "${indexName}"}),
            ErrorCodes.IndexBuildAborted);
    }`));

    // Test that index builds on non-user (internal collections) won't be aborted on
    // enableWriteBlockMode.
    testParallelShellWithFailpoint(() => fixture.runInParallelShell(true /* asAdmin */,
                                                                    `({conn}) => {
        assert.commandWorked(
            conn.getDB('config').system.sessions.createIndex(
                {"a": 1}, {"name": "${indexName}"}));
    }`));

    // Ensure index was not successfully created on user db, but was on internal db.
    fixture.asAdmin(({conn}) => {
        assert.eq(undefined,
                  conn.getDB(jsTestName()).coll3Name.getIndexes().find(i => i.name === indexName));
        assert.neq(
            undefined,
            conn.getDB('config').system.sessions.getIndexes().find(i => i.name === indexName));
    });

    // Test that index builds which hang before commit will block activation of
    // enableWriteBlockMode.
    {
        const fp = fixture.setFailPoint("hangIndexBuildBeforeCommit");
        const waitIndexBuild = fixture.runInParallelShell(true /* asAdmin */,
                                                                    `({conn}) => { 
            assert.commandWorked(
                conn.getDB(jsTestName()).${coll3Name}.createIndex({"a": 1}, {"name": "${indexName}"}));
        }`);
        fp.wait();

        const waitWriteBlock = fixture.runInParallelShell(true /* asAdmin */,
                                                          `({conn}) => { 
            assert.commandWorked(
                conn.getDB("admin").runCommand({setUserWriteBlockMode: 1, global: true}));
        }`);
        // Wait, and ensure that the setUserWriteBlockMode has not finished yet (it must wait for
        // the index build to finish).
        sleep(3000);
        fixture.assertWriteBlockMode(UserWriteBlockHelpers.WriteBlockState.DISABLED);

        fp.off();
        waitIndexBuild();
        waitWriteBlock();
        fixture.assertWriteBlockMode(UserWriteBlockHelpers.WriteBlockState.ENABLED);

        fixture.disableWriteBlockMode();
    }

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
// TODO (SERVER-83924) Remove once feature flag checks are not necessary.
if (!st.checkFailPointEnabled("TrackUnshardedCollectionsUponCreation")) {
    runTest(st);
}
st.stop();
