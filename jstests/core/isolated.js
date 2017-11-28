// Tests for the $isolated update/delete operator.
// Uses features that require featureCompatibilityVersion 3.6.
// @tags: [requires_fcv36, requires_non_retryable_writes]
(function() {
    "use strict";

    const coll = db.isolated_test;
    coll.drop();

    const isMaster = db.runCommand("ismaster");
    assert.commandWorked(isMaster);
    const isMongos = (isMaster.msg === "isdbgrid");

    //
    // $isolated in aggregate $match stage.
    //

    assert.commandFailed(db.runCommand(
        {aggregate: coll.getName(), pipeline: [{$match: {$isolated: 1}}], cursor: {}}));

    //
    // $isolated in count.
    //

    assert.throws(function() {
        coll.find({$isolated: 1}).count();
    });

    //
    // $isolated in distinct.
    //

    assert.throws(function() {
        coll.distinct("a", {$isolated: 1});
    });

    //
    // $isolated in find.
    //

    // $isolated is not allowed in a query filter.
    assert.throws(function() {
        coll.find({$isolated: 1}).itcount();
    });

    // $isolated is not allowed in find with explain.
    assert.throws(function() {
        coll.find({$isolated: 1}).explain();
    });

    // $isolated is not allowed in $elemMatch projection.
    assert.writeOK(coll.insert({a: [{b: 5}]}));
    assert.throws(function() {
        coll.find({}, {a: {$elemMatch: {$isolated: 1}}}).itcount();
    });

    //
    // $isolated in findAndModify.
    //

    // $isolated is allowed in the query when upsert=false.
    assert.writeOK(coll.insert({_id: 0, a: 5}));
    assert.eq(
        {_id: 0, a: 5, b: 6},
        coll.findAndModify({query: {_id: 0, $isolated: 1}, update: {$set: {b: 6}}, new: true}));

    // $isolated is allowed in the query when upsert=true.
    assert.writeOK(coll.insert({_id: 1, a: 5}));
    assert.eq(
        {_id: 1, a: 5},
        coll.findAndModify({query: {_id: 1, $isolated: 1}, update: {$set: {b: 6}}, upsert: true}));

    // $isolated is not allowed in $pull filter.
    assert.writeOK(coll.insert({_id: 2, a: [{b: 5}]}));
    assert.throws(function() {
        coll.findAndModify({query: {_id: 2}, update: {$pull: {a: {$isolated: 1}}}});
    });

    // $isolated is not allowed in arrayFilters.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();
        assert.writeOK(coll.insert({_id: 3, a: [{b: 5}]}));
        assert.throws(function() {
            coll.findAndModify({
                query: {_id: 3},
                update: {$set: {"a.$[i].b": 6}},
                arrayFilters: [{"i.b": 5, $isolated: 1}]
            });
        });
    }

    //
    // $isolated in geoNear.
    //

    assert.writeOK(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, a: 5}));
    assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));
    assert.commandFailed(db.runCommand({
        geoNear: coll.getName(),
        near: {type: "Point", coordinates: [0, 0]},
        spherical: true,
        query: {$isolated: 1}
    }));

    //
    // $isolated in mapReduce.
    //

    assert.writeOK(coll.insert({a: 5}));
    assert.throws(function() {
        coll.mapReduce(
            function() {
                emit(this.a, 1);
            },
            function(key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, query: {$isolated: 1}});
    });

    //
    // $isolated in remove.
    //

    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: 5}));
    let writeRes = coll.remove({_id: 0, $isolated: 1});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nRemoved);

    //
    // $isolated in update.
    //

    // $isolated is allowed in the filter of an update command.
    assert.writeOK(coll.insert({_id: 0, a: 5}));
    assert.writeOK(coll.update({_id: 0, $isolated: 1}, {$set: {b: 6}}));
    assert.eq({_id: 0, a: 5, b: 6}, coll.findOne({_id: 0}));

    // $isolated is not allowed in a $pull filter.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [{b: 5}]}));
    assert.writeErrorWithCode(coll.update({_id: 0}, {$pull: {a: {$isolated: 1}}}),
                              ErrorCodes.QueryFeatureNotAllowed);

    // $isolated is not allowed in arrayFilters.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [{b: 5}]}));
        assert.writeErrorWithCode(
            coll.update(
                {_id: 0}, {$set: {"a.$[i].b": 6}}, {arrayFilters: [{"i.b": 5, $isolated: 1}]}),
            ErrorCodes.QueryFeatureNotAllowed);
    }

    //
    // $isolated in a view.
    //

    assert.commandFailed(db.createView("invalid", coll.getName(), [{$match: {$isolated: 1}}]));
    assert.commandFailed(
        db.createView("invalid", coll.getName(), [{$match: {a: {$gt: 5}, $isolated: 1}}]));

    //
    // $isolated in a partial index filter.
    //

    assert.commandFailed(coll.createIndex({a: 1}, {partialFilterExpression: {$isolated: 1}}));
    assert.commandFailed(
        coll.createIndex({a: 1}, {partialFilterExpression: {a: {$lt: 5}, $isolated: 1}}));

})();
