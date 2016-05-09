// Ensure that shell helpers correctly deliver the collation to the server.
//
// TODO SERVER-23791: Once we have an end-to-end working on mongod, we should be able to strengthen
// the assertions in this file in order to ensure that the server is correctly respecting the
// assertion. Currently we exercise the code paths in the shell that are supposed to propagate the
// collation to the server, but we don't require that the result of executing the command respects
// the collation.
(function() {
    'use strict';

    //
    // TODO SERVER-23849: test using db.createCollection() to make a collection with a default
    // collation.
    //

    var coll = db.collation_shell_helpers;
    coll.drop();

    //
    // Creating an index with a collation.
    //

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

    assert.commandWorked(coll.ensureIndex({a: 1}, {collation: {locale: "en_US"}}));
    assertIndexHasCollation({a: 1}, {locale: "en_US"});
    assert.commandWorked(coll.createIndex({b: 1}, {collation: {locale: "en_US"}}));
    assertIndexHasCollation({b: 1}, {locale: "en_US"});
    assert.commandWorked(coll.createIndexes([{c: 1}, {d: 1}], {collation: {locale: "fr"}}));
    assertIndexHasCollation({c: 1}, {locale: "fr"});
    assertIndexHasCollation({d: 1}, {locale: "fr"});

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
    assert.eq(2,
              coll.aggregate([],
                             {
                                 collation : {
                                 locale:
                                     "fr"
                                 }
                             }).itcount());
    assert.commandWorked(coll.explain().aggregate([],
                                                  {
                                                      collation : {
                                                      locale:
                                                          "fr"
                                                      }
                                                  }));

    // Count command.
    assert.eq(2, coll.find().collation({locale: "fr"}).count());
    assert.eq(2, coll.count({}, {collation: {locale: "fr"}}));
    assert.commandWorked(coll.explain().find().collation({locale: "fr"}).count());

    // Distinct.
    assert.eq(["foo", "bar"], coll.distinct("str", {}, {collation: {locale: "fr"}}));
    assert.commandWorked(coll.explain().distinct("str", {}, {collation: {locale: "fr"}}));

    // Find command.
    if (db.getMongo().useReadCommands()) {
        assert.eq(2, coll.find().collation({locale: "fr"}).itcount());
        assert.neq(null,
                   coll.findOne({str: "foo"}, undefined, undefined, undefined, {locale: "fr"}));
    } else {
        assert.throws(function() {
            coll.find().collation({locale: "fr"}).itcount();
        });
    }
    // Explain of find always uses the find command, so this will succeed regardless of readMode.
    assert.commandWorked(coll.explain().find().collation({locale: "fr"}).finish());
    assert.commandWorked(coll.find().collation({locale: "fr"}).explain());

    // findAndModify.
    assert.eq({_id: 2, str: "baz"},
              coll.findAndModify({
                  query: {str: "bar"},
                  update: {$set: {str: "baz"}}, new: true,
                  collation: {locale: "fr"}
              }));
    assert.commandWorked(coll.explain().findAndModify({
        query: {str: "bar"},
        update: {$set: {str: "baz"}}, new: true,
        collation: {locale: "fr"}
    }));

    // Group.
    assert.eq([{str: "foo", count: 1}, {str: "baz", count: 1}],
              coll.group({
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
                replaceOne: {
                    filter: {str: "foo"},
                    replacement: {str: "bar"},
                    collation: {locale: "fr"}
                }
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
