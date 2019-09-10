/*
 * Tests that we can seed a secondary in a replica set, even if the config.system.sessions and
 * config.transactions tables have been dropped.
 *
 * This tests the scenario described in SERVER-42706.
 * @tags: [requires_persistence, requires_journaling, requires_replication]
 */

(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");
    load("jstests/replsets/rslib.js");
    load("jstests/libs/uuid_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    // This test needs to control the number of sessions which get started on the
    // server, so disable implicit sessions.
    TestData.disableImplicitSessions = true;

    const name = 'seed_secondary_without_sessions_table';
    const dbName = name;
    const collName = 'foo';
    const sessionsCollName = 'system.sessions';
    const txnCollName = 'transactions';

    function sessionsColl(node) {
        return node.getDB('config')[sessionsCollName];
    }

    function txnColl(node) {
        return node.getDB('config')[txnCollName];
    }

    function assertCollSize(coll, size) {
        const docs = coll.find().toArray();
        assert.eq(docs.length, size, () => tojson(docs));
    }

    const rst = new ReplSetTest({
        nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        // Allow sessions to be reaped without waiting an entire minute.
        nodeOptions: {setParameter: {TransactionRecordMinimumLifetimeMinutes: -1}},
    });

    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    let seed = rst.getSecondary();

    const session1 = primary.startSession({retryWrites: true});
    const testDB1 = session1.getDatabase(dbName);
    const session2 = primary.startSession({retryWrites: true});
    const testDB2 = session2.getDatabase(dbName);

    jsTestLog("Check no sessions or transactions tables");
    assertCollSize(sessionsColl(primary), 0);
    assertCollSize(txnColl(primary), 0);
    assertCollSize(sessionsColl(seed), 0);
    assertCollSize(txnColl(seed), 0);

    assert.commandWorked(testDB1[collName].insert({a: 1}));
    assert.commandWorked(testDB2[collName].insert({a: 2}));
    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    rst.awaitReplication();

    jsTestLog("Check sessions and transactions tables exist");
    assertCollSize(sessionsColl(primary), 2);
    assertCollSize(txnColl(primary), 2);
    assertCollSize(sessionsColl(seed), 2);
    assertCollSize(txnColl(seed), 2);

    const sessionsCollUUID = getUUIDFromListCollections(seed.getDB('config'), sessionsCollName);
    const txnCollUUID = getUUIDFromListCollections(seed.getDB('config'), txnCollName);
    jsTestLog(
        `config.system.sessions uuid: ${sessionsCollUUID}, config.transactions uuid: ${txnCollUUID}`);

    jsTestLog("Restarting seed node as standalone with disableLogicalSessionCacheRefresh on");
    seed = rst.restart(seed,
                       {noReplSet: true, setParameter: {disableLogicalSessionCacheRefresh: true}});
    reconnect(seed);

    jsTestLog("Checking standalone contents, the stable checkpoint should be up to date");
    assertCollSize(sessionsColl(seed), 2);
    assertCollSize(txnColl(seed), 2);

    jsTestLog("Dropping sessions and transactions tables");
    sessionsColl(seed).drop();
    txnColl(seed).drop();
    assertCollSize(sessionsColl(seed), 0);
    assertCollSize(txnColl(seed), 0);

    jsTestLog("Dropping local database on seed node");
    const seedLocal = seed.getDB('local');
    const lastOplogEntry = getLatestOp(seed);
    const config = seedLocal.system.replset.findOne();
    assert.commandWorked(seedLocal.runCommand({dropDatabase: 1}));

    jsTestLog("Populating replication collections in the seed node");
    seedLocal.createCollection("oplog.rs", {capped: true, size: 1000000 /* 1MB */});
    assert.commandWorked(seedLocal.oplog.rs.insert({
        ts: lastOplogEntry.ts,
        t: lastOplogEntry.t,
        h: lastOplogEntry.h,
        v: lastOplogEntry.v,
        op: 'n',
        ns: 'dummy.coll',
        o: {msg: "Dummy Seed"}
    }));
    assert.commandWorked(seedLocal.system.replset.insert(config));

    jsTestLog("Recreating sessions table");
    let op = {
        op: "c",
        ns: "config.$cmd",
        ui: sessionsCollUUID,
        o: {
            create: sessionsCollName,
            // The _id index is created as v: 1 by default when created via applyOps.
            idIndex: {key: {_id: 1}, ns: "config." + sessionsCollName, name: "_id_", v: 2}
        }
    };
    let cmd = {applyOps: [op]};
    jsTestLog(`Applying ${tojson(cmd)}`);
    assert.commandWorked(seed.adminCommand(cmd));
    assert.commandWorked(sessionsColl(seed).createIndex(
        {lastUse: 1}, {name: 'lsidTTLIndex', expireAfterSeconds: 1800}));

    jsTestLog("Recreating transactions table");
    op = {
        op: "c",
        ns: "config.$cmd",
        ui: txnCollUUID,
        o: {
            create: txnCollName,
            // The _id index is created as v: 1 by default when created via applyOps.
            idIndex: {key: {_id: 1}, ns: "config." + txnCollName, name: "_id_", v: 2}
        }
    };
    cmd = {applyOps: [op]};
    jsTestLog(`Applying ${tojson(cmd)}`);
    assert.commandWorked(seed.adminCommand(cmd));

    jsTestLog("Restarting seed node in replset");
    seed = rst.restart(seed, {noReplSet: false});
    reconnect(seed);
    seed.setSlaveOk();
    rst.waitForState(seed, ReplSetTest.State.SECONDARY);

    assertCollSize(sessionsColl(primary), 2);
    assertCollSize(txnColl(primary), 2);
    assertCollSize(sessionsColl(seed), 0);
    assertCollSize(txnColl(seed), 0);

    jsTestLog("Delete the first session");
    printjson(primary.getDB('config')['system.sessions'].find().toArray());
    const removeRes = assert.commandWorked(sessionsColl(primary).remove(
        {'_id.id': session1.getSessionId().id}, {writeConcern: {w: 3}}));
    assert.eq(1, removeRes.nRemoved);

    assertCollSize(sessionsColl(primary), 1);
    assertCollSize(txnColl(primary), 2);
    assertCollSize(sessionsColl(seed), 0);
    assertCollSize(txnColl(seed), 0);

    jsTestLog("Update the most-recent used time of the second session");
    const session2InfoBefore =
        sessionsColl(primary).find({'_id.id': session2.getSessionId().id}).toArray();
    assert.eq(session2InfoBefore.length, 1);
    const session2LastUseBefore = session2InfoBefore[0].lastUse;

    assert.commandWorked(testDB2[collName].insert({a: 3}));
    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    rst.awaitReplication();

    assertCollSize(sessionsColl(primary), 1);
    assertCollSize(txnColl(primary), 2);
    assertCollSize(sessionsColl(seed), 1);
    assertCollSize(txnColl(seed), 1);

    const session2InfoAfter =
        sessionsColl(primary).find({'_id.id': session2.getSessionId().id}).toArray();
    assert.eq(session2InfoAfter.length, 1);
    const session2LastUseAfter = session2InfoAfter[0].lastUse;

    assert.gt(session2LastUseAfter, session2LastUseBefore);

    jsTestLog("Start a third session");
    const testDB3 = primary.startSession({retryWrites: true}).getDatabase(dbName);
    assert.commandWorked(testDB3[collName].insert({a: 4}));
    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    rst.awaitReplication();

    assertCollSize(sessionsColl(primary), 2);
    assertCollSize(txnColl(primary), 3);
    assertCollSize(sessionsColl(seed), 2);
    assertCollSize(txnColl(seed), 2);

    jsTestLog("Reap the deleted session and ensure the transactions collection " +
              "entry is successfully deleted as well");
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    rst.awaitReplication();

    assertCollSize(sessionsColl(primary), 2);
    assertCollSize(txnColl(primary), 2);
    assertCollSize(sessionsColl(seed), 2);
    assertCollSize(txnColl(seed), 2);

    rst.stopSet();
})();