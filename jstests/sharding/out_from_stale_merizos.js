// Tests for $out against a stale merizos with combinations of sharded/unsharded source and target
// collections.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const st = new ShardingTest({
        shards: 2,
        merizos: 4,
    });

    const freshMerizos = st.s0.getDB(jsTestName());
    const staleMerizosSource = st.s1.getDB(jsTestName());
    const staleMerizosTarget = st.s2.getDB(jsTestName());
    const staleMerizosBoth = st.s3.getDB(jsTestName());

    const sourceColl = freshMerizos.getCollection("source");
    const targetColl = freshMerizos.getCollection("target");

    // Enable sharding on the test DB and ensure its primary is shard 0.
    assert.commandWorked(
        staleMerizosSource.adminCommand({enableSharding: staleMerizosSource.getName()}));
    st.ensurePrimaryShard(staleMerizosSource.getName(), st.rs0.getURL());

    // Shards the collection 'coll' through 'merizos'.
    function shardCollWithMerizos(merizos, coll) {
        coll.drop();
        // Shard the given collection on _id, split the collection into 2 chunks: [MinKey, 0) and
        // [0, MaxKey), then move the [0, MaxKey) chunk to shard 1.
        assert.commandWorked(
            merizos.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(merizos.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
        assert.commandWorked(merizos.adminCommand(
            {moveChunk: coll.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));
    }

    // Configures the two merizos, staleMerizosSource and staleMerizosTarget, to be stale on the source
    // and target collections, respectively. For instance, if 'shardedSource' is true then
    // staleMerizosSource will believe that the source collection is unsharded.
    function setupStaleMerizos({shardedSource, shardedTarget}) {
        // Initialize both merizos to believe the collections are unsharded.
        sourceColl.drop();
        targetColl.drop();
        assert.commandWorked(staleMerizosSource[sourceColl.getName()].insert(
            {_id: "insert when unsharded (source)"}));
        assert.commandWorked(staleMerizosSource[targetColl.getName()].insert(
            {_id: "insert when unsharded (source)"}));
        assert.commandWorked(staleMerizosTarget[sourceColl.getName()].insert(
            {_id: "insert when unsharded (target)"}));
        assert.commandWorked(staleMerizosTarget[targetColl.getName()].insert(
            {_id: "insert when unsharded (target)"}));

        if (shardedSource) {
            // Shard the source collection through the staleMerizosTarget merizos, keeping the
            // staleMerizosSource unaware.
            shardCollWithMerizos(staleMerizosTarget, sourceColl);
        } else {
            // Shard the collection through staleMerizosSource.
            shardCollWithMerizos(staleMerizosSource, sourceColl);

            // Then drop the collection, but do not recreate it yet as that will happen on the next
            // insert later in the test.
            sourceColl.drop();
        }

        if (shardedTarget) {
            // Shard the target collection through the staleMerizosSource merizos, keeping the
            // staleMerizosTarget unaware.
            shardCollWithMerizos(staleMerizosSource, targetColl);
        } else {
            // Shard the collection through staleMerizosTarget.
            shardCollWithMerizos(staleMerizosTarget, targetColl);

            // Then drop the collection, but do not recreate it yet as that will happen on the next
            // insert later in the test.
            targetColl.drop();
        }
    }

    // Runs a $out with the given mode against each merizos in 'merizosList'. This method will wrap
    // 'merizosList' into a list if it is not an array.
    function runOutTest(mode, merizosList) {
        if (!(merizosList instanceof Array)) {
            merizosList = [merizosList];
        }

        merizosList.forEach(merizos => {
            targetColl.remove({});
            sourceColl.remove({});
            // Insert several documents into the source and target collection without any conflicts.
            // Note that the chunk split point is at {_id: 0}.
            assert.commandWorked(sourceColl.insert([{_id: -1}, {_id: 0}, {_id: 1}]));
            assert.commandWorked(targetColl.insert([{_id: -2}, {_id: 2}, {_id: 3}]));

            merizos[sourceColl.getName()].aggregate(
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
        setupStaleMerizos({shardedSource: false, shardedTarget: false});
        runOutTest(mode, [staleMerizosSource, staleMerizosTarget]);

        setupStaleMerizos({shardedSource: true, shardedTarget: true});
        runOutTest(mode, [staleMerizosSource, staleMerizosTarget]);

        setupStaleMerizos({shardedSource: true, shardedTarget: false});
        runOutTest(mode, [staleMerizosSource, staleMerizosTarget]);

        setupStaleMerizos({shardedSource: false, shardedTarget: true});
        runOutTest(mode, [staleMerizosSource, staleMerizosTarget]);

        //
        // The remaining tests run against a merizos which is stale with respect to BOTH the source
        // and target collections.
        //
        const sourceCollStale = staleMerizosBoth.getCollection(sourceColl.getName());
        const targetCollStale = staleMerizosBoth.getCollection(targetColl.getName());

        //
        // 1. Both source and target collections are sharded.
        //
        sourceCollStale.drop();
        targetCollStale.drop();

        // Insert into both collections through the stale merizos such that it believes the
        // collections exist and are unsharded.
        assert.commandWorked(sourceCollStale.insert({_id: 0}));
        assert.commandWorked(targetCollStale.insert({_id: 0}));

        shardCollWithMerizos(freshMerizos, sourceColl);
        shardCollWithMerizos(freshMerizos, targetColl);

        // Test against the stale merizos, which believes both collections are unsharded.
        runOutTest(mode, staleMerizosBoth);

        //
        // 2. Both source and target collections are unsharded.
        //
        sourceColl.drop();
        targetColl.drop();

        // The collections were both dropped through a different merizos, so the stale merizos still
        // believes that they're sharded.
        runOutTest(mode, staleMerizosBoth);

        //
        // 3. Source collection is sharded and target collection is unsharded.
        //
        sourceCollStale.drop();

        // Insert into the source collection through the stale merizos such that it believes the
        // collection exists and is unsharded.
        assert.commandWorked(sourceCollStale.insert({_id: 0}));

        // Shard the source collection through the fresh merizos.
        shardCollWithMerizos(freshMerizos, sourceColl);

        // Shard the target through the stale merizos, but then drop and recreate it as unsharded
        // through a different merizos.
        shardCollWithMerizos(staleMerizosBoth, targetColl);
        targetColl.drop();

        // At this point, the stale merizos believes the source collection is unsharded and the
        // target collection is sharded when in fact the reverse is true.
        runOutTest(mode, staleMerizosBoth);

        //
        // 4. Source collection is unsharded and target collection is sharded.
        //
        sourceCollStale.drop();
        targetCollStale.drop();

        // Insert into the target collection through the stale merizos such that it believes the
        // collection exists and is unsharded.
        assert.commandWorked(targetCollStale.insert({_id: 0}));

        shardCollWithMerizos(freshMerizos, targetColl);

        // Shard the source through the stale merizos, but then drop and recreate it as unsharded
        // through a different merizos.
        shardCollWithMerizos(staleMerizosBoth, sourceColl);
        sourceColl.drop();

        // At this point, the stale merizos believes the source collection is sharded and the target
        // collection is unsharded when in fact the reverse is true.
        runOutTest(mode, staleMerizosBoth);
    });

    // Mode "replaceCollection" is special because the aggregation will fail if the target
    // collection is sharded.
    setupStaleMerizos({shardedSource: false, shardedTarget: false});
    runOutTest("replaceCollection", [staleMerizosSource, staleMerizosTarget]);

    setupStaleMerizos({shardedSource: true, shardedTarget: true});
    assert.eq(assert.throws(() => runOutTest("replaceCollection", staleMerizosSource)).code, 28769);
    assert.eq(assert.throws(() => runOutTest("replaceCollection", staleMerizosTarget)).code, 17017);

    setupStaleMerizos({shardedSource: true, shardedTarget: false});
    runOutTest("replaceCollection", [staleMerizosSource, staleMerizosTarget]);

    setupStaleMerizos({shardedSource: false, shardedTarget: true});
    assert.eq(assert.throws(() => runOutTest("replaceCollection", staleMerizosSource)).code, 28769);
    assert.eq(assert.throws(() => runOutTest("replaceCollection", staleMerizosTarget)).code, 17017);

    st.stop();
}());
