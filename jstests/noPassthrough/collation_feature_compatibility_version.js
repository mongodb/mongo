// Test that collation usage is restricted when the featureCompatibilityVersion is 3.2.

(function() {
    "use strict";

    //
    // Test correct behavior of operations with collation against a mongod when the
    // featureCompatibilityVersion is 3.2.
    //

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    let collationDB = conn.getDB("collation_operations_feature_compatibility_version");
    assert.commandWorked(collationDB.dropDatabase());

    let adminDB = conn.getDB("admin");

    // Create a collection with a case-insensitive default collation.
    assert.commandWorked(collationDB.createCollection("caseInsensitive",
                                                      {collation: {locale: "en_US", strength: 2}}));
    let caseInsensitive = collationDB.caseInsensitive;
    assert.commandWorked(caseInsensitive.createIndex({geo: "2dsphere"}));
    assert.writeOK(caseInsensitive.insert({str: "foo", geo: {type: "Point", coordinates: [0, 0]}}));

    // Ensure the featureCompatibilityVersion is 3.2.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    let res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.2", res.featureCompatibilityVersion);

    // We cannot create a collection with a default collation when the featureCompatibilityVersion
    // is 3.2.
    assert.commandFailed(
        collationDB.createCollection("collection", {collation: {locale: "fr_CA"}}));

    // We cannot explicitly give a collection the simple collation as its default when the
    // featureCompatibilityVersion is 3.2.
    assert.commandFailed(
        collationDB.createCollection("collection", {collation: {locale: "simple"}}));

    // We cannot create a view with a default collation when the featureCompatibilityVersion is 3.2.
    assert.commandFailed(collationDB.runCommand(
        {create: "view", viewOn: "caseInsensitive", collation: {locale: "fr_CA"}}));

    // We cannot explicitly give a view the simple collation as its default when the
    // featureCompatibilityVersion is 3.2.
    assert.commandFailed(collationDB.runCommand(
        {create: "view", viewOn: "caseInsensitive", collation: {locale: "simple"}}));

    // All operations reject the collation parameter when the featureCompatibilityVersion is 3.2.
    assert.throws(function() {
        caseInsensitive.aggregate([], {collation: {locale: "en_US"}});
    });
    assert.throws(function() {
        caseInsensitive.aggregate([], {explain: true, collation: {locale: "en_US"}});
    });
    assert.throws(function() {
        caseInsensitive.find().collation({locale: "en_US"}).count();
    });
    assert.throws(function() {
        caseInsensitive.explain().find().collation({locale: "en_US"}).count();
    });
    assert.throws(function() {
        caseInsensitive.distinct("str", {}, {collation: {locale: "en_US"}});
    });
    assert.throws(function() {
        caseInsensitive.explain().distinct("str", {}, {collation: {locale: "en_US"}});
    });
    assert.throws(function() {
        caseInsensitive.find().collation({locale: "en_US"}).itcount();
    });
    assert.throws(function() {
        caseInsensitive.find().collation({locale: "en_US"}).explain();
    });
    assert.throws(function() {
        caseInsensitive.findAndModify({update: {$set: {a: 1}}, collation: {locale: "en_US"}});
    });
    assert.throws(function() {
        caseInsensitive.explain().findAndModify(
            {update: {$set: {a: 1}}, collation: {locale: "en_US"}});
    });
    assert.throws(function() {
        caseInsensitive.findAndModify({remove: true, collation: {locale: "en_US"}});
    });
    assert.throws(function() {
        caseInsensitive.explain().findAndModify({remove: true, collation: {locale: "en_US"}});
    });
    assert.commandFailed(collationDB.runCommand({
        geoNear: caseInsensitive.getName(),
        near: {type: "Point", coordinates: [0, 0]},
        spherical: true,
        collation: {locale: "en_US"}
    }));
    assert.throws(function() {
        caseInsensitive.group(
            {initial: {}, reduce: function(curr, result) {}, collation: {locale: "en_US"}});
    });
    assert.throws(function() {
        caseInsensitive.explain().group(
            {initial: {}, reduce: function(curr, result) {}, collation: {locale: "en_US"}});
    });
    assert.throws(function() {
        caseInsensitive.mapReduce(
            function() {
                emit(this, 1);
            },
            function(key, values) {
                return 0;
            },
            {out: {inline: 1}, collation: {locale: "en_US"}});
    });
    assert.writeError(caseInsensitive.update({}, {$set: {a: 1}}, {collation: {locale: "en_US"}}));
    assert.throws(function() {
        caseInsensitive.explain().update({}, {$set: {a: 1}}, {collation: {locale: "en_US"}});
    });
    assert.writeError(caseInsensitive.remove({}, {collation: {locale: "en_US"}}));
    assert.throws(function() {
        caseInsensitive.explain().remove({}, {collation: {locale: "en_US"}});
    });

    // All operations respect the collection default collation when the featureCompatibilityVersion
    // is 3.2.
    assert.eq(1, caseInsensitive.aggregate([{$match: {str: "FOO"}}]).itcount());
    assert.eq(1, caseInsensitive.find({str: "FOO"}).count());
    assert.eq(1, caseInsensitive.distinct("str", {str: "FOO"}).length);
    assert.eq(1, caseInsensitive.find({str: "FOO"}).itcount());
    assert.eq("foo",
              caseInsensitive.findAndModify({query: {str: "FOO"}, update: {$set: {a: 1}}}).str);
    assert.eq(1,
              assert
                  .commandWorked(collationDB.runCommand({
                      geoNear: caseInsensitive.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "FOO"}
                  }))
                  .results.length);
    assert.eq([{count: 1}], caseInsensitive.group({
        cond: {str: "FOO"},
        initial: {count: 0},
        reduce: function(curr, result) {
            result.count += 1;
        }
    }));
    assert.eq(1,
              assert
                  .commandWorked(caseInsensitive.mapReduce(
                      function() {
                          emit(this.str, 1);
                      },
                      function(key, values) {
                          return Array.sum(values);
                      },
                      {out: {inline: 1}, query: {str: "FOO"}}))
                  .results.length);
    assert.eq(1, caseInsensitive.update({str: "FOO"}, {$set: {a: 1}}).nMatched);
    assert.eq(1, caseInsensitive.remove({str: "FOO"}).nRemoved);

    MongoRunner.stopMongod(conn);

    //
    // Test correct behavior of aggregation with collation against a sharded cluster when the
    // featureCompatibilityVersion is 3.2.
    //

    const st = new ShardingTest({shards: 2});
    collationDB = st.s.getDB("collation_operations_feature_compatibility_version");
    assert.commandWorked(collationDB.dropDatabase());
    assert.commandWorked(collationDB.adminCommand({enableSharding: collationDB.getName()}));

    adminDB = st.s.getDB("admin");

    // Create a collection with a case-insensitive default collation.
    assert.commandWorked(collationDB.createCollection("caseInsensitive",
                                                      {collation: {locale: "en_US", strength: 2}}));
    caseInsensitive = collationDB.caseInsensitive;
    assert.commandWorked(collationDB.adminCommand({
        shardCollection: caseInsensitive.getFullName(),
        key: {str: 1},
        collation: {locale: "simple"}
    }));

    // Split the collection. Ensure {str: "foo"} and {str: "FOO"} are on separate shards.
    assert.commandWorked(
        collationDB.adminCommand({split: caseInsensitive.getFullName(), middle: {str: "a"}}));
    assert.commandWorked(collationDB.adminCommand(
        {moveChunk: caseInsensitive.getFullName(), find: {str: "FOO"}, to: "shard0000"}));
    assert.commandWorked(collationDB.adminCommand(
        {moveChunk: caseInsensitive.getFullName(), find: {str: "foo"}, to: "shard0001"}));
    assert.writeOK(caseInsensitive.insert({str: "FOO"}));
    assert.writeOK(caseInsensitive.insert({str: "foo"}));

    // Ensure the featureCompatibilityVersion is 3.2.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));

    // Aggregate rejects the collation parameter when the featureCompatibilityVersion is 3.2.
    assert.throws(function() {
        caseInsensitive.aggregate([], {collation: {locale: "en_US"}});
    });
    assert.throws(function() {
        caseInsensitive.aggregate([], {explain: true, collation: {locale: "en_US"}});
    });

    // Aggregate respects the collection default collation in both halves of the pipeline when the
    // featureCompatibilityVersion is 3.2.
    assert.eq([{count: 2}],
              caseInsensitive
                  .aggregate([
                      {$match: {str: "foo"}},
                      {$group: {_id: "$str", count: {$sum: 1}}},
                      {$project: {_id: 0, count: 1}}
                  ])
                  .toArray());

    st.stop();
}());
