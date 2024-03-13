/*
 * Tests that bulk write operations succeed on a two shard cluster with both
 * sharded and unsharded data.
 * @tags: [multiversion_incompatible, requires_fcv_80]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getDBNameAndCollNameFromFullNamespace} from "jstests/libs/namespace_utils.js";
import {
    moveDatabaseAndUnshardedColls
} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";

function bulkWriteBasicTest(ordered) {
    jsTestLog(`Running bulkWrite command sharding test with ordered: ${ordered}`);
    const st = new ShardingTest({
        shards: 2,
        mongos: 2,
        config: 1,
        rs: {nodes: 1},
        mongosOptions: {setParameter: {logComponentVerbosity: tojson({sharding: 4})}}
    });

    function getCollection(ns) {
        const [dbName, collName] = getDBNameAndCollNameFromFullNamespace(ns);
        return st.s0.getDB(dbName)[collName];
    }

    const banana = "test.banana";
    const orange = "test2.orange";

    const staleConfigBananaLog = /7279201.*Noting stale config response.*banana/;
    const staleConfigOrangeLog = /7279201.*Noting stale config response.*orange/;
    const staleDbTest2Log = /7279202.*Noting stale database response.*test2/;

    jsTestLog("Case 1: Collection does't exist yet.");
    // Case 1: The collection doesn't exist yet. This results in a CannotImplicitlyCreateCollection
    // error on the shards and consequently mongos and the shards must all refresh. Then mongos
    // needs to retry the bulk operation.

    // Connect via the first mongos. We do this so that the second mongos remains unused until
    // a later test case.
    const db_s0 = st.s0.getDB("test");
    assert.commandWorked(db_s0.adminCommand({
        bulkWrite: 1,
        ops: [{insert: 0, document: {a: 0}}, {insert: 0, document: {a: 1}}],
        ordered,
        nsInfo: [{ns: banana}]
    }));

    let insertedDocs = getCollection(banana).find({}).toArray();
    assert.eq(2, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);
    assert(checkLog.checkContainsOnce(st.s0, staleConfigBananaLog));
    if (!ordered) {
        // Check that the error for the 0th op was duplicated and used for the 1st op as well.
        assert(
            checkLog.checkContainsOnce(st.s0, /7695304.*Duplicating the error.*opIdx":1.*banana/));
    }

    jsTestLog("Case 2: The collection exists for some of writes, but not for others.");
    assert.commandWorked(db_s0.adminCommand({
        bulkWrite: 1,
        ops: [
            {insert: 0, document: {a: 2}},
            {insert: 1, document: {a: 0}},
            {insert: 0, document: {a: 3}}
        ],
        ordered,
        nsInfo: [{ns: banana}, {ns: orange}]
    }));

    insertedDocs = getCollection(banana).find({}).toArray();
    assert.eq(4, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);
    insertedDocs = getCollection(orange).find({}).toArray();
    assert.eq(1, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);
    assert(checkLog.checkContainsOnce(st.s0, staleConfigOrangeLog));

    const db_s1 = st.s1.getDB("test");

    const isTrackUnshardedUponCreationEnabled = FeatureFlagUtil.isPresentAndEnabled(
        st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");

    // Case 3: Move the 'test2' DB back and forth across shards. This will result in bulkWrite
    // getting a StaleDbVersion error. We run this on s1 so s0 doesn't know about the change.
    moveDatabaseAndUnshardedColls(st.s1.getDB('test2'), st.shard0.shardName);
    moveDatabaseAndUnshardedColls(st.s1.getDB('test2'), st.shard1.shardName);

    // Now run the bulk write command on s0.
    assert.commandWorked(db_s0.adminCommand(
        {bulkWrite: 1, ops: [{insert: 0, document: {a: 3}}], nsInfo: [{ns: orange}]}));
    insertedDocs = getCollection(orange).find({}).toArray();
    assert.eq(2, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);

    // TODO (SERVER-87807): Skip this check also for uponMoveCollection feature flag
    if (!isTrackUnshardedUponCreationEnabled) {
        assert(checkLog.checkContainsOnce(st.s0, staleDbTest2Log));
    }

    jsTestLog("Case 4: The collection is sharded and lives on both shards.");
    // Case 4: Shard the collection and manually move chunks so that they live on
    // both shards. We stop the balancer as well. We do all of this on s0, but then,
    // we run a bulk write command through the s1 that has a stale view of the cluster.
    assert.commandWorked(st.stopBalancer());

    jsTestLog("Shard the collection.");
    assert.commandWorked(getCollection(banana).createIndex({a: 1}));
    assert.commandWorked(db_s0.adminCommand({enableSharding: "test"}));
    assert.commandWorked(db_s0.adminCommand({shardCollection: banana, key: {a: 1}}));

    jsTestLog("Create chunks, then move them.");
    assert.commandWorked(db_s0.adminCommand({split: banana, middle: {a: 2}}));
    assert.commandWorked(
        db_s0.adminCommand({moveChunk: banana, find: {a: 0}, to: st.shard0.shardName}));
    assert.commandWorked(
        db_s0.adminCommand({moveChunk: banana, find: {a: 3}, to: st.shard1.shardName}));

    jsTestLog("Running bulk write command.");
    assert.commandWorked(db_s1.adminCommand({
        bulkWrite: 1,
        ops: [
            {insert: 0, document: {a: -1}},
            {insert: 1, document: {a: 1}},
            {insert: 0, document: {a: 4}}
        ],
        ordered,
        nsInfo: [{ns: banana}, {ns: orange}]
    }));

    insertedDocs = getCollection(banana).find({}).toArray();
    assert.eq(6, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);
    insertedDocs = getCollection(orange).find({}).toArray();
    assert.eq(3, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);

    // Checklog doesn't work in this case because mongos may refresh its routing info before
    // runningthe bulkWrite command, which means that the logs we're looking for won't get printed.
    // However, since the number of documents matched up in the asserts above, it means that mongos
    // must've correctly routed the bulkWrite command.

    if (!ordered) {
        jsTestLog("Case 5: Remaining operations executed on non-staleness error.");
        // On errors like a DuplicateKeyError, execution of the bulkWrite command extends beyond
        // the erroring operation.
        // So overall, we expect:
        // 1) bulkWrite command sent
        // 2) Collection mango doesn't exist yet. CannotImplicitlyCreateCollection error returned.
        // 3) CannotImplicitlyCreateCollection error duplicated for all operations.
        // 4) Retry operation after creating collection and refreshing
        // 5) Operations 0, 1 (DuplicateKeyError), and 2 go through. Operation 3 hits a
        // CannotImplicitlyCreateCollection error.
        // 6) Retry operation after creating second collection and refreshing
        // 7) And finally the operation is retried and succeeds.
        const mango = 'test3.mango';
        const strawberry = 'test3.strawberry';
        assert.commandWorked(db_s0.adminCommand({
            bulkWrite: 1,
            ops: [
                {insert: 0, document: {_id: 1}},
                {insert: 0, document: {_id: 1}},  // DuplicateKeyError
                {insert: 0, document: {a: 1}},
                {insert: 1, document: {a: 1}},
                {insert: 1, document: {a: 2}}
            ],
            ordered,
            nsInfo: [{ns: mango}, {ns: strawberry}]
        }));
        // The fact that more than one document was inserted proves that the bulkWrite advanced
        // past op 1's DuplicateKeyError.
        insertedDocs = getCollection(mango).find({}).toArray();
        assert.eq(2, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);
        insertedDocs = getCollection(strawberry).find({}).toArray();
        assert.eq(2, insertedDocs.length, `Inserted docs: '${tojson(insertedDocs)}'`);

        // The CannotImplicitlyCreateCollection error on op 0 should have been duplicated to all
        // operations.
        for (let i = 1; i < 5; i++) {
            assert(checkLog.checkContainsOnce(
                st.s0, new RegExp(`7695304.*Duplicating the error.*opIdx":${i}.*mango`)));
        }

        // The CannotImplicitlyCreateCollection error on op 3 should have been duplicated to op 4.
        assert(
            checkLog.checkContainsOnce(
                st.s0, /8037206.*Noting cannotImplicitlyCreateCollection response.*strawberry/) ||
            checkLog.checkContainsOnce(st.s0, /7279201.*Noting stale config response.*strawberry/));
        assert(checkLog.checkContainsOnce(st.s0,
                                          /7695304.*Duplicating the error.*opIdx":4.*strawberry/));
    }

    st.stop();
}

bulkWriteBasicTest(true);
bulkWriteBasicTest(false);
