// Tests for $out against a stale mongos with combinations of sharded/unsharded source and target
// collections.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const st = new ShardingTest({
        shards: 2,
        mongos: 4,
    });

    const freshMongos = st.s0.getDB(jsTestName());
    const staleMongosSource = st.s1.getDB(jsTestName());
    const staleMongosTarget = st.s2.getDB(jsTestName());
    const staleMongosBoth = st.s3.getDB(jsTestName());

    const sourceColl = freshMongos.getCollection("source");
    const targetColl = freshMongos.getCollection("target");

    // Enable sharding on the test DB and ensure its primary is shard 0.
    assert.commandWorked(
        staleMongosSource.adminCommand({enableSharding: staleMongosSource.getName()}));
    st.ensurePrimaryShard(staleMongosSource.getName(), st.rs0.getURL());

    // Shards the collection 'coll' through 'mongos'.
    function shardCollWithMongos(mongos, coll) {
        coll.drop();
        // Shard the given collection on _id, split the collection into 2 chunks: [MinKey, 0) and
        // [0, MaxKey), then move the [0, MaxKey) chunk to shard 1.
        assert.commandWorked(
            mongos.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(mongos.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
        assert.commandWorked(mongos.adminCommand(
            {moveChunk: coll.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));
    }

    // Configures the two mongos, staleMongosSource and staleMongosTarget, to be stale on the source
    // and target
    // collections, respectively. For instance, if 'shardedSource' is true then staleMongosSource
    // will
    // believe that the source collection is unsharded.
    function setupStaleMongos({shardedSource, shardedTarget}) {
        // Initialize both mongos to believe the collections are unsharded.
        sourceColl.drop();
        targetColl.drop();
        assert.commandWorked(staleMongosSource[sourceColl.getName()].insert(
            {_id: "insert when unsharded (source)"}));
        assert.commandWorked(staleMongosSource[targetColl.getName()].insert(
            {_id: "insert when unsharded (source)"}));
        assert.commandWorked(staleMongosTarget[sourceColl.getName()].insert(
            {_id: "insert when unsharded (target)"}));
        assert.commandWorked(staleMongosTarget[targetColl.getName()].insert(
            {_id: "insert when unsharded (target)"}));

        if (shardedSource) {
            // Shard the source collection through the staleMongosTarget mongos, keeping the
            // staleMongosSource
            // unaware.
            shardCollWithMongos(staleMongosTarget, sourceColl);
        } else {
            // Shard the collection through staleMongosSource.
            shardCollWithMongos(staleMongosSource, sourceColl);

            // Then drop the collection, but do not recreate it yet as that will happen on the next
            // insert later in the test.
            sourceColl.drop();
        }

        if (shardedTarget) {
            // Shard the target collection through the staleMongosSource mongos, keeping the
            // staleMongosTarget
            // unaware.
            shardCollWithMongos(staleMongosSource, targetColl);
        } else {
            // Shard the collection through staleMongosTarget.
            shardCollWithMongos(staleMongosTarget, targetColl);

            // Then drop the collection, but do not recreate it yet as that will happen on the next
            // insert later in the test.
            targetColl.drop();
        }
    }

    // Runs a $out with the given mode against each mongos in 'mongosList'. This method will wrap
    // 'mongosList' into a list if it is not an array.
    function runOutTest(mode, mongosList) {
        if (!(mongosList instanceof Array)) {
            mongosList = [mongosList];
        }

        mongosList.forEach(mongos => {
            targetColl.remove({});
            sourceColl.remove({});
            // Insert several documents into the source and target collection without any conflicts.
            // Note that the chunk split point is at {_id: 0}.
            assert.commandWorked(sourceColl.insert([{_id: -1}, {_id: 0}, {_id: 1}]));
            assert.commandWorked(targetColl.insert([{_id: -2}, {_id: 2}, {_id: 3}]));

            mongos[sourceColl.getName()].aggregate(
                [{$out: {to: targetColl.getName(), mode: mode}}]);
            assert.eq(mode == "replaceCollection" ? 3 : 6, targetColl.find().itcount());
        });
    }

    ["replaceDocuments", "insertDocuments"].forEach(mode => {
        jsTestLog("Testing mode " + mode);

        // For each mode, test the following scenarios:
        // * Both the source and target collections are sharded.
        // * Both the source and target collections are unsharded.
        // * Source collection is sharded and the target collection is unsharded.
        // * Source collection is unsharded and the target collection is sharded.
        setupStaleMongos({shardedSource: false, shardedTarget: false});
        runOutTest(mode, [staleMongosSource, staleMongosTarget]);

        setupStaleMongos({shardedSource: true, shardedTarget: true});
        runOutTest(mode, [staleMongosSource, staleMongosTarget]);

        setupStaleMongos({shardedSource: true, shardedTarget: false});
        runOutTest(mode, [staleMongosSource, staleMongosTarget]);

        setupStaleMongos({shardedSource: false, shardedTarget: true});
        runOutTest(mode, [staleMongosSource, staleMongosTarget]);

        //
        // The remaining tests run against a mongos which is stale with respect to BOTH the source
        // and target collections.
        //
        const sourceCollStale = staleMongosBoth.getCollection(sourceColl.getName());
        const targetCollStale = staleMongosBoth.getCollection(targetColl.getName());

        //
        // 1. Both source and target collections are sharded.
        //
        sourceCollStale.drop();
        targetCollStale.drop();

        // Insert into both collections through the stale mongos such that it believes the
        // collections exist and are unsharded.
        assert.commandWorked(sourceCollStale.insert({_id: 0}));
        assert.commandWorked(targetCollStale.insert({_id: 0}));

        shardCollWithMongos(freshMongos, sourceColl);
        shardCollWithMongos(freshMongos, targetColl);

        // Test against the stale mongos, which believes both collections are unsharded.
        runOutTest(mode, staleMongosBoth);

        //
        // 2. Both source and target collections are unsharded.
        //
        sourceColl.drop();
        targetColl.drop();

        // The collections were both dropped through a different mongos, so the stale mongos still
        // believes that they're sharded.
        runOutTest(mode, staleMongosBoth);

        //
        // 3. Source collection is sharded and target collection is unsharded.
        //
        sourceCollStale.drop();

        // Insert into the source collection through the stale mongos such that it believes the
        // collection exists and is unsharded.
        assert.commandWorked(sourceCollStale.insert({_id: 0}));

        // Shard the source collection through the fresh mongos.
        shardCollWithMongos(freshMongos, sourceColl);

        // Shard the target through the stale mongos, but then drop and recreate it as unsharded
        // through a different mongos.
        shardCollWithMongos(staleMongosBoth, targetColl);
        targetColl.drop();

        // At this point, the stale mongos believes the source collection is unsharded and the
        // target collection is sharded when in fact the reverse is true.
        runOutTest(mode, staleMongosBoth);

        //
        // 4. Source collection is unsharded and target collection is sharded.
        //
        sourceCollStale.drop();
        targetCollStale.drop();

        // Insert into the target collection through the stale mongos such that it believes the
        // collection exists and is unsharded.
        assert.commandWorked(targetCollStale.insert({_id: 0}));

        shardCollWithMongos(freshMongos, targetColl);

        // Shard the source through the stale mongos, but then drop and recreate it as unsharded
        // through a different mongos.
        shardCollWithMongos(staleMongosBoth, sourceColl);
        sourceColl.drop();

        // At this point, the stale mongos believes the source collection is sharded and the target
        // collection is unsharded when in fact the reverse is true.
        runOutTest(mode, staleMongosBoth);
    });

    // Mode "replaceCollection" is special because the aggregation will fail if the target
    // collection is sharded.
    setupStaleMongos({shardedSource: false, shardedTarget: false});
    runOutTest("replaceCollection", [staleMongosSource, staleMongosTarget]);

    setupStaleMongos({shardedSource: true, shardedTarget: true});
    assert.eq(assert.throws(() => runOutTest("replaceCollection", staleMongosSource)).code, 28769);
    assert.eq(assert.throws(() => runOutTest("replaceCollection", staleMongosTarget)).code, 17017);

    setupStaleMongos({shardedSource: true, shardedTarget: false});
    runOutTest("replaceCollection", [staleMongosSource, staleMongosTarget]);

    setupStaleMongos({shardedSource: false, shardedTarget: true});
    assert.eq(assert.throws(() => runOutTest("replaceCollection", staleMongosSource)).code, 28769);
    assert.eq(assert.throws(() => runOutTest("replaceCollection", staleMongosTarget)).code, 28769);

    st.stop();
}());
