// Integration tests for the collation feature.
(function() {
    'use strict';

    load("jstests/libs/analyze_plan.js");

    var coll = db.collation;
    coll.drop();

    var explainRes;
    var writeRes;
    var planStage;

    var isMaster = db.runCommand("ismaster");
    assert.commandWorked(isMaster);
    var isMongos = (isMaster.msg === "isdbgrid");

    var assertIndexHasCollation = function(keyPattern, collation) {
        var foundIndex = false;
        var indexSpecs = coll.getIndexes();
        for (var i = 0; i < indexSpecs.length; ++i) {
            if (bsonWoCompare(indexSpecs[i].key, keyPattern) === 0) {
                foundIndex = true;
                // We assume that the key pattern is unique, even though indices with different
                // collations but the same key pattern are allowed.
                if (collation.locale === "simple") {
                    // The simple collation is not explicitly stored in the catalog, so we expect
                    // the "collation" field to be absent.
                    assert(!indexSpecs[i].hasOwnProperty("collation"),
                           "Expected the simple collation in: " + tojson(indexSpecs[i]));
                } else {
                    assert.eq(indexSpecs[i].collation,
                              collation,
                              "Expected collation " + tojson(collation) + " in: " +
                                  tojson(indexSpecs[i]));
                }
            }
        }
        assert(foundIndex, "index with key pattern " + tojson(keyPattern) + " not found");
    };

    var getQueryCollation = function(explainRes) {
        if (explainRes.queryPlanner.hasOwnProperty("collation")) {
            return explainRes.queryPlanner.collation;
        }

        if (explainRes.queryPlanner.winningPlan.hasOwnProperty("shards") &&
            explainRes.queryPlanner.winningPlan.shards.length > 0 &&
            explainRes.queryPlanner.winningPlan.shards[0].hasOwnProperty("collation")) {
            return explainRes.queryPlanner.winningPlan.shards[0].collation;
        }

        return null;
    };

    //
    // Test using db.createCollection() to make a collection with a default collation.
    //

    // Attempting to create a collection with an invalid collation should fail.
    assert.commandFailed(db.createCollection("collation", {collation: "not an object"}));
    assert.commandFailed(db.createCollection("collation", {collation: {}}));
    assert.commandFailed(db.createCollection("collation", {collation: {blah: 1}}));
    assert.commandFailed(db.createCollection("collation", {collation: {locale: "en", blah: 1}}));
    assert.commandFailed(db.createCollection("collation", {collation: {locale: "xx"}}));
    assert.commandFailed(
        db.createCollection("collation", {collation: {locale: "en", strength: 99}}));

    // Attempting to create a collection whose collation version does not match the collator version
    // produced by ICU should result in failure with a special error code.
    assert.commandFailedWithCode(
        db.createCollection("collation", {collation: {locale: "en", version: "unknownVersion"}}),
        ErrorCodes.IncompatibleCollationVersion);

    // Ensure we can create a collection with the "simple" collation as the collection default.
    assert.commandWorked(db.createCollection("collation", {collation: {locale: "simple"}}));
    var collectionInfos = db.getCollectionInfos({name: "collation"});
    assert.eq(collectionInfos.length, 1);
    assert(!collectionInfos[0].options.hasOwnProperty("collation"));
    coll.drop();

    // Ensure that we populate all collation-related fields when we create a collection with a valid
    // collation.
    assert.commandWorked(db.createCollection("collation", {collation: {locale: "fr_CA"}}));
    var collectionInfos = db.getCollectionInfos({name: "collation"});
    assert.eq(collectionInfos.length, 1);
    assert.eq(collectionInfos[0].options.collation, {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    // Ensure that an index with no collation inherits the collection-default collation.
    assert.commandWorked(coll.ensureIndex({a: 1}));
    assertIndexHasCollation({a: 1}, {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    // Ensure that an index which specifies an overriding collation does not use the collection
    // default.
    assert.commandWorked(coll.ensureIndex({b: 1}, {collation: {locale: "en_US"}}));
    assertIndexHasCollation({b: 1}, {
        locale: "en_US",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: false,
        version: "57.1",
    });

    coll.drop();

    //
    // Creating an index with a collation.
    //

    // Attempting to build an index with an invalid collation should fail.
    assert.commandFailed(coll.ensureIndex({a: 1}, {collation: "not an object"}));
    assert.commandFailed(coll.ensureIndex({a: 1}, {collation: {}}));
    assert.commandFailed(coll.ensureIndex({a: 1}, {collation: {blah: 1}}));
    assert.commandFailed(coll.ensureIndex({a: 1}, {collation: {locale: "en", blah: 1}}));
    assert.commandFailed(coll.ensureIndex({a: 1}, {collation: {locale: "xx"}}));
    assert.commandFailed(coll.ensureIndex({a: 1}, {collation: {locale: "en", strength: 99}}));

    // Attempting to create an index whose collation version does not match the collator version
    // produced by ICU should result in failure with a special error code.
    assert.commandFailedWithCode(
        coll.ensureIndex({a: 1}, {collation: {locale: "en", version: "unknownVersion"}}),
        ErrorCodes.IncompatibleCollationVersion);

    assert.commandWorked(coll.ensureIndex({a: 1}, {collation: {locale: "en_US"}}));
    assertIndexHasCollation({a: 1}, {
        locale: "en_US",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: false,
        version: "57.1",
    });

    assert.commandWorked(coll.createIndex({b: 1}, {collation: {locale: "en_US"}}));
    assertIndexHasCollation({b: 1}, {
        locale: "en_US",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: false,
        version: "57.1",
    });

    assert.commandWorked(coll.createIndexes([{c: 1}, {d: 1}], {collation: {locale: "fr_CA"}}));
    assertIndexHasCollation({c: 1}, {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });
    assertIndexHasCollation({d: 1}, {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    assert.commandWorked(coll.createIndexes([{e: 1}], {collation: {locale: "simple"}}));
    assertIndexHasCollation({e: 1}, {locale: "simple"});

    // Test that an index with a non-simple collation contains collator-generated comparison keys
    // rather than the verbatim indexed strings.
    if (db.getMongo().useReadCommands()) {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "fr_CA"}}));
        assert.commandWorked(coll.createIndex({b: 1}));
        assert.writeOK(coll.insert({a: "foo", b: "foo"}));
        assert.eq(
            1, coll.find({}, {_id: 0, a: 1}).collation({locale: "fr_CA"}).hint({a: 1}).itcount());
        assert.neq(
            "foo",
            coll.find({}, {_id: 0, a: 1}).collation({locale: "fr_CA"}).hint({a: 1}).next().a);
        assert.eq(
            1, coll.find({}, {_id: 0, b: 1}).collation({locale: "fr_CA"}).hint({b: 1}).itcount());
        assert.eq("foo",
                  coll.find({}, {_id: 0, b: 1}).collation({locale: "fr_CA"}).hint({b: 1}).next().b);
    }

    // Test that a query with a string comparison can use an index with a non-simple collation if it
    // has a matching collation.
    if (db.getMongo().useReadCommands()) {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "fr_CA"}}));

        // Query has simple collation, but index has fr_CA collation.
        explainRes = coll.find({a: "foo"}).explain();
        assert.commandWorked(explainRes);
        assert(planHasStage(explainRes.queryPlanner.winningPlan, "COLLSCAN"));

        // Query has en_US collation, but index has fr_CA collation.
        explainRes = coll.find({a: "foo"}).collation({locale: "en_US"}).explain();
        assert.commandWorked(explainRes);
        assert(planHasStage(explainRes.queryPlanner.winningPlan, "COLLSCAN"));

        // Matching collations.
        explainRes = coll.find({a: "foo"}).collation({locale: "fr_CA"}).explain();
        assert.commandWorked(explainRes);
        assert(planHasStage(explainRes.queryPlanner.winningPlan, "IXSCAN"));
    }

    //
    // Collation tests for aggregation.
    //

    // Aggregation should return correct results when collation specified and collection does not
    // exist.
    coll.drop();
    assert.eq(0, coll.aggregate([], {collation: {locale: "fr"}}).itcount());

    // Aggregation should return correct results when collation specified and collection does exist.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    assert.eq(0, coll.aggregate([{$match: {str: "FOO"}}]).itcount());
    assert.eq(1,
              coll.aggregate([{$match: {str: "FOO"}}], {collation: {locale: "en_US", strength: 2}})
                  .itcount());

    // Aggregation should return correct results when no collation specified and collection has a
    // default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({str: "foo"}));
    assert.eq(1, coll.aggregate([{$match: {str: "FOO"}}]).itcount());

    // Aggregation should return correct results when "simple" collation specified and collection
    // has a default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({str: "foo"}));
    assert.eq(0,
              coll.aggregate([{$match: {str: "FOO"}}], {collation: {locale: "simple"}}).itcount());

    // Aggregation should select compatible index when no collation specified and collection has a
    // default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    assert.commandWorked(coll.ensureIndex({a: 1}, {collation: {locale: "en_US"}}));
    var explain = coll.explain("queryPlanner").aggregate([{$match: {a: "foo"}}]).stages[0].$cursor;
    assert(isIxscan(explain.queryPlanner.winningPlan));

    // Aggregation should not use index when no collation specified and collection default
    // collation is incompatible with index collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    assert.commandWorked(coll.ensureIndex({a: 1}, {collation: {locale: "simple"}}));
    var explain = coll.explain("queryPlanner").aggregate([{$match: {a: "foo"}}]).stages[0].$cursor;
    assert(isCollscan(explain.queryPlanner.winningPlan));

    // Explain of aggregation with collation should succeed.
    assert.commandWorked(coll.explain().aggregate([], {collation: {locale: "fr"}}));

    //
    // Collation tests for count.
    //

    // Count should return correct results when collation specified and collection does not exist.
    coll.drop();
    assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).count());

    // Count should return correct results when collation specified and collection does exist.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    assert.eq(0, coll.find({str: "FOO"}).count());
    assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).count());
    assert.eq(1, coll.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).count());
    assert.eq(0, coll.count({str: "FOO"}));
    assert.eq(0, coll.count({str: "FOO"}, {collation: {locale: "en_US"}}));
    assert.eq(1, coll.count({str: "FOO"}, {collation: {locale: "en_US", strength: 2}}));

    // Count should return correct results when no collation specified and collection has a default
    // collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({str: "foo"}));
    assert.eq(1, coll.find({str: "FOO"}).count());

    // Count should return correct results when "simple" collation specified and collection has a
    // default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({str: "foo"}));
    assert.eq(0, coll.find({str: "FOO"}).collation({locale: "simple"}).count());

    // Count should return correct results when collation specified and when run with explain.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    explainRes =
        coll.explain("executionStats").find({str: "FOO"}).collation({locale: "en_US"}).count();
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, planStage);
    assert.eq(0, planStage.advanced);
    explainRes = coll.explain("executionStats")
                     .find({str: "FOO"})
                     .collation({locale: "en_US", strength: 2})
                     .count();
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "COLLSCAN");
    assert.neq(null, planStage);
    assert.eq(1, planStage.advanced);

    // Explain of COUNT_SCAN stage should include index collation.
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "fr_CA"}}));
    explainRes = coll.explain("executionStats").find({a: 5}).count();
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "COUNT_SCAN");
    assert.neq(null, planStage);
    assert.eq(planStage.collation, {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    // Explain of COUNT_SCAN stage should include index collation when index collation is
    // inherited from collection default.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
    assert.commandWorked(coll.createIndex({a: 1}));
    explainRes = coll.explain("executionStats").find({a: 5}).count();
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "COUNT_SCAN");
    assert.neq(null, planStage);
    assert.eq(planStage.collation, {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    //
    // Collation tests for distinct.
    //

    // Distinct should return correct results when collation specified and collection does not
    // exist.
    coll.drop();
    assert.eq(0, coll.distinct("str", {}, {collation: {locale: "en_US", strength: 2}}).length);

    // Distinct should return correct results when collation specified and no indexes exist.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "FOO"}));
    var res = coll.distinct("str", {}, {collation: {locale: "en_US", strength: 2}});
    assert.eq(1, res.length);
    assert.eq("foo", res[0].toLowerCase());
    assert.eq(2, coll.distinct("str", {}, {collation: {locale: "en_US", strength: 3}}).length);
    assert.eq(
        2, coll.distinct("_id", {str: "foo"}, {collation: {locale: "en_US", strength: 2}}).length);

    // Distinct should return correct results when collation specified and compatible index exists.
    coll.createIndex({str: 1}, {collation: {locale: "en_US", strength: 2}});
    res = coll.distinct("str", {}, {collation: {locale: "en_US", strength: 2}});
    assert.eq(1, res.length);
    assert.eq("foo", res[0].toLowerCase());
    assert.eq(2, coll.distinct("str", {}, {collation: {locale: "en_US", strength: 3}}).length);

    // Distinct should return correct results when no collation specified and collection has a
    // default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({str: "foo"}));
    assert.writeOK(coll.insert({str: "FOO"}));
    assert.eq(1, coll.distinct("str").length);
    assert.eq(2, coll.distinct("_id", {str: "foo"}).length);

    // Distinct should return correct results when "simple" collation specified and collection has a
    // default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({str: "foo"}));
    assert.writeOK(coll.insert({str: "FOO"}));
    assert.eq(2, coll.distinct("str", {}, {collation: {locale: "simple"}}).length);
    assert.eq(1, coll.distinct("_id", {str: "foo"}, {collation: {locale: "simple"}}).length);

    // Distinct should select compatible index when no collation specified and collection has a
    // default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    assert.commandWorked(coll.ensureIndex({a: 1}, {collation: {locale: "en_US"}}));
    var explain = coll.explain("queryPlanner").distinct("a");
    assert(planHasStage(explain.queryPlanner.winningPlan, "DISTINCT_SCAN"));

    // Distinct should not use index when no collation specified and collection default collation is
    // incompatible with index collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    assert.commandWorked(coll.ensureIndex({a: 1}, {collation: {locale: "simple"}}));
    var explain = coll.explain("queryPlanner").distinct("a");
    assert(isCollscan(explain.queryPlanner.winningPlan));

    // Explain of DISTINCT_SCAN stage should include index collation.
    coll.drop();
    assert.commandWorked(coll.createIndex({str: 1}, {collation: {locale: "fr_CA"}}));
    explainRes = coll.explain("executionStats").distinct("str", {}, {collation: {locale: "fr_CA"}});
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "DISTINCT_SCAN");
    assert.neq(null, planStage);
    assert.eq(planStage.collation, {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    // Explain of DISTINCT_SCAN stage should include index collation when index collation is
    // inherited from collection default.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
    assert.commandWorked(coll.createIndex({str: 1}));
    explainRes = coll.explain("executionStats").distinct("str");
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "DISTINCT_SCAN");
    assert.neq(null, planStage);
    assert.eq(planStage.collation, {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    //
    // Collation tests for find.
    //

    if (db.getMongo().useReadCommands()) {
        // Find should return correct results when collation specified and collection does not
        // exist.
        coll.drop();
        assert.eq(0, coll.find({_id: "FOO"}).collation({locale: "en_US"}).itcount());

        // Find should return correct results when collation specified and filter is a match on _id.
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "bar"}));
        assert.writeOK(coll.insert({_id: "foo"}));
        assert.eq(0, coll.find({_id: "FOO"}).itcount());
        assert.eq(0, coll.find({_id: "FOO"}).collation({locale: "en_US"}).itcount());
        assert.eq(1, coll.find({_id: "FOO"}).collation({locale: "en_US", strength: 2}).itcount());
        assert.writeOK(coll.remove({_id: "foo"}));

        // Find should return correct results when collation specified and no indexes exist.
        assert.eq(0, coll.find({str: "FOO"}).itcount());
        assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).itcount());
        assert.eq(1, coll.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).itcount());

        // Find should return correct results when collation specified and compatible index exists.
        assert.commandWorked(
            coll.ensureIndex({str: 1}, {collation: {locale: "en_US", strength: 2}}));
        assert.eq(0, coll.find({str: "FOO"}).hint({str: 1}).itcount());
        assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).hint({str: 1}).itcount());
        assert.eq(1,
                  coll.find({str: "FOO"})
                      .collation({locale: "en_US", strength: 2})
                      .hint({str: 1})
                      .itcount());
        assert.commandWorked(coll.dropIndexes());

        // Find should return correct results when collation specified and compatible partial index
        // exists.
        assert.commandWorked(coll.ensureIndex({str: 1}, {
            partialFilterExpression: {str: {$lte: "FOO"}},
            collation: {locale: "en_US", strength: 2}
        }));
        assert.eq(1,
                  coll.find({str: "foo"})
                      .collation({locale: "en_US", strength: 2})
                      .hint({str: 1})
                      .itcount());
        assert.writeOK(coll.insert({_id: 3, str: "goo"}));
        assert.eq(0,
                  coll.find({str: "goo"})
                      .collation({locale: "en_US", strength: 2})
                      .hint({str: 1})
                      .itcount());
        assert.writeOK(coll.remove({_id: 3}));
        assert.commandWorked(coll.dropIndexes());

        // Queries that use a index with a non-matching collation should add a sort
        // stage if needed.
        coll.drop();
        assert.writeOK(coll.insert([{a: "A"}, {a: "B"}, {a: "b"}, {a: "a"}]));

        // Ensure results from an index that doesn't match the query collation are sorted to match
        // the requested collation.
        assert.commandWorked(coll.ensureIndex({a: 1}));
        var res = coll.find({a: {'$exists': true}}, {_id: 0})
                      .collation({locale: "en_US", strength: 3})
                      .sort({a: 1});
        assert.eq(res.toArray(), [{a: "a"}, {a: "A"}, {a: "b"}, {a: "B"}]);

        // Ensure results from index with min/max query are sorted to match requested collation.
        coll.drop();
        assert.commandWorked(coll.ensureIndex({a: 1, b: 1}));
        assert.writeOK(coll.insert(
            [{a: 1, b: 1}, {a: 1, b: 2}, {a: 1, b: "A"}, {a: 1, b: "a"}, {a: 2, b: 2}]));
        var expected = [{a: 1, b: 1}, {a: 1, b: 2}, {a: 1, b: "a"}, {a: 1, b: "A"}, {a: 2, b: 2}];
        res = coll.find({}, {_id: 0})
                  .hint({a: 1, b: 1})
                  .min({a: 1, b: 1})
                  .max({a: 2, b: 3})
                  .collation({locale: "en_US", strength: 3})
                  .sort({a: 1, b: 1});
        assert.eq(res.toArray(), expected);
        res = coll.find({}, {_id: 0})
                  .hint({a: 1, b: 1})
                  .min({a: 1, b: 1})
                  .collation({locale: "en_US", strength: 3})
                  .sort({a: 1, b: 1});
        assert.eq(res.toArray(), expected);
        res = coll.find({}, {_id: 0})
                  .hint({a: 1, b: 1})
                  .max({a: 2, b: 3})
                  .collation({locale: "en_US", strength: 3})
                  .sort({a: 1, b: 1});
        assert.eq(res.toArray(), expected);
        res = coll.find({}, {_id: 0})
                  .hint({a: 1, b: 1})
                  .min({a: 1, b: "A"})
                  .max({a: 2, b: 1})
                  .collation({locale: "en_US", strength: 3})
                  .sort({a: 1, b: 1});
        assert.eq(res.toArray(), [{a: 1, b: "a"}, {a: 1, b: "A"}]);
    }

    // Find should return correct results when no collation specified and collection has a default
    // collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({str: "foo"}));
    assert.writeOK(coll.insert({str: "FOO"}));
    assert.writeOK(coll.insert({str: "bar"}));
    assert.eq(3, coll.find({str: {$in: ["foo", "bar"]}}).itcount());
    assert.eq(2, coll.find({str: "foo"}).itcount());
    assert.eq([{str: "bar"}, {str: "foo"}, {str: "FOO"}],
              coll.find({}, {_id: 0, str: 1}).sort({str: 1}).toArray());

    // Find with idhack should return correct results when no collation specified and collection has
    // a default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: "foo"}));
    assert.eq(1, coll.find({_id: "FOO"}).itcount());

    // Find on _id should use idhack stage when query inherits collection default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").find({_id: "foo"}).finish();
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
    assert.neq(null, planStage);

    // Find with oplog replay should return correct results when no collation specified and
    // collection has a default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(
        coll.getName(),
        {collation: {locale: "en_US", strength: 2}, capped: true, size: 16 * 1024}));
    assert.writeOK(coll.insert({str: "FOO", ts: Timestamp(1000, 0)}));
    assert.writeOK(coll.insert({str: "FOO", ts: Timestamp(1000, 1)}));
    assert.writeOK(coll.insert({str: "FOO", ts: Timestamp(1000, 2)}));
    assert.eq(2,
              coll.find({str: "foo", ts: {$gte: Timestamp(1000, 1)}})
                  .addOption(DBQuery.Option.oplogReplay)
                  .itcount());

    if (db.getMongo().useReadCommands()) {
        // Find should return correct results when "simple" collation specified and collection has a
        // default collation.
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
        assert.writeOK(coll.insert({str: "foo"}));
        assert.writeOK(coll.insert({str: "FOO"}));
        assert.writeOK(coll.insert({str: "bar"}));
        assert.eq(2,
                  coll.find({str: {$in: ["foo", "bar"]}}).collation({locale: "simple"}).itcount());
        assert.eq(1, coll.find({str: "foo"}).collation({locale: "simple"}).itcount());
        assert.eq(
            [{str: "FOO"}, {str: "bar"}, {str: "foo"}],
            coll.find({}, {_id: 0, str: 1}).sort({str: 1}).collation({locale: "simple"}).toArray());

        // Find on _id should return correct results when query collation differs from collection
        // default collation.
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 3}}));
        assert.writeOK(coll.insert({_id: "foo"}));
        assert.writeOK(coll.insert({_id: "FOO"}));
        assert.eq(2, coll.find({_id: "foo"}).collation({locale: "en_US", strength: 2}).itcount());

        // Find on _id should use idhack stage when explicitly given query collation matches
        // collection default.
        coll.drop();
        assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
        explainRes =
            coll.explain("executionStats").find({_id: "foo"}).collation({locale: "en_US"}).finish();
        assert.commandWorked(explainRes);
        planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
        assert.neq(null, planStage);

        // Find on _id should not use idhack stage when query collation does not match collection
        // default.
        coll.drop();
        assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
        explainRes =
            coll.explain("executionStats").find({_id: "foo"}).collation({locale: "fr_CA"}).finish();
        assert.commandWorked(explainRes);
        planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
        assert.eq(null, planStage);

        // Find with oplog replay should return correct results when "simple" collation specified
        // and collection has a default collation.
        coll.drop();
        assert.commandWorked(db.createCollection(
            coll.getName(),
            {collation: {locale: "en_US", strength: 2}, capped: true, size: 16 * 1024}));
        const t0 = Timestamp(1000, 0);
        const t1 = Timestamp(1000, 1);
        const t2 = Timestamp(1000, 2);
        assert.writeOK(coll.insert({str: "FOO", ts: Timestamp(1000, 0)}));
        assert.writeOK(coll.insert({str: "FOO", ts: Timestamp(1000, 1)}));
        assert.writeOK(coll.insert({str: "FOO", ts: Timestamp(1000, 2)}));
        assert.eq(0,
                  coll.find({str: "foo", ts: {$gte: Timestamp(1000, 1)}})
                      .addOption(DBQuery.Option.oplogReplay)
                      .collation({locale: "simple"})
                      .itcount());
    }

    // Find should select compatible index when no collation specified and collection has a default
    // collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    assert.commandWorked(coll.ensureIndex({a: 1}, {collation: {locale: "en_US"}}));
    var explain = coll.find({a: "foo"}).explain("queryPlanner");
    assert(isIxscan(explain.queryPlanner.winningPlan));

    // Find should select compatible index when no collation specified and collection default
    // collation is "simple".
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "simple"}}));
    assert.commandWorked(coll.ensureIndex({a: 1}, {collation: {locale: "simple"}}));
    var explain = coll.find({a: "foo"}).explain("queryPlanner");
    assert(isIxscan(explain.queryPlanner.winningPlan));

    // Find should not use index when no collation specified, index collation is "simple", and
    // collection has a non-"simple" default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    assert.commandWorked(coll.ensureIndex({a: 1}, {collation: {locale: "simple"}}));
    var explain = coll.find({a: "foo"}).explain("queryPlanner");
    assert(isCollscan(explain.queryPlanner.winningPlan));

    // Find should select compatible index when "simple" collation specified and collection has a
    // non-"simple" default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    assert.commandWorked(coll.ensureIndex({a: 1}, {collation: {locale: "simple"}}));
    var explain = coll.find({a: "foo"}).collation({locale: "simple"}).explain("queryPlanner");
    assert(isIxscan(explain.queryPlanner.winningPlan));

    // Find should return correct results when collation specified and run with explain.
    coll.drop();
    assert.writeOK(coll.insert({str: "foo"}));
    explainRes =
        coll.explain("executionStats").find({str: "FOO"}).collation({locale: "en_US"}).finish();
    assert.commandWorked(explainRes);
    assert.eq(0, explainRes.executionStats.nReturned);
    explainRes = coll.explain("executionStats")
                     .find({str: "FOO"})
                     .collation({locale: "en_US", strength: 2})
                     .finish();
    assert.commandWorked(explainRes);
    assert.eq(1, explainRes.executionStats.nReturned);

    // Explain of find should include query collation.
    coll.drop();
    explainRes =
        coll.explain("executionStats").find({str: "foo"}).collation({locale: "fr_CA"}).finish();
    assert.commandWorked(explainRes);
    assert.eq(getQueryCollation(explainRes), {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    // Explain of find should include query collation when inherited from collection default.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
    explainRes = coll.explain("executionStats").find({str: "foo"}).finish();
    assert.commandWorked(explainRes);
    assert.eq(getQueryCollation(explainRes), {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    // Explain of IXSCAN stage should include index collation.
    coll.drop();
    assert.commandWorked(coll.createIndex({str: 1}, {collation: {locale: "fr_CA"}}));
    explainRes =
        coll.explain("executionStats").find({str: "foo"}).collation({locale: "fr_CA"}).finish();
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IXSCAN");
    assert.neq(null, planStage);
    assert.eq(planStage.collation, {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    // Explain of IXSCAN stage should include index collation when index collation is inherited from
    // collection default.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
    assert.commandWorked(coll.createIndex({str: 1}));
    explainRes = coll.explain("executionStats").find({str: "foo"}).finish();
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IXSCAN");
    assert.neq(null, planStage);
    assert.eq(planStage.collation, {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    });

    if (!db.getMongo().useReadCommands()) {
        // find() shell helper should error if a collation is specified and the shell is not using
        // read commands.
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "bar"}));
        assert.throws(function() {
            coll.find().collation({locale: "fr"}).itcount();
        });
    }

    //
    // Collation tests for findAndModify.
    //

    // findAndModify should return correct results when collation specified and collection does not
    // exist.
    coll.drop();
    assert.eq(null, coll.findAndModify({
        query: {str: "bar"},
        update: {$set: {str: "baz"}},
        new: true,
        collation: {locale: "fr"}
    }));

    // Update-findAndModify should return correct results when collation specified.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    assert.eq({_id: 1, str: "baz"}, coll.findAndModify({
        query: {str: "FOO"},
        update: {$set: {str: "baz"}},
        new: true,
        collation: {locale: "en_US", strength: 2}
    }));

    // Explain of update-findAndModify should return correct results when collation specified.
    explainRes = coll.explain("executionStats").findAndModify({
        query: {str: "BAR"},
        update: {$set: {str: "baz"}},
        new: true,
        collation: {locale: "en_US", strength: 2}
    });
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "UPDATE");
    assert.neq(null, planStage);
    assert.eq(1, planStage.nWouldModify);

    // Delete-findAndModify should return correct results when collation specified.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    assert.eq({_id: 1, str: "foo"},
              coll.findAndModify(
                  {query: {str: "FOO"}, remove: true, collation: {locale: "en_US", strength: 2}}));

    // Explain of delete-findAndModify should return correct results when collation specified.
    explainRes = coll.explain("executionStats").findAndModify({
        query: {str: "BAR"},
        remove: true,
        collation: {locale: "en_US", strength: 2}
    });
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "DELETE");
    assert.neq(null, planStage);
    assert.eq(1, planStage.nWouldDelete);

    // findAndModify should return correct results when no collation specified and collection has a
    // default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.eq({_id: 1, str: "foo"},
              coll.findAndModify({query: {str: "FOO"}, update: {$set: {x: 1}}}));
    assert.eq({_id: 1, str: "foo", x: 1}, coll.findAndModify({query: {str: "FOO"}, remove: true}));

    // findAndModify should return correct results when "simple" collation specified and collection
    // has a default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.eq(null,
              coll.findAndModify(
                  {query: {str: "FOO"}, update: {$set: {x: 1}}, collation: {locale: "simple"}}));
    assert.eq(
        null,
        coll.findAndModify({query: {str: "FOO"}, remove: true, collation: {locale: "simple"}}));

    //
    // Collation tests for group.
    //

    // Group should return correct results when collation specified and collection does not exist.
    coll.drop();
    assert.eq([], coll.group({
        key: {str: 1},
        initial: {count: 0},
        reduce: function(curr, result) {
            result.count += 1;
        },
        collation: {locale: "fr"}
    }));

    // Group should return correct results when collation specified and no indexes exist.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    assert.eq([{str: "foo", count: 1}], coll.group({
        cond: {str: "FOO"},
        key: {str: 1},
        initial: {count: 0},
        reduce: function(curr, result) {
            result.count += 1;
        },
        collation: {locale: "en_US", strength: 2}
    }));

    // Group should return correct results when no collation specified and collection has a default
    // collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.eq([{str: "foo", count: 1}], coll.group({
        cond: {str: "FOO"},
        key: {str: 1},
        initial: {count: 0},
        reduce: function(curr, result) {
            result.count += 1;
        }
    }));

    // Group should return correct results when "simple" collation specified and collection has a
    // default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.eq([], coll.group({
        cond: {str: "FOO"},
        key: {str: 1},
        initial: {count: 0},
        reduce: function(curr, result) {
            result.count += 1;
        },
        collation: {locale: "simple"}
    }));

    // Explain of group should return correct results when collation specified.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    explainRes = coll.explain("executionStats").group({
        cond: {str: "FOO"},
        key: {str: 1},
        initial: {count: 0},
        reduce: function(curr, result) {
            result.count += 1;
        },
        collation: {locale: "en_US", strength: 2}
    });
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "GROUP");
    assert.neq(null, planStage);
    assert.eq(planStage.nGroups, 1);

    //
    // Collation tests for mapReduce.
    //

    // mapReduce should return "collection doesn't exist" error when collation specified and
    // collection does not exist.
    coll.drop();
    assert.throws(function() {
        coll.mapReduce(
            function() {
                emit(this.str, 1);
            },
            function(key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, collation: {locale: "fr"}});
    });

    // mapReduce should return correct results when collation specified and no indexes exist.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    var mapReduceOut = coll.mapReduce(
        function() {
            emit(this.str, 1);
        },
        function(key, values) {
            return Array.sum(values);
        },
        {out: {inline: 1}, query: {str: "FOO"}, collation: {locale: "en_US", strength: 2}});
    assert.commandWorked(mapReduceOut);
    assert.eq(mapReduceOut.results.length, 1);

    // mapReduce should return correct results when no collation specified and collection has a
    // default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    var mapReduceOut = coll.mapReduce(
        function() {
            emit(this.str, 1);
        },
        function(key, values) {
            return Array.sum(values);
        },
        {out: {inline: 1}, query: {str: "FOO"}});
    assert.commandWorked(mapReduceOut);
    assert.eq(mapReduceOut.results.length, 1);

    // mapReduce should return correct results when "simple" collation specified and collection has
    // a default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    var mapReduceOut = coll.mapReduce(
        function() {
            emit(this.str, 1);
        },
        function(key, values) {
            return Array.sum(values);
        },
        {out: {inline: 1}, query: {str: "FOO"}, collation: {locale: "simple"}});
    assert.commandWorked(mapReduceOut);
    assert.eq(mapReduceOut.results.length, 0);

    //
    // Collation tests for remove.
    //

    if (db.getMongo().writeMode() === "commands") {
        // Remove should succeed when collation specified and collection does not exist.
        coll.drop();
        assert.writeOK(coll.remove({str: "foo"}, {justOne: true, collation: {locale: "fr"}}));

        // Remove should return correct results when collation specified.
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        writeRes =
            coll.remove({str: "FOO"}, {justOne: true, collation: {locale: "en_US", strength: 2}});
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nRemoved);

        // Explain of remove should return correct results when collation specified.
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        explainRes = coll.explain("executionStats").remove({str: "FOO"}, {
            justOne: true,
            collation: {locale: "en_US", strength: 2}
        });
        assert.commandWorked(explainRes);
        planStage = getPlanStage(explainRes.executionStats.executionStages, "DELETE");
        assert.neq(null, planStage);
        assert.eq(1, planStage.nWouldDelete);
    }

    // Remove should return correct results when no collation specified and collection has a default
    // collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    writeRes = coll.remove({str: "FOO"}, {justOne: true});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nRemoved);

    // Remove with idhack should return correct results when no collation specified and collection
    // has a default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: "foo"}));
    writeRes = coll.remove({_id: "FOO"}, {justOne: true});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nRemoved);

    // Remove on _id should use idhack stage when query inherits collection default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").remove({_id: "foo"});
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
    assert.neq(null, planStage);

    if (db.getMongo().writeMode() === "commands") {
        // Remove should return correct results when "simple" collation specified and collection has
        // a default collation.
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        writeRes = coll.remove({str: "FOO"}, {justOne: true, collation: {locale: "simple"}});
        assert.writeOK(writeRes);
        assert.eq(0, writeRes.nRemoved);

        // Remove on _id should return correct results when "simple" collation specified and
        // collection has a default collation.
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
        assert.writeOK(coll.insert({_id: "foo"}));
        writeRes = coll.remove({_id: "FOO"}, {justOne: true, collation: {locale: "simple"}});
        assert.writeOK(writeRes);
        assert.eq(0, writeRes.nRemoved);

        // Remove on _id should use idhack stage when explicit query collation matches collection
        // default.
        coll.drop();
        assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
        explainRes =
            coll.explain("executionStats").remove({_id: "foo"}, {collation: {locale: "en_US"}});
        assert.commandWorked(explainRes);
        planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
        assert.neq(null, planStage);

        // Remove on _id should not use idhack stage when query collation does not match collection
        // default.
        coll.drop();
        assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
        explainRes =
            coll.explain("executionStats").remove({_id: "foo"}, {collation: {locale: "fr_CA"}});
        assert.commandWorked(explainRes);
        planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
        assert.eq(null, planStage);
    }

    if (db.getMongo().writeMode() !== "commands") {
        // remove() shell helper should error if a collation is specified and the shell is not using
        // write commands.
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        assert.throws(function() {
            coll.remove({str: "FOO"}, {justOne: true, collation: {locale: "en_US", strength: 2}});
        });
        assert.throws(function() {
            coll.explain().remove({str: "FOO"},
                                  {justOne: true, collation: {locale: "en_US", strength: 2}});
        });
    }

    //
    // Collation tests for update.
    //

    if (db.getMongo().writeMode() === "commands") {
        // Update should succeed when collation specified and collection does not exist.
        coll.drop();
        assert.writeOK(coll.update(
            {str: "foo"}, {$set: {other: 99}}, {multi: true, collation: {locale: "fr"}}));

        // Update should return correct results when collation specified.
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        writeRes = coll.update({str: "FOO"},
                               {$set: {other: 99}},
                               {multi: true, collation: {locale: "en_US", strength: 2}});
        assert.eq(2, writeRes.nModified);

        // Explain of update should return correct results when collation specified.
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        explainRes = coll.explain("executionStats").update({str: "FOO"}, {$set: {other: 99}}, {
            multi: true,
            collation: {locale: "en_US", strength: 2}
        });
        assert.commandWorked(explainRes);
        planStage = getPlanStage(explainRes.executionStats.executionStages, "UPDATE");
        assert.neq(null, planStage);
        assert.eq(2, planStage.nWouldModify);
    }

    // Update should return correct results when no collation specified and collection has a default
    // collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    writeRes = coll.update({str: "FOO"}, {$set: {other: 99}});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nMatched);

    // Update with idhack should return correct results when no collation specified and collection
    // has a default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.writeOK(coll.insert({_id: "foo"}));
    writeRes = coll.update({_id: "FOO"}, {$set: {other: 99}});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nMatched);

    // Update on _id should use idhack stage when query inherits collection default collation.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").update({_id: "foo"}, {$set: {other: 99}});
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
    assert.neq(null, planStage);

    if (db.getMongo().writeMode() === "commands") {
        // Update should return correct results when "simple" collation specified and collection has
        // a default collation.
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        writeRes = coll.update({str: "FOO"}, {$set: {other: 99}}, {collation: {locale: "simple"}});
        assert.writeOK(writeRes);
        assert.eq(0, writeRes.nModified);

        // Update on _id should return correct results when "simple" collation specified and
        // collection has a default collation.
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
        assert.writeOK(coll.insert({_id: "foo"}));
        writeRes = coll.update({_id: "FOO"}, {$set: {other: 99}}, {collation: {locale: "simple"}});
        assert.writeOK(writeRes);
        assert.eq(0, writeRes.nModified);

        // Update on _id should use idhack stage when explicitly given query collation matches
        // collection default.
        coll.drop();
        assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
        explainRes = coll.explain("executionStats").update({_id: "foo"}, {$set: {other: 99}}, {
            collation: {locale: "en_US"}
        });
        assert.commandWorked(explainRes);
        planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
        assert.neq(null, planStage);

        // Update on _id should not use idhack stage when query collation does not match collection
        // default.
        coll.drop();
        assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
        explainRes = coll.explain("executionStats").update({_id: "foo"}, {$set: {other: 99}}, {
            collation: {locale: "fr_CA"}
        });
        assert.commandWorked(explainRes);
        planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
        assert.eq(null, planStage);
    }

    if (db.getMongo().writeMode() !== "commands") {
        // update() shell helper should error if a collation is specified and the shell is not using
        // write commands.
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        assert.throws(function() {
            coll.update({str: "FOO"},
                        {$set: {other: 99}},
                        {multi: true, collation: {locale: "en_US", strength: 2}});
        });
        assert.throws(function() {
            coll.explain().update({str: "FOO"},
                                  {$set: {other: 99}},
                                  {multi: true, collation: {locale: "en_US", strength: 2}});
        });
    }

    //
    // Collation tests for geoNear.
    //

    // geoNear should return "collection doesn't exist" error when collation specified and
    // collection does not exist.
    coll.drop();
    assert.commandFailed(db.runCommand({
        geoNear: coll.getName(),
        near: {type: "Point", coordinates: [0, 0]},
        spherical: true,
        query: {str: "ABC"},
        collation: {locale: "en_US", strength: 2}
    }));

    // geoNear should return correct results when collation specified and string predicate not
    // indexed.
    coll.drop();
    assert.writeOK(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, str: "abc"}));
    assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));
    assert.eq(0,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "ABC"}
                  }))
                  .results.length);
    assert.eq(1,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "ABC"},
                      collation: {locale: "en_US", strength: 2}
                  }))
                  .results.length);

    // geoNear should return correct results when no collation specified and string predicate
    // indexed.
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.ensureIndex({geo: "2dsphere", str: 1}));
    assert.eq(0,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "ABC"}
                  }))
                  .results.length);
    assert.eq(1,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "ABC"},
                      collation: {locale: "en_US", strength: 2}
                  }))
                  .results.length);

    // geoNear should return correct results when collation specified and collation on index is
    // incompatible with string predicate.
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(
        coll.ensureIndex({geo: "2dsphere", str: 1}, {collation: {locale: "en_US", strength: 3}}));
    assert.eq(0,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "ABC"}
                  }))
                  .results.length);
    assert.eq(1,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "ABC"},
                      collation: {locale: "en_US", strength: 2}
                  }))
                  .results.length);

    // geoNear should return correct results when collation specified and collation on index is
    // compatible with string predicate.
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(
        coll.ensureIndex({geo: "2dsphere", str: 1}, {collation: {locale: "en_US", strength: 2}}));
    assert.eq(0,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "ABC"}
                  }))
                  .results.length);
    assert.eq(1,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "ABC"},
                      collation: {locale: "en_US", strength: 2}
                  }))
                  .results.length);

    // geoNear should return correct results when no collation specified and collection has a
    // default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));
    assert.writeOK(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, str: "abc"}));
    assert.eq(1,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "ABC"}
                  }))
                  .results.length);

    // geoNear should return correct results when "simple" collation specified and collection has
    // a default collation.
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));
    assert.writeOK(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, str: "abc"}));
    assert.eq(0,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {str: "ABC"},
                      collation: {locale: "simple"}
                  }))
                  .results.length);

    //
    // Collation tests for find with $nearSphere.
    //

    if (db.getMongo().useReadCommands()) {
        // Find with $nearSphere should return correct results when collation specified and
        // collection does not exist.
        coll.drop();
        assert.eq(0,
                  coll.find({
                          str: "ABC",
                          geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}
                      })
                      .collation({locale: "en_US", strength: 2})
                      .itcount());

        // Find with $nearSphere should return correct results when collation specified and string
        // predicate not indexed.
        coll.drop();
        assert.writeOK(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, str: "abc"}));
        assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));
        assert.eq(0,
                  coll.find({
                          str: "ABC",
                          geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}
                      })
                      .itcount());
        assert.eq(1,
                  coll.find({
                          str: "ABC",
                          geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}
                      })
                      .collation({locale: "en_US", strength: 2})
                      .itcount());

        // Find with $nearSphere should return correct results when no collation specified and
        // string predicate indexed.
        assert.commandWorked(coll.dropIndexes());
        assert.commandWorked(coll.ensureIndex({geo: "2dsphere", str: 1}));
        assert.eq(0,
                  coll.find({
                          str: "ABC",
                          geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}
                      })
                      .itcount());
        assert.eq(1,
                  coll.find({
                          str: "ABC",
                          geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}
                      })
                      .collation({locale: "en_US", strength: 2})
                      .itcount());

        // Find with $nearSphere should return correct results when collation specified and
        // collation on index is incompatible with string predicate.
        assert.commandWorked(coll.dropIndexes());
        assert.commandWorked(coll.ensureIndex({geo: "2dsphere", str: 1},
                                              {collation: {locale: "en_US", strength: 3}}));
        assert.eq(0,
                  coll.find({
                          str: "ABC",
                          geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}
                      })
                      .itcount());
        assert.eq(1,
                  coll.find({
                          str: "ABC",
                          geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}
                      })
                      .collation({locale: "en_US", strength: 2})
                      .itcount());

        // Find with $nearSphere should return correct results when collation specified and
        // collation on index is compatible with string predicate.
        assert.commandWorked(coll.dropIndexes());
        assert.commandWorked(coll.ensureIndex({geo: "2dsphere", str: 1},
                                              {collation: {locale: "en_US", strength: 2}}));
        assert.eq(0,
                  coll.find({
                          str: "ABC",
                          geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}
                      })
                      .itcount());
        assert.eq(1,
                  coll.find({
                          str: "ABC",
                          geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}
                      })
                      .collation({locale: "en_US", strength: 2})
                      .itcount());
    }

    //
    // Tests for the bulk API.
    //

    var bulk;

    if (db.getMongo().writeMode() !== "commands") {
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));

        // Can't use the bulk API to set a collation when using legacy write ops.
        bulk = coll.initializeUnorderedBulkOp();
        assert.throws(function() {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2});
        });

        bulk = coll.initializeOrderedBulkOp();
        assert.throws(function() {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2});
        });
    } else {
        // update().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).update({
            $set: {other: 99}
        });
        writeRes = bulk.execute();
        assert.writeOK(writeRes);
        assert.eq(2, writeRes.nModified);

        // updateOne().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).updateOne({
            $set: {other: 99}
        });
        writeRes = bulk.execute();
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nModified);

        // replaceOne().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).replaceOne({str: "oof"});
        writeRes = bulk.execute();
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nModified);

        // replaceOne() with upsert().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "FOO"}).collation({locale: "en_US"}).upsert().replaceOne({str: "foo"});
        writeRes = bulk.execute();
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nUpserted);
        assert.eq(0, writeRes.nModified);

        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).upsert().replaceOne({
            str: "foo"
        });
        writeRes = bulk.execute();
        assert.writeOK(writeRes);
        assert.eq(0, writeRes.nUpserted);
        assert.eq(1, writeRes.nModified);

        // removeOne().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).removeOne();
        writeRes = bulk.execute();
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nRemoved);

        // remove().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).remove();
        writeRes = bulk.execute();
        assert.writeOK(writeRes);
        assert.eq(2, writeRes.nRemoved);
    }

    //
    // Tests for the CRUD API.
    //

    // deleteOne().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.deleteOne({str: "FOO"}, {collation: {locale: "en_US", strength: 2}});
        assert.eq(1, res.deletedCount);
    } else {
        assert.throws(function() {
            coll.deleteOne({str: "FOO"}, {collation: {locale: "en_US", strength: 2}});
        });
    }

    // deleteMany().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.deleteMany({str: "FOO"}, {collation: {locale: "en_US", strength: 2}});
        assert.eq(2, res.deletedCount);
    } else {
        assert.throws(function() {
            coll.deleteMany({str: "FOO"}, {collation: {locale: "en_US", strength: 2}});
        });
    }

    // findOneAndDelete().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.eq({_id: 1, str: "foo"},
              coll.findOneAndDelete({str: "FOO"}, {collation: {locale: "en_US", strength: 2}}));
    assert.eq(null, coll.findOne({_id: 1}));

    // findOneAndReplace().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.eq({_id: 1, str: "foo"},
              coll.findOneAndReplace(
                  {str: "FOO"}, {str: "bar"}, {collation: {locale: "en_US", strength: 2}}));
    assert.neq(null, coll.findOne({str: "bar"}));

    // findOneAndUpdate().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.eq({_id: 1, str: "foo"},
              coll.findOneAndUpdate(
                  {str: "FOO"}, {$set: {other: 99}}, {collation: {locale: "en_US", strength: 2}}));
    assert.neq(null, coll.findOne({other: 99}));

    // replaceOne().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.replaceOne(
            {str: "FOO"}, {str: "bar"}, {collation: {locale: "en_US", strength: 2}});
        assert.eq(1, res.modifiedCount);
    } else {
        assert.throws(function() {
            coll.replaceOne(
                {str: "FOO"}, {str: "bar"}, {collation: {locale: "en_US", strength: 2}});
        });
    }

    // updateOne().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.updateOne(
            {str: "FOO"}, {$set: {other: 99}}, {collation: {locale: "en_US", strength: 2}});
        assert.eq(1, res.modifiedCount);
    } else {
        assert.throws(function() {
            coll.updateOne(
                {str: "FOO"}, {$set: {other: 99}}, {collation: {locale: "en_US", strength: 2}});
        });
    }

    // updateMany().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.updateMany(
            {str: "FOO"}, {$set: {other: 99}}, {collation: {locale: "en_US", strength: 2}});
        assert.eq(2, res.modifiedCount);
    } else {
        assert.throws(function() {
            coll.updateMany(
                {str: "FOO"}, {$set: {other: 99}}, {collation: {locale: "en_US", strength: 2}});
        });
    }

    // updateOne with bulkWrite().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.bulkWrite([{
            updateOne: {
                filter: {str: "FOO"},
                update: {$set: {other: 99}},
                collation: {locale: "en_US", strength: 2}
            }
        }]);
        assert.eq(1, res.matchedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite([{
                updateOne: {
                    filter: {str: "FOO"},
                    update: {$set: {other: 99}},
                    collation: {locale: "en_US", strength: 2}
                }
            }]);
        });
    }

    // updateMany with bulkWrite().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.bulkWrite([{
            updateMany: {
                filter: {str: "FOO"},
                update: {$set: {other: 99}},
                collation: {locale: "en_US", strength: 2}
            }
        }]);
        assert.eq(2, res.matchedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite([{
                updateMany: {
                    filter: {str: "FOO"},
                    update: {$set: {other: 99}},
                    collation: {locale: "en_US", strength: 2}
                }
            }]);
        });
    }

    // replaceOne with bulkWrite().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.bulkWrite([{
            replaceOne: {
                filter: {str: "FOO"},
                replacement: {str: "bar"},
                collation: {locale: "en_US", strength: 2}
            }
        }]);
        assert.eq(1, res.matchedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite([{
                replaceOne: {
                    filter: {str: "FOO"},
                    replacement: {str: "bar"},
                    collation: {locale: "en_US", strength: 2}
                }
            }]);
        });
    }

    // deleteOne with bulkWrite().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.bulkWrite(
            [{deleteOne: {filter: {str: "FOO"}, collation: {locale: "en_US", strength: 2}}}]);
        assert.eq(1, res.deletedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite(
                [{deleteOne: {filter: {str: "FOO"}, collation: {locale: "en_US", strength: 2}}}]);
        });
    }

    // deleteMany with bulkWrite().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.bulkWrite(
            [{deleteMany: {filter: {str: "FOO"}, collation: {locale: "en_US", strength: 2}}}]);
        assert.eq(2, res.deletedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite(
                [{deleteMany: {filter: {str: "FOO"}, collation: {locale: "en_US", strength: 2}}}]);
        });
    }

    // Two deleteOne ops with bulkWrite using different collations.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.bulkWrite([
            {deleteOne: {filter: {str: "FOO"}, collation: {locale: "fr", strength: 2}}},
            {deleteOne: {filter: {str: "BAR"}, collation: {locale: "en_US", strength: 2}}}
        ]);
        assert.eq(2, res.deletedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite([
                {deleteOne: {filter: {str: "FOO"}, collation: {locale: "fr", strength: 2}}},
                {deleteOne: {filter: {str: "BAR"}, collation: {locale: "en_US", strength: 2}}}
            ]);
        });
    }

    // applyOps.
    if (!isMongos) {
        coll.drop();
        assert.commandWorked(
            db.createCollection("collation", {collation: {locale: "en_US", strength: 2}}));
        assert.writeOK(coll.insert({_id: "foo", x: 5, str: "bar"}));

        // preCondition.q respects collection default collation.
        assert.commandFailed(db.runCommand({
            applyOps: [{op: "u", ns: coll.getFullName(), o2: {_id: "foo"}, o: {$set: {x: 6}}}],
            preCondition: [{ns: coll.getFullName(), q: {_id: "not foo"}, res: {str: "bar"}}]
        }));
        assert.eq(5, coll.findOne({_id: "foo"}).x);
        assert.commandWorked(db.runCommand({
            applyOps: [{op: "u", ns: coll.getFullName(), o2: {_id: "foo"}, o: {$set: {x: 6}}}],
            preCondition: [{ns: coll.getFullName(), q: {_id: "FOO"}, res: {str: "bar"}}]
        }));
        assert.eq(6, coll.findOne({_id: "foo"}).x);

        // preCondition.res respects collection default collation.
        assert.commandFailed(db.runCommand({
            applyOps: [{op: "u", ns: coll.getFullName(), o2: {_id: "foo"}, o: {$set: {x: 7}}}],
            preCondition: [{ns: coll.getFullName(), q: {_id: "foo"}, res: {str: "not bar"}}]
        }));
        assert.eq(6, coll.findOne({_id: "foo"}).x);
        assert.commandWorked(db.runCommand({
            applyOps: [{op: "u", ns: coll.getFullName(), o2: {_id: "foo"}, o: {$set: {x: 7}}}],
            preCondition: [{ns: coll.getFullName(), q: {_id: "foo"}, res: {str: "BAR"}}]
        }));
        assert.eq(7, coll.findOne({_id: "foo"}).x);

        // <operation>.o2 respects collection default collation.
        assert.commandWorked(db.runCommand(
            {applyOps: [{op: "u", ns: coll.getFullName(), o2: {_id: "FOO"}, o: {$set: {x: 8}}}]}));
        assert.eq(8, coll.findOne({_id: "foo"}).x);
    }
})();
