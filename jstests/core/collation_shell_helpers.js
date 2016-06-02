// Ensure that shell helpers correctly deliver the collation to the server.
//
// TODO SERVER-23791: Once we have an end-to-end working on mongod, we should be able to strengthen
// the assertions in this file in order to ensure that the server is correctly respecting the
// assertion. Currently we exercise the code paths in the shell that are supposed to propagate the
// collation to the server, but we don't require that the result of executing the command respects
// the collation.
(function() {
    'use strict';

    var coll = db.collation_shell_helpers;
    coll.drop();

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
    assert.commandFailed(
        db.createCollection("collation_shell_helpers", {collation: "not an object"}));
    assert.commandFailed(db.createCollection("collation_shell_helpers", {collation: {}}));
    assert.commandFailed(db.createCollection("collation_shell_helpers", {collation: {blah: 1}}));
    assert.commandFailed(
        db.createCollection("collation_shell_helpers", {collation: {locale: "en", blah: 1}}));
    assert.commandFailed(
        db.createCollection("collation_shell_helpers", {collation: {locale: "xx"}}));
    assert.commandFailed(
        db.createCollection("collation_shell_helpers", {collation: {locale: "en", strength: 99}}));

    // Ensure we can create a collection with the "simple" collation as the collection default.
    assert.commandWorked(
        db.createCollection("collation_shell_helpers", {collation: {locale: "simple"}}));
    var collectionInfos = db.getCollectionInfos({name: "collation_shell_helpers"});
    assert.eq(collectionInfos.length, 1);
    assert.eq(collectionInfos[0].options.collation, {locale: "simple"});
    coll.drop();

    // Ensure that we populate all collation-related fields when we create a collection with a valid
    // collation.
    assert.commandWorked(
        db.createCollection("collation_shell_helpers", {collation: {locale: "fr_CA"}}));
    var collectionInfos = db.getCollectionInfos({name: "collation_shell_helpers"});
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

    // TODO SERVER-23791: Test that queries with matching collations can use these indices, and that
    // the indices contain collator-generated comparison keys rather than the verbatim indexed
    // strings.

    //
    // Test helpers for operations that accept a collation.
    //

    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));

    // Aggregation.
    assert.eq(2, coll.aggregate([], {collation: {locale: "fr"}}).itcount());
    assert.commandWorked(coll.explain().aggregate([], {collation: {locale: "fr"}}));

    // Count command.
    assert.eq(0, coll.find({str: "FOO"}).count());
    assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).count());
    assert.eq(1, coll.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).count());
    assert.eq(0, coll.count({str: "FOO"}));
    assert.eq(0, coll.count({str: "FOO"}, {collation: {locale: "en_US"}}));
    assert.eq(1, coll.count({str: "FOO"}, {collation: {locale: "en_US", strength: 2}}));
    assert.commandWorked(coll.explain().find().collation({locale: "fr"}).count());

    // Distinct.
    assert.eq(["foo", "bar"], coll.distinct("str", {}, {collation: {locale: "fr"}}));
    assert.commandWorked(coll.explain().distinct("str", {}, {collation: {locale: "fr"}}));

    // Find command.
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

        // With a partial index.
        // {_id: 1, str: "foo"} will be indexed even though "foo" > "FOO", since the collation is
        // case-insensitive.
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
    assert.commandWorked(coll.explain().find().collation({locale: "fr"}).finish());
    assert.commandWorked(coll.find().collation({locale: "fr"}).explain());

    // findAndModify.
    assert.eq({_id: 2, str: "baz"}, coll.findAndModify({
        query: {str: "bar"},
        update: {$set: {str: "baz"}},
        new: true,
        collation: {locale: "fr"}
    }));
    assert.commandWorked(coll.explain().findAndModify(
        {query: {str: "bar"}, update: {$set: {str: "baz"}}, new: true, collation: {locale: "fr"}}));

    // Group.
    assert.eq([{str: "foo", count: 1}, {str: "baz", count: 1}], coll.group({
        key: {str: 1},
        initial: {count: 0},
        reduce: function(curr, result) {
            result.count += 1;
        },
        collation: {locale: "fr"}
    }));
    assert.commandWorked(coll.explain().group({
        key: {str: 1},
        initial: {count: 0},
        reduce: function(curr, result) {
            result.count += 1;
        },
        collation: {locale: "fr"}
    }));

    // mapReduce.
    var mapReduceOut = coll.mapReduce(
        function() {
            emit(this.str, 1);
        },
        function(key, values) {
            return Array.sum(values);
        },
        {out: {inline: 1}, collation: {locale: "fr"}});
    assert.commandWorked(mapReduceOut);
    assert.eq(mapReduceOut.results.length, 2);

    // Remove.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        assert.commandWorked(
            coll.explain().remove({str: "foo"}, {justOne: true, collation: {locale: "fr"}}));
        assert.writeOK(coll.remove({str: "foo"}, {justOne: true, collation: {locale: "fr"}}));
    } else {
        assert.throws(function() {
            coll.remove({str: "foo"}, {justOne: true, collation: {locale: "fr"}});
        });
        assert.throws(function() {
            coll.explain().remove({str: "foo"}, {justOne: true, collation: {locale: "fr"}});
        });
    }

    // Update.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        assert.commandWorked(coll.explain().update(
            {str: "foo"}, {$set: {other: 99}}, {multi: true, collation: {locale: "fr"}}));
        assert.writeOK(coll.update(
            {str: "foo"}, {$set: {other: 99}}, {multi: true, collation: {locale: "fr"}}));
    } else {
        assert.throws(function() {
            coll.update(
                {str: "foo"}, {$set: {other: 99}}, {multi: true, collation: {locale: "fr"}});
        });
        assert.throws(function() {
            coll.explain().update(
                {str: "foo"}, {$set: {other: 99}}, {multi: true, collation: {locale: "fr"}});
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
        // Can't use the bulk API to set a collation when using legacy write ops.
        bulk = coll.initializeUnorderedBulkOp();
        assert.throws(function() {
            bulk.find({str: "foo"}).collation({locale: "fr"});
        });

        bulk = coll.initializeOrderedBulkOp();
        assert.throws(function() {
            bulk.find({str: "foo"}).collation({locale: "en_US"});
        });
    } else {
        // update().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "foo"}).collation({locale: "fr"}).update({$set: {other: 99}});
        assert.writeOK(bulk.execute());

        // updateOne().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "foo"}).collation({locale: "fr"}).updateOne({$set: {other: 99}});
        assert.writeOK(bulk.execute());

        // replaceOne().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "foo"}).collation({locale: "fr"}).replaceOne({str: "oof"});
        assert.writeOK(bulk.execute());

        // replaceOne() with upsert().
        coll.drop();
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "foo"}).collation({locale: "fr"}).upsert().replaceOne({str: "foo"});
        assert.writeOK(bulk.execute());

        // removeOne().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "foo"}).collation({locale: "fr"}).removeOne();
        assert.writeOK(bulk.execute());

        // remove().
        coll.drop();
        assert.writeOK(coll.insert({_id: 1, str: "foo"}));
        assert.writeOK(coll.insert({_id: 2, str: "foo"}));
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({str: "foo"}).collation({locale: "fr"}).remove();
        assert.writeOK(bulk.execute());
    }

    //
    // Tests for the CRUD API.
    //

    // deleteOne().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.deleteOne({str: "foo"}, {collation: {locale: "fr"}});
        assert.eq(1, res.deletedCount);
    } else {
        assert.throws(function() {
            coll.deleteOne({str: "foo"}, {collation: {locale: "fr"}});
        });
    }

    // deleteMany().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.deleteMany({str: "foo"}, {collation: {locale: "fr"}});
        assert.eq(2, res.deletedCount);
    } else {
        assert.throws(function() {
            coll.deleteMany({str: "foo"}, {collation: {locale: "fr"}});
        });
    }

    // findOneAndDelete().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.eq({_id: 1, str: "foo"},
              coll.findOneAndDelete({str: "foo"}, {collation: {locale: "fr"}}));
    assert.eq(null, coll.findOne({_id: 1}));

    // findOneAndReplace().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.eq({_id: 1, str: "foo"},
              coll.findOneAndReplace({str: "foo"}, {str: "bar"}, {collation: {locale: "fr"}}));
    assert.neq(null, coll.findOne({str: "bar"}));

    // findOneAndUpdate().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.eq(
        {_id: 1, str: "foo"},
        coll.findOneAndUpdate({str: "foo"}, {$set: {other: 99}}, {collation: {locale: "fr"}}));
    assert.neq(null, coll.findOne({other: 99}));

    // replaceOne().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.replaceOne({str: "foo"}, {str: "bar"}, {collation: {locale: "fr"}});
        assert.eq(1, res.modifiedCount);
    } else {
        assert.throws(function() {
            coll.replaceOne({str: "foo"}, {str: "bar"}, {collation: {locale: "fr"}});
        });
    }

    // updateOne().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.updateOne({str: "foo"}, {$set: {other: 99}}, {collation: {locale: "fr"}});
        assert.eq(1, res.modifiedCount);
    } else {
        assert.throws(function() {
            coll.updateOne({str: "foo"}, {$set: {other: 99}}, {collation: {locale: "fr"}});
        });
    }

    // updateMany().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.updateMany({str: "foo"}, {$set: {other: 99}}, {collation: {locale: "fr"}});
        assert.eq(2, res.modifiedCount);
    } else {
        assert.throws(function() {
            coll.updateMany({str: "foo"}, {$set: {other: 99}}, {collation: {locale: "fr"}});
        });
    }

    // updateOne with bulkWrite().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.bulkWrite([{
            updateOne:
                {filter: {str: "foo"}, update: {$set: {other: 99}}, collation: {locale: "fr"}}
        }]);
        assert.eq(1, res.matchedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite([{
                updateOne: {
                    filter: {str: "foo"},
                    update: {$set: {other: 99}},
                    collation: {locale: "fr"}
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
            updateMany:
                {filter: {str: "foo"}, update: {$set: {other: 99}}, collation: {locale: "fr"}}
        }]);
        assert.eq(2, res.matchedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite([{
                updateMany: {
                    filter: {str: "foo"},
                    update: {$set: {other: 99}},
                    collation: {locale: "fr"}
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
            replaceOne:
                {filter: {str: "foo"}, replacement: {str: "bar"}, collation: {locale: "fr"}}
        }]);
        assert.eq(1, res.matchedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite([{
                replaceOne:
                    {filter: {str: "foo"}, replacement: {str: "bar"}, collation: {locale: "fr"}}
            }]);
        });
    }

    // deleteOne with bulkWrite().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.bulkWrite([{deleteOne: {filter: {str: "foo"}, collation: {locale: "fr"}}}]);
        assert.eq(1, res.deletedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite([{deleteOne: {filter: {str: "foo"}, collation: {locale: "fr"}}}]);
        });
    }

    // deleteMany with bulkWrite().
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "foo"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.bulkWrite([{deleteMany: {filter: {str: "foo"}, collation: {locale: "fr"}}}]);
        assert.eq(2, res.deletedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite([{deleteMany: {filter: {str: "foo"}, collation: {locale: "fr"}}}]);
        });
    }

    // Two deleteOne ops with bulkWrite using different collations.
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, str: "foo"}));
    assert.writeOK(coll.insert({_id: 2, str: "bar"}));
    if (db.getMongo().writeMode() === "commands") {
        var res = coll.bulkWrite([
            {deleteOne: {filter: {str: "foo"}, collation: {locale: "fr"}}},
            {deleteOne: {filter: {str: "bar"}, collation: {locale: "en_US"}}}
        ]);
        assert.eq(2, res.deletedCount);
    } else {
        assert.throws(function() {
            coll.bulkWrite([
                {deleteOne: {filter: {str: "foo"}, collation: {locale: "fr"}}},
                {deleteOne: {filter: {str: "bar"}, collation: {locale: "en_US"}}}
            ]);
        });
    }
})();
