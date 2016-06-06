// Integration tests for the collation feature.
(function() {
    'use strict';

    load("jstests/libs/analyze_plan.js");

    var coll = db.collation;
    coll.drop();

    var explainRes;
    var planStage;

    var assertIndexHasCollation = function(keyPattern, collation) {
        var foundIndex = false;
        var indexSpecs = coll.getIndexes();
        for (var i = 0; i < indexSpecs.length; ++i) {
            if (bsonWoCompare(indexSpecs[i].key, keyPattern) === 0) {
                foundIndex = true;
                // We assume that the key pattern is unique, even though indices with different
                // collations but the same key pattern are allowed.
                assert.eq(indexSpecs[i].collation, collation);
            }
        }
        assert(foundIndex, "index with key pattern " + tojson(keyPattern) + " not found");
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

    // Ensure we can create a collection with the "simple" collation as the collection default.
    assert.commandWorked(db.createCollection("collation", {collation: {locale: "simple"}}));
    var collectionInfos = db.getCollectionInfos({name: "collation"});
    assert.eq(collectionInfos.length, 1);
    assert.eq(collectionInfos[0].options.collation, {locale: "simple"});
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
        backwards: true
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
        backwards: true
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
        backwards: false
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
        backwards: false
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
        backwards: false
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
        backwards: true
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
        backwards: true
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
    // Test helpers for operations that accept a collation.
    //

    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));

    // Aggregation.
    assert.eq(0, coll.aggregate([{$match: {str: "FOO"}}]).itcount());
    assert.eq(1,
              coll.aggregate([{$match: {str: "FOO"}}], {collation: {locale: "en_US", strength: 2}})
                  .itcount());
    assert.commandWorked(coll.explain().aggregate([], {collation: {locale: "fr"}}));

    // Count command.
    assert.eq(0, coll.find({str: "FOO"}).count());
    assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).count());
    assert.eq(1, coll.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).count());
    assert.eq(0, coll.count({str: "FOO"}));
    assert.eq(0, coll.count({str: "FOO"}, {collation: {locale: "en_US"}}));
    assert.eq(1, coll.count({str: "FOO"}, {collation: {locale: "en_US", strength: 2}}));

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

    // Distinct.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "FOO"}));

    // Without an index.
    var res = coll.distinct("str", {}, {collation: {locale: "en_US", strength: 2}});
    assert.eq(1, res.length);
    assert.eq("foo", res[0].toLowerCase());
    assert.eq(2, coll.distinct("str", {}, {collation: {locale: "en_US", strength: 3}}).length);
    assert.eq(
        2, coll.distinct("_id", {str: "foo"}, {collation: {locale: "en_US", strength: 2}}).length);

    // With an index.
    coll.createIndex({str: 1}, {collation: {locale: "en_US", strength: 2}});
    res = coll.distinct("str", {}, {collation: {locale: "en_US", strength: 2}});
    assert.eq(1, res.length);
    assert.eq("foo", res[0].toLowerCase());
    assert.eq(2, coll.distinct("str", {}, {collation: {locale: "en_US", strength: 3}}).length);

    assert.commandWorked(coll.explain().distinct("str", {}, {collation: {locale: "fr"}}));

    // Find command.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    if (db.getMongo().useReadCommands()) {
        // On _id field.
        assert.writeOK(coll.insert({_id: "foo"}));
        assert.eq(0, coll.find({_id: "FOO"}).itcount());
        assert.eq(0, coll.find({_id: "FOO"}).collation({locale: "en_US"}).itcount());
        assert.eq(1, coll.find({_id: "FOO"}).collation({locale: "en_US", strength: 2}).itcount());
        assert.writeOK(coll.remove({_id: "foo"}));

        // Without an index.
        assert.eq(0, coll.find({str: "FOO"}).itcount());
        assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).itcount());
        assert.eq(1, coll.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).itcount());

        // With an index.
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

        // With a partial index. {_id: 1, str: "foo"} will be indexed even though "foo" > "FOO",
        // since the collation is case-insensitive.
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
    } else {
        assert.throws(function() {
            coll.find().collation({locale: "fr"}).itcount();
        });
    }

    // Explain of find always uses the find command, so this will succeed regardless of readMode.
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

    // Update via findAndModify.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    assert.eq({_id: 1, str: "baz"}, coll.findAndModify({
        query: {str: "FOO"},
        update: {$set: {str: "baz"}},
        new: true,
        collation: {locale: "en_US", strength: 2}
    }));
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

    // Delete via findAndModify.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    assert.eq({_id: 1, str: "foo"},
              coll.findAndModify(
                  {query: {str: "FOO"}, remove: true, collation: {locale: "en_US", strength: 2}}));
    explainRes = coll.explain("executionStats").findAndModify({
        query: {str: "BAR"},
        remove: true,
        collation: {locale: "en_US", strength: 2}
    });
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "DELETE");
    assert.neq(null, planStage);
    assert.eq(1, planStage.nWouldDelete);

    // Group.
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

    // mapReduce.
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

    // Remove.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        explainRes = coll.explain("executionStats").remove({str: "FOO"}, {
            justOne: true,
            collation: {locale: "en_US", strength: 2}
        });
        assert.commandWorked(explainRes);
        planStage = getPlanStage(explainRes.executionStats.executionStages, "DELETE");
        assert.neq(null, planStage);
        assert.eq(1, planStage.nWouldDelete);

        var writeRes =
            coll.remove({str: "FOO"}, {justOne: true, collation: {locale: "en_US", strength: 2}});
        assert.writeOK(writeRes);
        assert.eq(1, writeRes.nRemoved);
    } else {
        assert.throws(function() {
            coll.remove({str: "FOO"}, {justOne: true, collation: {locale: "en_US", strength: 2}});
        });
        assert.throws(function() {
            coll.explain().remove({str: "FOO"},
                                  {justOne: true, collation: {locale: "en_US", strength: 2}});
        });
    }

    // Update.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        explainRes = coll.explain("executionStats").update({str: "FOO"}, {$set: {other: 99}}, {
            multi: true,
            collation: {locale: "en_US", strength: 2}
        });
        assert.commandWorked(explainRes);
        planStage = getPlanStage(explainRes.executionStats.executionStages, "UPDATE");
        assert.neq(null, planStage);
        assert.eq(2, planStage.nWouldModify);

        var writeRes = coll.update({str: "FOO"},
                                   {$set: {other: 99}},
                                   {multi: true, collation: {locale: "en_US", strength: 2}});
        assert.eq(2, writeRes.nModified);
    } else {
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

    // geoNear.
    coll.drop();
    assert.writeOK(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, str: "abc"}));

    // String field not indexed.
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

    // String field indexed without collation.
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

    // String field indexed with non-matching collation.
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

    // String field indexed with matching collation.
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

    coll.drop();

    // $nearSphere.
    if (db.getMongo().useReadCommands()) {
        assert.writeOK(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, str: "abc"}));

        // String field not indexed.
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

        // String field indexed without collation.
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

        // String field indexed with non-matching collation.
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

        // String field indexed with matching collation.
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

        coll.drop();
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
        var writeRes;

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
})();
