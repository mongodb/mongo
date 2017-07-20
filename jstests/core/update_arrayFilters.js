// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]

// Tests for the arrayFilters option to update and findAndModify.
(function() {
    "use strict";

    let coll = db.update_arrayFilters;
    coll.drop();
    let res;

    //
    // Tests for update.
    //

    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]});
        });
    } else {
        // Non-array arrayFilters fails to parse.
        assert.writeError(coll.update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: {i: 0}}),
                          ErrorCodes.TypeMismatch);

        // Non-object array filter fails to parse.
        assert.writeError(coll.update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: ["bad"]}),
                          ErrorCodes.TypeMismatch);

        // Bad array filter fails to parse.
        res = coll.update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0, j: 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.FailedToParse);
        assert.neq(-1,
                   res.getWriteError().errmsg.indexOf(
                       "Each array filter must use a single top-level field name"),
                   "update failed for a reason other than failing to parse array filters");

        // Multiple array filters with the same id fails to parse.
        res = coll.update(
            {_id: 0}, {$set: {"a.$[i]": 5, "a.$[j]": 6}}, {arrayFilters: [{i: 0}, {j: 0}, {i: 1}]});
        assert.writeErrorWithCode(res, ErrorCodes.FailedToParse);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf(
                "Found multiple array filters with the same top-level field name"),
            "update failed for a reason other than multiple array filters with the same top-level field name");

        // Unused array filter fails to parse.
        res = coll.update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}, {j: 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.FailedToParse);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf(
                "The array filter for identifier 'j' was not used in the update { $set: { a.$[i]: 5.0 } }"),
            "update failed for a reason other than unused array filter");

        // Good value for arrayFilters succeeds.
        assert.writeOK(coll.update(
            {_id: 0}, {$set: {"a.$[i]": 5, "a.$[j]": 6}}, {arrayFilters: [{i: 0}, {j: 0}]}));
    }

    //
    // Tests for findAndModify.
    //

    // Non-array arrayFilters fails to parse.
    assert.throws(function() {
        coll.findAndModify({query: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: {i: 0}});
    });

    // Non-object array filter fails to parse.
    assert.throws(function() {
        coll.findAndModify({query: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: ["bad"]});
    });

    // arrayFilters option not allowed with remove=true.
    assert.throws(function() {
        coll.findAndModify({query: {_id: 0}, remove: true, arrayFilters: [{i: 0}]});
    });

    // Bad array filter fails to parse.
    assert.throws(function() {
        coll.findAndModify(
            {query: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: [{i: 0, j: 0}]});
    });

    // Multiple array filters with the same id fails to parse.
    assert.throws(function() {
        coll.findAndModify({
            query: {_id: 0},
            update: {$set: {"a.$[i]": 5, "a.$[j]": 6}},
            arrayFilters: [{i: 0}, {j: 0}, {i: 1}]
        });
    });

    // Unused array filter fails to parse.
    assert.throws(function() {
        coll.findAndModify(
            {query: {_id: 0}, update: {$set: {"a.$[i]": 5}, arrayFilters: [{i: 0}, {j: 0}]}});
    });

    // Good value for arrayFilters succeeds.
    assert.eq(null, coll.findAndModify({
        query: {_id: 0},
        update: {$set: {"a.$[i]": 5, "a.$[j]": 6}},
        arrayFilters: [{i: 0}, {j: 0}]
    }));

    //
    // Tests for the bulk API.
    //

    if (db.getMongo().writeMode() !== "commands") {
        let bulk = coll.initializeUnorderedBulkOp();
        bulk.find({});
        assert.throws(function() {
            bulk.arrayFilters([{i: 0}]);
        });
    } else {
        // update().
        let bulk = coll.initializeUnorderedBulkOp();
        bulk.find({}).arrayFilters("bad").update({$set: {"a.$[i]": 5}});
        assert.throws(function() {
            bulk.execute();
        });
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({}).arrayFilters([{i: 0}]).update({$set: {"a.$[i]": 5}});
        assert.writeOK(bulk.execute());

        // updateOne().
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({_id: 0}).arrayFilters("bad").updateOne({$set: {"a.$[i]": 5}});
        assert.throws(function() {
            bulk.execute();
        });
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({_id: 0}).arrayFilters([{i: 0}]).updateOne({$set: {"a.$[i]": 5}});
        assert.writeOK(bulk.execute());
    }

    //
    // Tests for the CRUD API.
    //

    // findOneAndUpdate().
    assert.throws(function() {
        coll.findOneAndUpdate({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: "bad"});
    });
    assert.eq(null,
              coll.findOneAndUpdate({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]}));

    // updateOne().
    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.updateOne({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]});
        });
    } else {
        assert.throws(function() {
            coll.updateOne({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: "bad"});
        });
        res = coll.updateOne({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]});
        assert.eq(0, res.modifiedCount);
    }

    // updateMany().
    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.updateMany({}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]});
        });
    } else {
        assert.throws(function() {
            coll.updateMany({}, {$set: {"a.$[i]": 5}}, {arrayFilters: "bad"});
        });
        res = coll.updateMany({}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]});
        assert.eq(0, res.modifiedCount);
    }

    // updateOne with bulkWrite().
    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.bulkWrite([{
                updateOne:
                    {filter: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: [{i: 0}]}
            }]);
        });
    } else {
        assert.throws(function() {
            coll.bulkWrite([{
                updateOne:
                    {filter: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: "bad"}
            }]);
        });
        res = coll.bulkWrite([{
            updateOne: {filter: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: [{i: 0}]}
        }]);
        assert.eq(0, res.matchedCount);
    }

    // updateMany with bulkWrite().
    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.bulkWrite([
                {updateMany: {filter: {}, update: {$set: {"a.$[i]": 5}}, arrayFilters: [{i: 0}]}}
            ]);
        });
    } else {
        assert.throws(function() {
            coll.bulkWrite(
                [{updateMany: {filter: {}, update: {$set: {"a.$[i]": 5}}, arrayFilters: "bad"}}]);
        });
        res = coll.bulkWrite(
            [{updateMany: {filter: {}, update: {$set: {"a.$[i]": 5}}, arrayFilters: [{i: 0}]}}]);
        assert.eq(0, res.matchedCount);
    }

    //
    // Tests for explain().
    //

    // update().
    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.explain().update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]});
        });
    } else {
        assert.throws(function() {
            coll.explain().update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: "bad"});
        });
        assert.commandWorked(
            coll.explain().update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]}));
    }

    // findAndModify().
    assert.throws(function() {
        coll.explain().findAndModify(
            {query: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: "bad"});
    });
    assert.commandWorked(coll.explain().findAndModify(
        {query: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: [{i: 0}]}));

    //
    // Tests for individual update modifiers.
    //

    // $set.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [0, 1, 0, 1]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(coll.update({_id: 0}, {$set: {"a.$[i]": 2}}, {arrayFilters: [{i: 0}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [2, 1, 2, 1]});
    }
    assert.writeOK(coll.update({_id: 0}, {$set: {"a.$[]": 3}}));
    assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [3, 3, 3, 3]});

    // $unset.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [0, 1, 0, 1]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(coll.update({_id: 0}, {$unset: {"a.$[i]": true}}, {arrayFilters: [{i: 0}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [null, 1, null, 1]});
    }
    assert.writeOK(coll.update({_id: 0}, {$unset: {"a.$[]": true}}));
    assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [null, null, null, null]});

    // $inc.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [0, 1, 0, 1]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(coll.update({_id: 0}, {$inc: {"a.$[i]": 1}}, {arrayFilters: [{i: 1}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [0, 2, 0, 2]});
    }
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [0, 1, 0, 1]}));
    assert.writeOK(coll.update({_id: 0}, {$inc: {"a.$[]": 1}}));
    assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [1, 2, 1, 2]});

    // $mul.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [0, 2, 0, 2]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(coll.update({_id: 0}, {$mul: {"a.$[i]": 3}}, {arrayFilters: [{i: 2}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [0, 6, 0, 6]});
    }
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [1, 2, 1, 2]}));
    assert.writeOK(coll.update({_id: 0}, {$mul: {"a.$[]": 3}}));
    assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [3, 6, 3, 6]});

    // $rename.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [1, 2, 3, 4]}));
    if (db.getMongo().writeMode() === "commands") {
        res = coll.update({_id: 0}, {$rename: {"a.$[i]": "b"}}, {arrayFilters: [{i: 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.BadValue);
        assert.neq(-1,
                   res.getWriteError().errmsg.indexOf(
                       "The source field for $rename may not be dynamic: a.$[i]"),
                   "update failed for a reason other than using $[] syntax in $rename path");
        res = coll.update({id: 0}, {$rename: {"a": "b"}}, {arrayFilters: [{i: 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.FailedToParse);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf(
                "The array filter for identifier 'i' was not used in the update { $rename: { a: \"b\" } }"),
            "updated failed for reason other than unused array filter");
    }
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [0], b: [1]}));
    res = coll.update({_id: 0}, {$rename: {"a.$[]": "b"}});
    assert.writeErrorWithCode(res, ErrorCodes.BadValue);
    assert.neq(-1,
               res.getWriteError().errmsg.indexOf(
                   "The source field for $rename may not be dynamic: a.$[]"),
               "update failed for a reason other than using array updates with $rename");
    res = coll.update({_id: 0}, {$rename: {"a": "b.$[]"}});
    assert.writeErrorWithCode(res, ErrorCodes.BadValue);
    assert.neq(-1,
               res.getWriteError().errmsg.indexOf(
                   "The destination field for $rename may not be dynamic: b.$[]"),
               "update failed for a reason other than using array updates with $rename");
    assert.writeOK(coll.update({_id: 0}, {$rename: {"a": "b"}}));
    assert.eq(coll.findOne({_id: 0}), {_id: 0, b: [0]});

    // $setOnInsert.
    // TODO SERVER-28773: $setOnInsert should use the new update implementation.
    coll.drop();
    if (db.getMongo().writeMode() === "commands") {
        res = coll.update({_id: 0}, {$setOnInsert: {"a.$[i]": 1}}, {arrayFilters: [{i: 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.InvalidOptions);
        assert.neq(-1,
                   res.getWriteError().errmsg.indexOf(
                       "Cannot use array filters with modifier $setOnInsert"),
                   "update failed for a reason other than using array filters with $setOnInsert");
    }
    res = coll.update({_id: 0, a: [0]}, {$setOnInsert: {"a.$[]": 1}}, {upsert: true});
    assert.writeErrorWithCode(res, 16836);
    assert.neq(-1,
               res.getWriteError().errmsg.indexOf(
                   "cannot use the part (a of a.$[]) to traverse the element ({a: [ 0.0 ]})"),
               "update failed for a reason other than using array updates with $setOnInsert");

    // $min.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [{b: 0, c: 1}, {b: 0, c: -1}, {b: 1, c: 1}]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(
            coll.update({_id: 0}, {$min: {"a.$[i].c": 0}}, {arrayFilters: [{"i.b": 0}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [{b: 0, c: 0}, {b: 0, c: -1}, {b: 1, c: 1}]});
    }
    assert.writeOK(coll.update({_id: 0}, {$min: {"a.$[].c": 0}}));
    assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [{b: 0, c: 0}, {b: 0, c: -1}, {b: 1, c: 0}]});

    // $max.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [{b: 0, c: 1}, {b: 0, c: -1}, {b: 1, c: -1}]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(
            coll.update({_id: 0}, {$max: {"a.$[i].c": 0}}, {arrayFilters: [{"i.b": 0}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [{b: 0, c: 1}, {b: 0, c: 0}, {b: 1, c: -1}]});
    }
    assert.writeOK(coll.update({_id: 0}, {$max: {"a.$[].c": 0}}));
    assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [{b: 0, c: 1}, {b: 0, c: 0}, {b: 1, c: 0}]});

    // $currentDate.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [0, 1]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(
            coll.update({_id: 0}, {$currentDate: {"a.$[i]": true}}, {arrayFilters: [{i: 0}]}));
        let doc = coll.findOne({_id: 0});
        assert(doc.a[0].constructor == Date, tojson(doc));
        assert.eq(doc.a[1], 1, printjson(doc));
    }
    assert.writeOK(coll.update({_id: 0}, {$currentDate: {"a.$[]": true}}));
    let doc = coll.findOne({_id: 0});
    assert(doc.a[0].constructor == Date, tojson(doc));
    assert(doc.a[1].constructor == Date, tojson(doc));

    // $addToSet.
    coll.drop();
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(coll.insert({_id: 0, a: [[0], [1]]}));
        assert.writeOK(coll.update({_id: 0}, {$addToSet: {"a.$[i]": 2}}, {arrayFilters: [{i: 0}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [[0, 2], [1]]});
    }
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [[0], [1]]}));
    assert.writeOK(coll.update({_id: 0}, {$addToSet: {"a.$[]": 2}}));
    assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [[0, 2], [1, 2]]});

    // $pop.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [[0, 1], [1, 2]]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(coll.update({_id: 0}, {$pop: {"a.$[i]": 1}}, {arrayFilters: [{i: 0}]}));
        assert.eq({_id: 0, a: [[0], [1, 2]]}, coll.findOne());
    }
    assert.writeOK(coll.remove({}));
    assert.writeOK(coll.insert({_id: 0, a: [[0]]}));
    assert.writeOK(coll.update({_id: 0}, {$pop: {"a.$[]": 1}}));
    assert.eq({_id: 0, a: [[]]}, coll.findOne());

    // $pullAll.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [[0, 1, 2, 3], [1, 2, 3, 4]]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(
            coll.update({_id: 0}, {$pullAll: {"a.$[i]": [0, 2]}}, {arrayFilters: [{i: 0}]}));
        assert.eq({_id: 0, a: [[1, 3], [1, 2, 3, 4]]}, coll.findOne());
    }
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [[0, 1, 2, 3], [1, 2, 3, 4]]}));
    res = coll.update({_id: 0}, {$pullAll: {"a.$[]": [0, 2]}});
    assert.eq({_id: 0, a: [[1, 3], [1, 3, 4]]}, coll.findOne());

    // $pull.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [[0, 1], [1, 2]]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(coll.update({_id: 0}, {$pull: {"a.$[i]": 1}}, {arrayFilters: [{i: 2}]}));
        assert.eq({_id: 0, a: [[0, 1], [2]]}, coll.findOne());
    }
    assert.writeOK(coll.remove({}));
    assert.writeOK(coll.insert({_id: 0, a: [[0, 1], [1, 2]]}));
    assert.writeOK(coll.update({_id: 0}, {$pull: {"a.$[]": 1}}));
    assert.eq({_id: 0, a: [[0], [2]]}, coll.findOne());

    // $pushAll.
    // TODO SERVER-28772: $pushAll should use the new update implementation.
    coll.drop();
    if (db.getMongo().writeMode() === "commands") {
        res = coll.update({_id: 0}, {$pushAll: {"a.$[i]": [0]}}, {arrayFilters: [{i: 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.InvalidOptions);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf("Cannot use array filters with modifier $pushAll"),
            "update failed for a reason other than using array filters with $pushAll");
    }
    assert.writeOK(coll.insert({_id: 0, a: [[0]]}));
    res = coll.update({_id: 0}, {$pushAll: {"a.$[]": [1]}});
    assert.writeErrorWithCode(res, 16837);
    assert.neq(-1,
               res.getWriteError().errmsg.indexOf(
                   "cannot use the part (a of a.$[]) to traverse the element ({a: [ [ 0.0 ] ]})"),
               "update failed for a reason other than using array updates with $pushAll");

    // $push.
    // TODO SERVER-28772: $push should use the new update implementation.
    coll.drop();
    if (db.getMongo().writeMode() === "commands") {
        res = coll.update({_id: 0}, {$push: {"a.$[i]": 0}}, {arrayFilters: [{i: 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.InvalidOptions);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf("Cannot use array filters with modifier $push"),
            "update failed for a reason other than using array filters with $push");
    }
    assert.writeOK(coll.insert({_id: 0, a: [[0]]}));
    res = coll.update({_id: 0}, {$push: {"a.$[]": 1}});
    assert.writeErrorWithCode(res, 16837);
    assert.neq(-1,
               res.getWriteError().errmsg.indexOf(
                   "cannot use the part (a of a.$[]) to traverse the element ({a: [ [ 0.0 ] ]})"),
               "update failed for a reason other than using array updates with $push");

    // $bit.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [NumberInt(0), NumberInt(2)]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(coll.update(
            {_id: 0}, {$bit: {"a.$[i]": {or: NumberInt(10)}}}, {arrayFilters: [{i: 0}]}));
        assert.eq({_id: 0, a: [NumberInt(10), NumberInt(2)]}, coll.findOne());
    }
    assert.writeOK(coll.remove({}));
    assert.writeOK(coll.insert({_id: 0, a: [NumberInt(0), NumberInt(2)]}));
    assert.writeOK(coll.update({_id: 0}, {$bit: {"a.$[]": {or: NumberInt(10)}}}));
    assert.eq({_id: 0, a: [NumberInt(10), NumberInt(10)]}, coll.findOne());

    //
    // Multi update tests.
    //

    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [0, 1, 0, 1]}));
    assert.writeOK(coll.insert({_id: 1, a: [0, 2, 0, 2]}));
    if (db.getMongo().writeMode() === "commands") {
        assert.writeOK(
            coll.update({}, {$set: {"a.$[i]": 3}}, {multi: true, arrayFilters: [{i: 0}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [3, 1, 3, 1]});
        assert.eq(coll.findOne({_id: 1}), {_id: 1, a: [3, 2, 3, 2]});
    }
    assert.writeOK(coll.update({}, {$set: {"a.$[]": 3}}, {multi: true}));
    assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [3, 3, 3, 3]});
    assert.eq(coll.findOne({_id: 1}), {_id: 1, a: [3, 3, 3, 3]});

    //
    // Collation tests.
    //

    if (db.getMongo().writeMode() === "commands") {
        // arrayFilters respect operation collation.
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: ["foo", "FOO"]}));
        assert.writeOK(
            coll.update({_id: 0},
                        {$set: {"a.$[i]": "bar"}},
                        {arrayFilters: [{i: "foo"}], collation: {locale: "en_US", strength: 2}}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: ["bar", "bar"]});

        // arrayFilters respect the collection default collation.
        coll.drop();
        assert.commandWorked(db.createCollection("update_arrayFilters",
                                                 {collation: {locale: "en_US", strength: 2}}));
        coll = db.update_arrayFilters;
        assert.writeOK(coll.insert({_id: 0, a: ["foo", "FOO"]}));
        assert.writeOK(
            coll.update({_id: 0}, {$set: {"a.$[i]": "bar"}}, {arrayFilters: [{i: "foo"}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: ["bar", "bar"]});
    }

    //
    // Examples.
    //

    // Update all documents in array.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [{b: 0}, {b: 1}]}));
    assert.writeOK(coll.update({_id: 0}, {$set: {"a.$[].b": 2}}));
    assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [{b: 2}, {b: 2}]});

    // Update all matching documents in array.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [{b: 0}, {b: 1}]}));
        assert.writeOK(
            coll.update({_id: 0}, {$set: {"a.$[i].b": 2}}, {arrayFilters: [{"i.b": 0}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [{b: 2}, {b: 1}]});
    }

    // Update all matching scalars in array.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [0, 1]}));
        assert.writeOK(coll.update({_id: 0}, {$set: {"a.$[i]": 2}}, {arrayFilters: [{i: 0}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [2, 1]});
    }

    // Update all matching scalars in array of arrays.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [[0, 1], [0, 1]]}));
        assert.writeOK(coll.update({_id: 0}, {$set: {"a.$[].$[j]": 2}}, {arrayFilters: [{j: 0}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [[2, 1], [2, 1]]});
    }

    // Update all matching documents in nested array.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();
        assert.writeOK(
            coll.insert({_id: 0, a: [{b: 0, c: [{d: 0}, {d: 1}]}, {b: 1, c: [{d: 0}, {d: 1}]}]}));
        assert.writeOK(coll.update(
            {_id: 0}, {$set: {"a.$[i].c.$[j].d": 2}}, {arrayFilters: [{"i.b": 0}, {"j.d": 0}]}));
        assert.eq(coll.findOne({_id: 0}),
                  {_id: 0, a: [{b: 0, c: [{d: 2}, {d: 1}]}, {b: 1, c: [{d: 0}, {d: 1}]}]});
    }

    // Update all scalars in array matching a logical predicate.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [0, 1, 3]}));
        assert.writeOK(coll.update(
            {_id: 0}, {$set: {"a.$[i]": 2}}, {arrayFilters: [{$or: [{i: 0}, {i: 3}]}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [2, 1, 2]});
    }

    //
    // Error cases.
    //

    // Provide an <id> with no array filter.
    coll.drop();
    res = coll.update({_id: 0}, {$set: {"a.$[i]": 0}});
    assert.writeErrorWithCode(res, ErrorCodes.BadValue);
    assert.neq(-1,
               res.getWriteError().errmsg.indexOf(
                   "No array filter found for identifier 'i' in path 'a.$[i]'"),
               "update failed for a reason other than missing array filter");

    // Use an <id> at the same position as a $, integer, or field name.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();

        res = coll.update({_id: 0}, {$set: {"a.$[i]": 0, "a.$": 0}}, {arrayFilters: [{i: 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.ConflictingUpdateOperators);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf(
                "Updating the path 'a.$' would create a conflict at 'a'"),
            "update failed for a reason other than conflicting array update and positional operator");

        res = coll.update({_id: 0}, {$set: {"a.$[i]": 0, "a.0": 0}}, {arrayFilters: [{i: 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.ConflictingUpdateOperators);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf(
                "Updating the path 'a.0' would create a conflict at 'a'"),
            "update failed for a reason other than conflicting array update and integer field name");

        res = coll.update({_id: 0}, {$set: {"a.$[i]": 0, "a.b": 0}}, {arrayFilters: [{i: 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.ConflictingUpdateOperators);
        assert.neq(-1,
                   res.getWriteError().errmsg.indexOf(
                       "Updating the path 'a.b' would create a conflict at 'a'"),
                   "update failed for a reason other than conflicting array update and field name");
    }

    // Include an implicit array traversal in a path in an update modifier.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [{b: 0}]}));
    res = coll.update({_id: 0}, {$set: {"a.b": 1}});
    assert.writeErrorWithCode(res, ErrorCodes.PathNotViable);
    assert.neq(-1,
               res.getWriteError().errmsg.indexOf(
                   "Cannot create field 'b' in element {a: [ { b: 0.0 } ]}"),
               "update failed for a reason other than implicit array traversal");

    // <id> contains special characters or does not begin with a lowercase letter.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();

        res = coll.update({_id: 0}, {$set: {"a.$[$i]": 1}}, {arrayFilters: [{"$i": 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.BadValue);
        assert.neq(-1,
                   res.getWriteError().errmsg.indexOf("unknown top level operator: $i"),
                   "update failed for a reason other than bad array filter identifier");

        res = coll.update({_id: 0}, {$set: {"a.$[I]": 1}}, {arrayFilters: [{"I": 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.BadValue);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf(
                "Error parsing array filter: The top-level field name must be an alphanumeric string beginning with a lowercase letter, found 'I'"),
            "update failed for a reason other than bad array filter identifier");

        assert.writeOK(coll.insert({_id: 0, a: [0], b: [{j: 0}]}));
        res = coll.update(
            {_id: 0}, {$set: {"a.$[i.j]": 1, "b.$[i]": 1}}, {arrayFilters: [{"i.j": 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.PathNotViable);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf("Cannot create field '$[i' in element {a: [ 0.0 ]}"),
            "update failed for a reason other than bad array filter identifier");
    }

    //
    // Nested array update conflict detection.
    //

    if (db.getMongo().writeMode() === "commands") {
        // "a.$[i].b.$[k].c" and "a.$[j].b.$[k].d" are not a conflict, even if i and j are not
        // disjoint.
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [{x: 0, b: [{y: 0, c: 0, d: 0}]}]}));
        assert.writeOK(coll.update({_id: 0},
                                   {$set: {"a.$[i].b.$[k].c": 1, "a.$[j].b.$[k].d": 1}},
                                   {arrayFilters: [{"i.x": 0}, {"j.x": 0}, {"k.y": 0}]}));
        assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [{x: 0, b: [{y: 0, c: 1, d: 1}]}]});

        // "a.$[i].b.$[k].c" and "a.$[j].b.$[k].c" are a conflict iff i and j are not disjoint.
        coll.drop();
        assert.writeOK(
            coll.insert({_id: 0, a: [{x: 0, b: [{y: 0, c: 0}]}, {x: 1, b: [{y: 0, c: 0}]}]}));

        res = coll.update({_id: 0},
                          {$set: {"a.$[i].b.$[k].c": 1, "a.$[j].b.$[k].c": 2}},
                          {arrayFilters: [{"i.x": 0}, {"j.x": 0}, {"k.y": 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.ConflictingUpdateOperators);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf("Update created a conflict at 'a.0.b.$[k].c'"),
            "update failed for a reason other than conflicting array updates");

        assert.writeOK(coll.update({_id: 0},
                                   {$set: {"a.$[i].b.$[k].c": 1, "a.$[j].b.$[k].c": 2}},
                                   {arrayFilters: [{"i.x": 0}, {"j.x": 1}, {"k.y": 0}]}));
        assert.eq(coll.findOne({_id: 0}),
                  {_id: 0, a: [{x: 0, b: [{y: 0, c: 1}]}, {x: 1, b: [{y: 0, c: 2}]}]});

        // "a.$[i].b.$[k].c" and "a.$[j].b.$[m].c" are a conflict iff k and m intersect for some
        // element of a matching i and j.
        coll.drop();
        assert.writeOK(coll.insert(
            {_id: 0, a: [{x: 0, b: [{y: 0, c: 0}]}, {x: 1, b: [{y: 0, c: 0}, {y: 1, c: 0}]}]}));

        res = coll.update({_id: 0},
                          {$set: {"a.$[i].b.$[k].c": 1, "a.$[j].b.$[m].c": 2}},
                          {arrayFilters: [{"i.x": 0}, {"j.x": 0}, {"k.y": 0}, {"m.y": 0}]});
        assert.writeErrorWithCode(res, ErrorCodes.ConflictingUpdateOperators);
        assert.neq(-1,
                   res.getWriteError().errmsg.indexOf("Update created a conflict at 'a.0.b.0.c'"),
                   "update failed for a reason other than conflicting array updates");

        assert.writeOK(coll.update({_id: 0}, {$set: {"a.$[i].b.$[k].c": 1, "a.$[j].b.$[m].c": 2}}, {
            arrayFilters: [{"i.x": 1}, {"j.x": 1}, {"k.y": 0}, {"m.y": 1}]
        }));
        assert.eq(
            coll.findOne({_id: 0}),
            {_id: 0, a: [{x: 0, b: [{y: 0, c: 0}]}, {x: 1, b: [{y: 0, c: 1}, {y: 1, c: 2}]}]});
    }

})();
