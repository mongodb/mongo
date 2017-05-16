// Tests for the arrayFilters option to update and findAndModify.
(function() {
    "use strict";

    let coll = db.update_arrayFilters;
    coll.drop();

    //
    // Tests for update.
    //

    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.update({_id: 0}, {$set: {a: 5}}, {arrayFilters: [{i: 0}]});
        });
    } else {
        // Non-array arrayFilters fails to parse.
        assert.writeError(coll.update({_id: 0}, {$set: {a: 5}}, {arrayFilters: {i: 0}}),
                          ErrorCodes.TypeMismatch);

        // Non-object array filter fails to parse.
        assert.writeError(coll.update({_id: 0}, {$set: {a: 5}}, {arrayFilters: ["bad"]}),
                          ErrorCodes.TypeMismatch);

        // Bad array filter fails to parse.
        let res = coll.update({_id: 0}, {$set: {a: 5}}, {arrayFilters: [{i: 0, j: 0}]});
        assert.writeError(res, ErrorCodes.FailedToParse);
        assert.neq(-1,
                   res.getWriteError().errmsg.indexOf(
                       "Each array filter must use a single top-level field name"),
                   "update failed for a reason other than failing to parse array filters");

        // Multiple array filters with the same id fails to parse.
        res = coll.update({_id: 0}, {$set: {a: 5}}, {arrayFilters: [{i: 0}, {j: 0}, {i: 1}]});
        assert.writeError(res, ErrorCodes.FailedToParse);
        assert.neq(
            -1,
            res.getWriteError().errmsg.indexOf(
                "Found multiple array filters with the same top-level field name"),
            "update failed for a reason other than multiple array filters with the same top-level field name");

        // Good value for arrayFilters succeeds.
        assert.writeOK(coll.update({_id: 0}, {$set: {a: 5}}, {arrayFilters: [{i: 0}, {j: 0}]}));
    }

    //
    // Tests for findAndModify.
    //

    // Non-array arrayFilters fails to parse.
    assert.throws(function() {
        coll.findAndModify({query: {_id: 0}, update: {$set: {a: 5}}, arrayFilters: {i: 0}});
    });

    // Non-object array filter fails to parse.
    assert.throws(function() {
        coll.findAndModify({query: {_id: 0}, update: {$set: {a: 5}}, arrayFilters: ["bad"]});
    });

    // arrayFilters option not allowed with remove=true.
    assert.throws(function() {
        coll.findAndModify({query: {_id: 0}, remove: true, arrayFilters: [{i: 0}]});
    });

    // Bad array filter fails to parse.
    assert.throws(function() {
        coll.findAndModify({query: {_id: 0}, update: {$set: {a: 5}}, arrayFilters: [{i: 0, j: 0}]});
    });

    // Multiple array filters with the same id fails to parse.
    assert.throws(function() {
        coll.findAndModify(
            {query: {_id: 0}, update: {$set: {a: 5}}, arrayFilters: [{i: 0}, {j: 0}, {i: 1}]});
    });

    // Good value for arrayFilters succeeds.
    assert.eq(null,
              coll.findAndModify(
                  {query: {_id: 0}, update: {$set: {a: 5}}, arrayFilters: [{i: 0}, {j: 0}]}));

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
        bulk.find({}).arrayFilters("bad").update({$set: {a: 5}});
        assert.throws(function() {
            bulk.execute();
        });
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({}).arrayFilters([{i: 0}]).update({$set: {a: 5}});
        assert.writeOK(bulk.execute());

        // updateOne().
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({_id: 0}).arrayFilters("bad").updateOne({$set: {a: 5}});
        assert.throws(function() {
            bulk.execute();
        });
        bulk = coll.initializeUnorderedBulkOp();
        bulk.find({_id: 0}).arrayFilters([{i: 0}]).updateOne({$set: {a: 5}});
        assert.writeOK(bulk.execute());
    }

    //
    // Tests for the CRUD API.
    //

    // findOneAndUpdate().
    assert.throws(function() {
        coll.findOneAndUpdate({_id: 0}, {$set: {a: 5}}, {arrayFilters: "bad"});
    });
    assert.eq(null, coll.findOneAndUpdate({_id: 0}, {$set: {a: 5}}, {arrayFilters: [{i: 0}]}));

    // updateOne().
    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.updateOne({_id: 0}, {$set: {a: 5}}, {arrayFilters: [{i: 0}]});
        });
    } else {
        assert.throws(function() {
            coll.updateOne({_id: 0}, {$set: {a: 5}}, {arrayFilters: "bad"});
        });
        let res = coll.updateOne({_id: 0}, {$set: {a: 5}}, {arrayFilters: [{i: 0}]});
        assert.eq(0, res.modifiedCount);
    }

    // updateMany().
    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.updateMany({}, {$set: {a: 5}}, {arrayFilters: [{i: 0}]});
        });
    } else {
        assert.throws(function() {
            coll.updateMany({}, {$set: {a: 5}}, {arrayFilters: "bad"});
        });
        let res = coll.updateMany({}, {$set: {a: 5}}, {arrayFilters: [{i: 0}]});
        assert.eq(0, res.modifiedCount);
    }

    // updateOne with bulkWrite().
    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.bulkWrite(
                [{updateOne: {filter: {_id: 0}, update: {$set: {a: 5}}, arrayFilters: [{i: 0}]}}]);
        });
    } else {
        assert.throws(function() {
            coll.bulkWrite(
                [{updateOne: {filter: {_id: 0}, update: {$set: {a: 5}}, arrayFilters: "bad"}}]);
        });
        let res = coll.bulkWrite(
            [{updateOne: {filter: {_id: 0}, update: {$set: {a: 5}}, arrayFilters: [{i: 0}]}}]);
        assert.eq(0, res.matchedCount);
    }

    // updateMany with bulkWrite().
    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.bulkWrite(
                [{updateMany: {filter: {}, update: {$set: {a: 5}}, arrayFilters: [{i: 0}]}}]);
        });
    } else {
        assert.throws(function() {
            coll.bulkWrite(
                [{updateMany: {filter: {}, update: {$set: {a: 5}}, arrayFilters: "bad"}}]);
        });
        let res = coll.bulkWrite(
            [{updateMany: {filter: {}, update: {$set: {a: 5}}, arrayFilters: [{i: 0}]}}]);
        assert.eq(0, res.matchedCount);
    }

    //
    // Tests for explain().
    //

    // update().
    if (db.getMongo().writeMode() !== "commands") {
        assert.throws(function() {
            coll.explain().update({_id: 0}, {$set: {a: 5}}, {arrayFilters: [{i: 0}]});
        });
    } else {
        assert.throws(function() {
            coll.explain().update({_id: 0}, {$set: {a: 5}}, {arrayFilters: "bad"});
        });
        assert.commandWorked(
            coll.explain().update({_id: 0}, {$set: {a: 5}}, {arrayFilters: [{i: 0}]}));
    }

    // findAndModify().
    assert.throws(function() {
        coll.explain().findAndModify(
            {query: {_id: 0}, update: {$set: {a: 5}}, arrayFilters: "bad"});
    });
    assert.commandWorked(coll.explain().findAndModify(
        {query: {_id: 0}, update: {$set: {a: 5}}, arrayFilters: [{i: 0}]}));
})();