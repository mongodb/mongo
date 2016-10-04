// Test shard targeting for queries on a collection with a default collation.
(function() {
    "use strict";

    const caseInsensitive = {locale: "en_US", strength: 2};

    var explain;
    var writeRes;

    // Create a cluster with 3 shards.
    var st = new ShardingTest({shards: 3});
    var testDB = st.s.getDB("test");
    assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), "shard0001");

    // Create a collection with a case-insensitive default collation sharded on {a: 1}.
    var collCaseInsensitive = testDB.getCollection("case_insensitive");
    collCaseInsensitive.drop();
    assert.commandWorked(testDB.createCollection("case_insensitive", {collation: caseInsensitive}));
    assert.commandWorked(collCaseInsensitive.createIndex({a: 1}, {collation: {locale: "simple"}}));
    assert.commandWorked(collCaseInsensitive.createIndex({geo: "2dsphere"}));
    assert.commandWorked(testDB.adminCommand({
        shardCollection: collCaseInsensitive.getFullName(),
        key: {a: 1},
        collation: {locale: "simple"}
    }));

    // Split the collection.
    // shard0000: { "a" : { "$minKey" : 1 } } -->> { "a" : 10 }
    // shard0001: { "a" : 10 } -->> { "a" : "a"}
    // shard0002: { "a" : "a" } -->> { "a" : { "$maxKey" : 1 }}
    assert.commandWorked(
        testDB.adminCommand({split: collCaseInsensitive.getFullName(), middle: {a: 10}}));
    assert.commandWorked(
        testDB.adminCommand({split: collCaseInsensitive.getFullName(), middle: {a: "a"}}));
    assert.commandWorked(testDB.adminCommand(
        {moveChunk: collCaseInsensitive.getFullName(), find: {a: 1}, to: "shard0000"}));
    assert.commandWorked(testDB.adminCommand(
        {moveChunk: collCaseInsensitive.getFullName(), find: {a: "FOO"}, to: "shard0001"}));
    assert.commandWorked(testDB.adminCommand(
        {moveChunk: collCaseInsensitive.getFullName(), find: {a: "foo"}, to: "shard0002"}));

    // Put data on each shard.
    // Note that the balancer is off by default, so the chunks will stay put.
    // shard0000: {a: 1}
    // shard0001: {a: 100}, {a: "FOO"}
    // shard0002: {a: "foo"}
    // Include geo field to test geoNear.
    var a_1 = {_id: 0, a: 1, geo: {type: "Point", coordinates: [0, 0]}};
    var a_100 = {_id: 1, a: 100, geo: {type: "Point", coordinates: [0, 0]}};
    var a_FOO = {_id: 2, a: "FOO", geo: {type: "Point", coordinates: [0, 0]}};
    var a_foo = {_id: 3, a: "foo", geo: {type: "Point", coordinates: [0, 0]}};
    assert.writeOK(collCaseInsensitive.insert(a_1));
    assert.writeOK(collCaseInsensitive.insert(a_100));
    assert.writeOK(collCaseInsensitive.insert(a_FOO));
    assert.writeOK(collCaseInsensitive.insert(a_foo));

    // Aggregate.

    // Test an aggregate command on strings with a non-simple collation inherited from the
    // collection default. This should be scatter-gather.
    assert.eq(2, collCaseInsensitive.aggregate([{$match: {a: "foo"}}]).itcount());
    explain = collCaseInsensitive.explain().aggregate([{$match: {a: "foo"}}]);
    assert.commandWorked(explain);
    assert.eq(3, Object.keys(explain.shards).length);

    // Test an aggregate command with a simple collation. This should be single-shard.
    assert.eq(1,
              collCaseInsensitive.aggregate([{$match: {a: "foo"}}], {collation: {locale: "simple"}})
                  .itcount());
    explain = collCaseInsensitive.explain().aggregate([{$match: {a: "foo"}}],
                                                      {collation: {locale: "simple"}});
    assert.commandWorked(explain);
    assert.eq(1, Object.keys(explain.shards).length);

    // Test an aggregate command on numbers with a non-simple collation inherited from the
    // collection default. This should be single-shard.
    assert.eq(1, collCaseInsensitive.aggregate([{$match: {a: 100}}]).itcount());
    explain = collCaseInsensitive.explain().aggregate([{$match: {a: 100}}]);
    assert.commandWorked(explain);
    assert.eq(1, Object.keys(explain.shards).length);

    // Count.

    // Test a count command on strings with a non-simple collation inherited from the collection
    // default. This should be scatter-gather.
    assert.eq(2, collCaseInsensitive.find({a: "foo"}).count());
    explain = collCaseInsensitive.explain().find({a: "foo"}).count();
    assert.commandWorked(explain);
    assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

    // Test a count command with a simple collation. This should be single-shard.
    assert.eq(1, collCaseInsensitive.find({a: "foo"}).collation({locale: "simple"}).count());
    explain = collCaseInsensitive.explain().find({a: "foo"}).collation({locale: "simple"}).count();
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

    // Test a find command on numbers with a non-simple collation inheritied from the collection
    // default. This should be single-shard.
    assert.eq(1, collCaseInsensitive.find({a: 100}).count());
    explain = collCaseInsensitive.explain().find({a: 100}).count();
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

    // Distinct.

    // Test a distinct command on strings with a non-simple collation inherited from the collection
    // default. This should be scatter-gather.
    assert.eq(2, collCaseInsensitive.distinct("_id", {a: "foo"}).length);
    explain = collCaseInsensitive.explain().distinct("_id", {a: "foo"});
    assert.commandWorked(explain);
    assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

    // Test that deduping respects the collation inherited from the collection default.
    assert.eq(1, collCaseInsensitive.distinct("a", {a: "foo"}).length);

    // Test a distinct command with a simple collation. This should be single-shard.
    assert.eq(
        1, collCaseInsensitive.distinct("_id", {a: "foo"}, {collation: {locale: "simple"}}).length);
    explain =
        collCaseInsensitive.explain().distinct("_id", {a: "foo"}, {collation: {locale: "simple"}});
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

    // Test a distinct command on numbers with a non-simple collation inherited from the collection
    // default. This should be single-shard.
    assert.eq(1, collCaseInsensitive.distinct("_id", {a: 100}).length);
    explain = collCaseInsensitive.explain().distinct("_id", {a: 100});
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

    // Find.

    // Test a find command on strings with a non-simple collation inherited from the collection
    // default. This should be scatter-gather.
    assert.eq(2, collCaseInsensitive.find({a: "foo"}).itcount());
    explain = collCaseInsensitive.find({a: "foo"}).explain();
    assert.commandWorked(explain);
    assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

    // Test a find command with a simple collation. This should be single-shard.
    if (testDB.getMongo().useReadCommands()) {
        assert.eq(1, collCaseInsensitive.find({a: "foo"}).collation({locale: "simple"}).itcount());
        explain = collCaseInsensitive.find({a: "foo"}).collation({locale: "simple"}).explain();
        assert.commandWorked(explain);
        assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
    }

    // Test a find command on numbers with a non-simple collation inherited from the collection
    // default. This should be single-shard.
    assert.eq(1, collCaseInsensitive.find({a: 100}).itcount());
    explain = collCaseInsensitive.find({a: 100}).explain();
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

    // FindAndModify.

    // Sharded findAndModify on strings with non-simple collation inherited from the collection
    // default should fail, because findAndModify must target a single shard.
    assert.throws(function() {
        collCaseInsensitive.findAndModify({query: {a: "foo"}, update: {$set: {b: 1}}});
    });
    assert.throws(function() {
        collCaseInsensitive.explain().findAndModify({query: {a: "foo"}, update: {$set: {b: 1}}});
    });

    // Sharded findAndModify on strings with simple collation should succeed. This should be
    // single-shard.
    assert.eq("foo",
              collCaseInsensitive
                  .findAndModify(
                      {query: {a: "foo"}, update: {$set: {b: 1}}, collation: {locale: "simple"}})
                  .a);
    explain = collCaseInsensitive.explain().findAndModify(
        {query: {a: "foo"}, update: {$set: {b: 1}}, collation: {locale: "simple"}});
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

    // Sharded findAndModify on numbers with non-simple collation inherited from collection default
    // should succeed. This should be single-shard.
    assert.eq(100, collCaseInsensitive.findAndModify({query: {a: 100}, update: {$set: {b: 1}}}).a);
    explain =
        collCaseInsensitive.explain().findAndModify({query: {a: 100}, update: {$set: {b: 1}}});
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

    // GeoNear.

    // Test geoNear on strings with a non-simple collation inherited from collection default.
    assert.eq(2,
              assert
                  .commandWorked(testDB.runCommand({
                      geoNear: collCaseInsensitive.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {a: "foo"}
                  }))
                  .results.length);

    // Test geoNear on strings with a simple collation.
    assert.eq(1,
              assert
                  .commandWorked(testDB.runCommand({
                      geoNear: collCaseInsensitive.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {a: "foo"},
                      collation: {locale: "simple"}
                  }))
                  .results.length);

    // MapReduce.

    // Test mapReduce on strings with a non-simple collation inherited from collection default.
    assert.eq(2,
              assert
                  .commandWorked(collCaseInsensitive.mapReduce(
                      function() {
                          emit(this.a, 1);
                      },
                      function(key, values) {
                          return Array.sum(values);
                      },
                      {out: {inline: 1}, query: {a: "foo"}}))
                  .results.length);

    // Test mapReduce on strings with a simple collation.
    assert.eq(1,
              assert
                  .commandWorked(collCaseInsensitive.mapReduce(
                      function() {
                          emit(this.a, 1);
                      },
                      function(key, values) {
                          return Array.sum(values);
                      },
                      {out: {inline: 1}, query: {a: "foo"}, collation: {locale: "simple"}}))
                  .results.length);

    // Remove.

    // Test a remove command on strings with non-simple collation inherited from collection default.
    // This should be scatter-gather.
    writeRes = collCaseInsensitive.remove({a: "foo"});
    assert.writeOK(writeRes);
    assert.eq(2, writeRes.nRemoved);
    explain = collCaseInsensitive.explain().remove({a: "foo"});
    assert.commandWorked(explain);
    assert.eq(3, explain.queryPlanner.winningPlan.shards.length);
    assert.writeOK(collCaseInsensitive.insert(a_FOO));
    assert.writeOK(collCaseInsensitive.insert(a_foo));

    // Test a remove command on strings with simple collation. This should be single-shard.
    if (testDB.getMongo().writeMode() === "commands") {
        writeRes = collCaseInsensitive.remove({a: "foo"}, {collation: {locale: "simple"}});
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nRemoved);
        explain = collCaseInsensitive.explain().remove({a: "foo"}, {collation: {locale: "simple"}});
        assert.commandWorked(explain);
        assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
        assert.writeOK(collCaseInsensitive.insert(a_foo));
    }

    // Test a remove command on numbers with non-simple collation inherited from collection default.
    // This should be single-shard.
    writeRes = collCaseInsensitive.remove({a: 100});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nRemoved);
    explain = collCaseInsensitive.explain().remove({a: 100});
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
    assert.writeOK(collCaseInsensitive.insert(a_100));

    // A single remove (justOne: true) must be single-shard or an exact-ID query. A query is
    // exact-ID if it contains an equality on _id and either has the collection default collation or
    // _id is not a string/object/array.

    // Single remove on string shard key with non-simple collation inherited from collection default
    // should fail, because it is not single-shard.
    assert.writeError(collCaseInsensitive.remove({a: "foo"}, {justOne: true}));

    // Single remove on string shard key with simple collation should succeed, because it is
    // single-shard.
    if (testDB.getMongo().writeMode() === "commands") {
        writeRes =
            collCaseInsensitive.remove({a: "foo"}, {justOne: true, collation: {locale: "simple"}});
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nRemoved);
        explain = collCaseInsensitive.explain().remove(
            {a: "foo"}, {justOne: true, collation: {locale: "simple"}});
        assert.commandWorked(explain);
        assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
        assert.writeOK(collCaseInsensitive.insert(a_foo));
    }

    // Single remove on number shard key with non-simple collation inherited from collection default
    // should succeed, because it is single-shard.
    writeRes = collCaseInsensitive.remove({a: 100}, {justOne: true});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nRemoved);
    explain = collCaseInsensitive.explain().remove({a: 100}, {justOne: true});
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
    assert.writeOK(collCaseInsensitive.insert(a_100));

    // Single remove on string _id with non-collection-default collation should fail, because it is
    // not an exact-ID query.
    assert.writeError(
        collCaseInsensitive.remove({_id: "foo"}, {justOne: true, collation: {locale: "simple"}}));

    // Single remove on string _id with collection-default collation should succeed, because it is
    // an exact-ID query.
    assert.writeOK(collCaseInsensitive.insert({_id: "foo", a: "bar"}));
    writeRes = collCaseInsensitive.remove({_id: "foo"}, {justOne: true});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nRemoved);

    // Single remove on string _id with collection-default collation explicitly given should
    // succeed, because it is an exact-ID query.
    if (testDB.getMongo().writeMode() === "commands") {
        assert.writeOK(collCaseInsensitive.insert({_id: "foo", a: "bar"}));
        writeRes =
            collCaseInsensitive.remove({_id: "foo"}, {justOne: true, collation: caseInsensitive});
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nRemoved);
    }

    // Single remove on number _id with non-collection-default collation should succeed, because it
    // is an exact-ID query.
    writeRes = collCaseInsensitive.remove({_id: a_100._id},
                                          {justOne: true, collation: {locale: "simple"}});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nRemoved);
    assert.writeOK(collCaseInsensitive.insert(a_100));

    // Update.

    // Test an update command on strings with non-simple collation inherited from collection
    // default. This should be scatter-gather.
    writeRes = collCaseInsensitive.update({a: "foo"}, {$set: {b: 1}}, {multi: true});
    assert.writeOK(writeRes);
    assert.eq(2, writeRes.nMatched);
    explain = collCaseInsensitive.explain().update({a: "foo"}, {$set: {b: 1}}, {multi: true});
    assert.commandWorked(explain);
    assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

    // Test an update command on strings with simple collation. This should be single-shard.
    if (testDB.getMongo().writeMode() === "commands") {
        writeRes = collCaseInsensitive.update(
            {a: "foo"}, {$set: {b: 1}}, {multi: true, collation: {locale: "simple"}});
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nMatched);
        explain = collCaseInsensitive.explain().update(
            {a: "foo"}, {$set: {b: 1}}, {multi: true, collation: {locale: "simple"}});
        assert.commandWorked(explain);
        assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
    }

    // Test an update command on numbers with non-simple collation inherited from collection
    // default. This should be single-shard.
    writeRes = collCaseInsensitive.update({a: 100}, {$set: {b: 1}}, {multi: true});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nMatched);
    explain = collCaseInsensitive.explain().update({a: 100}, {$set: {b: 1}}, {multi: true});
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

    // A single (non-multi) update must be single-shard or an exact-ID query. A query is exact-ID if
    // it
    // contains an equality on _id and either has the collection default collation or _id is not a
    // string/object/array.

    // Single update on string shard key with non-simple collation inherited from collection default
    // should fail, because it is not single-shard.
    assert.writeError(collCaseInsensitive.update({a: "foo"}, {$set: {b: 1}}));

    // Single update on string shard key with simple collation should succeed, because it is
    // single-shard.
    if (testDB.getMongo().writeMode() === "commands") {
        writeRes =
            collCaseInsensitive.update({a: "foo"}, {$set: {b: 1}}, {collation: {locale: "simple"}});
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nMatched);
        explain = collCaseInsensitive.explain().update(
            {a: "foo"}, {$set: {b: 1}}, {collation: {locale: "simple"}});
        assert.commandWorked(explain);
        assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
    }

    // Single update on number shard key with non-simple collation inherited from collation default
    // should succeed, because it is single-shard.
    writeRes = collCaseInsensitive.update({a: 100}, {$set: {b: 1}});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nMatched);
    explain = collCaseInsensitive.explain().update({a: 100}, {$set: {b: 1}});
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

    // Single update on string _id with non-collection-default collation should fail, because it is
    // not an exact-ID query.
    if (testDB.getMongo().writeMode() === "commands") {
        assert.writeError(collCaseInsensitive.update(
            {_id: "foo"}, {$set: {b: 1}}, {collation: {locale: "simple"}}));
    }

    // Single update on string _id with collection-default collation should succeed, because it is
    // an exact-ID query.
    assert.writeOK(collCaseInsensitive.insert({_id: "foo", a: "bar"}));
    writeRes = collCaseInsensitive.update({_id: "foo"}, {$set: {b: 1}});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nMatched);
    assert.writeOK(collCaseInsensitive.remove({_id: "foo"}, {justOne: true}));

    // Single update on string _id with collection-default collation explicitly given should
    // succeed, because it is an exact-ID query.
    if (testDB.getMongo().writeMode() === "commands") {
        assert.writeOK(collCaseInsensitive.insert({_id: "foo", a: "bar"}));
        writeRes =
            collCaseInsensitive.update({_id: "foo"}, {$set: {b: 1}}, {collation: caseInsensitive});
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nMatched);
        assert.writeOK(collCaseInsensitive.remove({_id: "foo"}, {justOne: true}));
    }

    // Single update on number _id with non-collection-default collation inherited from collection
    // default should succeed, because it is an exact-ID query.
    if (testDB.getMongo().writeMode() === "commands") {
        writeRes = collCaseInsensitive.update(
            {_id: a_foo._id}, {$set: {b: 1}}, {collation: {locale: "simple"}});
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nMatched);
    }

    // Upsert must always be single-shard.

    // Upsert on strings with non-simple collation inherited from collection default should fail,
    // because it is not single-shard.
    assert.writeError(
        collCaseInsensitive.update({a: "foo"}, {$set: {b: 1}}, {multi: true, upsert: true}));

    // Upsert on strings with simple collation should succeed, because it is single-shard.
    if (testDB.getMongo().writeMode() === "commands") {
        writeRes = collCaseInsensitive.update(
            {a: "foo"}, {$set: {b: 1}}, {multi: true, upsert: true, collation: {locale: "simple"}});
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nMatched);
        explain = collCaseInsensitive.explain().update(
            {a: "foo"}, {$set: {b: 1}}, {multi: true, upsert: true, collation: {locale: "simple"}});
        assert.commandWorked(explain);
        assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
    }

    // Upsert on numbers with non-simple collation inherited from collection default should succeed,
    // because it is single-shard.
    writeRes = collCaseInsensitive.update({a: 100}, {$set: {b: 1}}, {multi: true, upsert: true});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nMatched);
    explain =
        collCaseInsensitive.explain().update({a: 100}, {$set: {b: 1}}, {multi: true, upsert: true});
    assert.commandWorked(explain);
    assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

    st.stop();
})();