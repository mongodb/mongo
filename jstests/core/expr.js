// @tags: [does_not_support_stepdowns, requires_getmore, requires_non_retryable_writes]

// Tests for $expr in the CRUD commands.
(function() {
    "use strict";

    const coll = db.expr;

    const isMaster = db.runCommand("ismaster");
    assert.commandWorked(isMaster);
    const isMongos = (isMaster.msg === "isdbgrid");

    //
    // $expr in aggregate.
    //

    coll.drop();
    assert.writeOK(coll.insert({a: 0}));
    assert.eq(1, coll.aggregate([{$match: {$expr: {$eq: ["$a", 0]}}}]).itcount());
    assert.throws(function() {
        coll.aggregate([{$match: {$expr: {$eq: ["$a", "$$unbound"]}}}]);
    });
    assert.throws(function() {
        coll.aggregate([{$match: {$expr: {$divide: [1, "$a"]}}}]);
    });

    //
    // $expr in count.
    //

    coll.drop();
    assert.writeOK(coll.insert({a: 0}));
    assert.eq(1, coll.find({$expr: {$eq: ["$a", 0]}}).count());
    assert.throws(function() {
        coll.find({$expr: {$eq: ["$a", "$$unbound"]}}).count();
    });
    assert.throws(function() {
        coll.find({$expr: {$divide: [1, "$a"]}}).count();
    });

    //
    // $expr in distinct.
    //

    coll.drop();
    assert.writeOK(coll.insert({a: 0}));
    assert.eq(1, coll.distinct("a", {$expr: {$eq: ["$a", 0]}}).length);
    assert.throws(function() {
        coll.distinct("a", {$expr: {$eq: ["$a", "$$unbound"]}});
    });
    assert.throws(function() {
        coll.distinct("a", {$expr: {$divide: [1, "$a"]}});
    });

    //
    // $expr in find.
    //

    // $expr is allowed in query.
    coll.drop();
    assert.writeOK(coll.insert({a: 0}));
    assert.eq(1, coll.find({$expr: {$eq: ["$a", 0]}}).itcount());

    // $expr with time zone expression across getMore (SERVER-31664).
    coll.drop();
    assert.writeOK(coll.insert({a: ISODate("2017-10-01T22:00:00")}));

    let res = assert.commandWorked(db.runCommand({
        find: coll.getName(),
        filter: {$expr: {$eq: [1, {$dayOfMonth: {date: "$a", timezone: "America/New_York"}}]}},
        batchSize: 0
    }));
    assert.eq(0, res.cursor.firstBatch.length);

    let cursorId = res.cursor.id;
    res = assert.commandWorked(db.runCommand({getMore: cursorId, collection: coll.getName()}));
    assert.eq(1, res.cursor.nextBatch.length);

    // $expr with unbound variable throws.
    assert.throws(function() {
        coll.find({$expr: {$eq: ["$a", "$$unbound"]}}).itcount();
    });

    // $and with $expr child containing an invalid expression throws.
    assert.throws(function() {
        coll.find({$and: [{a: 0}, {$expr: {$anyElementTrue: undefined}}]}).itcount();
    });

    // $or with $expr child containing an invalid expression throws.
    assert.throws(function() {
        coll.find({$or: [{a: 0}, {$expr: {$anyElementTrue: undefined}}]}).itcount();
    });

    // $nor with $expr child containing an invalid expression throws.
    assert.throws(function() {
        coll.find({$nor: [{a: 0}, {$expr: {$anyElementTrue: undefined}}]}).itcount();
    });

    // $expr with division by zero throws.
    assert.throws(function() {
        coll.find({$expr: {$divide: [1, "$a"]}}).itcount();
    });

    // $expr is allowed in find with explain.
    assert.commandWorked(coll.find({$expr: {$eq: ["$a", 0]}}).explain());

    // $expr with unbound variable in find with explain throws.
    assert.throws(function() {
        coll.find({$expr: {$eq: ["$a", "$$unbound"]}}).explain();
    });

    // $expr with division by zero in find with explain with executionStats throws.
    assert.throws(function() {
        coll.find({$expr: {$divide: [1, "$a"]}}).explain("executionStats");
    });

    // $expr is not allowed in $elemMatch projection.
    coll.drop();
    assert.writeOK(coll.insert({a: [{b: 5}]}));
    assert.throws(function() {
        coll.find({}, {a: {$elemMatch: {$expr: {$eq: ["$b", 5]}}}}).itcount();
    });

    //
    // $expr in findAndModify.
    //

    // $expr is allowed in the query when upsert=false.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: 0}));
    assert.eq({_id: 0, a: 0, b: 6},
              coll.findAndModify(
                  {query: {_id: 0, $expr: {$eq: ["$a", 0]}}, update: {$set: {b: 6}}, new: true}));

    // $expr with unbound variable throws.
    assert.throws(function() {
        coll.findAndModify(
            {query: {_id: 0, $expr: {$eq: ["$a", "$$unbound"]}}, update: {$set: {b: 6}}});
    });

    // $expr with division by zero throws.
    assert.throws(function() {
        coll.findAndModify({query: {_id: 0, $expr: {$divide: [1, "$a"]}}, update: {$set: {b: 6}}});
    });

    // $expr is not allowed in the query when upsert=true.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: 0}));
    assert.throws(function() {
        coll.findAndModify(
            {query: {_id: 0, $expr: {$eq: ["$a", 0]}}, update: {$set: {b: 6}}, upsert: true});
    });

    // $expr is not allowed in $pull filter.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [{b: 5}]}));
    assert.throws(function() {
        coll.findAndModify({query: {_id: 0}, update: {$pull: {a: {$expr: {$eq: ["$b", 5]}}}}});
    });

    // $expr is not allowed in arrayFilters.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [{b: 5}]}));
        assert.throws(function() {
            coll.findAndModify({
                query: {_id: 0},
                update: {$set: {"a.$[i].b": 6}},
                arrayFilters: [{"i.b": 5, $expr: {$eq: ["$i.b", 5]}}]
            });
        });
    }

    //
    // $expr in the $geoNear stage.
    //

    coll.drop();
    assert.writeOK(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, a: 0}));
    assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));
    assert.eq(1,
              coll.aggregate({
                      $geoNear: {
                          near: {type: "Point", coordinates: [0, 0]},
                          distanceField: "dist",
                          spherical: true,
                          query: {$expr: {$eq: ["$a", 0]}}
                      }
                  })
                  .toArray()
                  .length);
    assert.throws(() => coll.aggregate({
        $geoNear: {
            near: {type: "Point", coordinates: [0, 0]},
            distanceField: "dist",
            spherical: true,
            query: {$expr: {$eq: ["$a", "$$unbound"]}}
        }
    }));
    assert.throws(() => coll.aggregate({
        $geoNear: {
            near: {type: "Point", coordinates: [0, 0]},
            distanceField: "dist",
            spherical: true,
            query: {$expr: {$divide: [1, "$a"]}}
        }
    }));

    //
    // $expr in mapReduce.
    //

    coll.drop();
    assert.writeOK(coll.insert({a: 0}));
    let mapReduceOut = coll.mapReduce(
        function() {
            emit(this.a, 1);
        },
        function(key, values) {
            return Array.sum(values);
        },
        {out: {inline: 1}, query: {$expr: {$eq: ["$a", 0]}}});
    assert.commandWorked(mapReduceOut);
    assert.eq(mapReduceOut.results.length, 1, tojson(mapReduceOut));
    assert.throws(function() {
        coll.mapReduce(
            function() {
                emit(this.a, 1);
            },
            function(key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, query: {$expr: {$eq: ["$a", "$$unbound"]}}});
    });
    assert.throws(function() {
        coll.mapReduce(
            function() {
                emit(this.a, 1);
            },
            function(key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, query: {$expr: {$divide: [1, "$a"]}}});
    });

    //
    // $expr in remove.
    //

    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: 0}));
    let writeRes = coll.remove({_id: 0, $expr: {$eq: ["$a", 0]}});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nRemoved);
    assert.writeError(coll.remove({_id: 0, $expr: {$eq: ["$a", "$$unbound"]}}));
    assert.writeOK(coll.insert({_id: 0, a: 0}));
    assert.writeError(coll.remove({_id: 0, $expr: {$divide: [1, "$a"]}}));

    // Any writes preceding the write that fails to parse are executed.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0}));
    assert.writeOK(coll.insert({_id: 1}));
    writeRes = db.runCommand({
        delete: coll.getName(),
        deletes: [{q: {_id: 0}, limit: 1}, {q: {$expr: "$$unbound"}, limit: 1}]
    });
    assert.commandWorkedIgnoringWriteErrors(writeRes);
    assert.eq(writeRes.writeErrors[0].code, 17276, tojson(writeRes));
    assert.eq(writeRes.n, 1, tojson(writeRes));

    //
    // $expr in update.
    //

    // $expr is allowed in the query when upsert=false.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: 0}));
    assert.writeOK(coll.update({_id: 0, $expr: {$eq: ["$a", 0]}}, {$set: {b: 6}}));
    assert.eq({_id: 0, a: 0, b: 6}, coll.findOne({_id: 0}));

    // $expr with unbound variable fails.
    assert.writeError(coll.update({_id: 0, $expr: {$eq: ["$a", "$$unbound"]}}, {$set: {b: 6}}));

    // $expr with division by zero fails.
    assert.writeError(coll.update({_id: 0, $expr: {$divide: [1, "$a"]}}, {$set: {b: 6}}));

    // $expr is not allowed in the query when upsert=true.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: 5}));
    assert.writeError(
        coll.update({_id: 0, $expr: {$eq: ["$a", 5]}}, {$set: {b: 6}}, {upsert: true}));

    // $expr is not allowed in $pull filter.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: [{b: 5}]}));
    assert.writeError(coll.update({_id: 0}, {$pull: {a: {$expr: {$eq: ["$b", 5]}}}}));

    // $expr is not allowed in arrayFilters.
    if (db.getMongo().writeMode() === "commands") {
        coll.drop();
        assert.writeOK(coll.insert({_id: 0, a: [{b: 5}]}));
        assert.writeError(coll.update({_id: 0},
                                      {$set: {"a.$[i].b": 6}},
                                      {arrayFilters: [{"i.b": 5, $expr: {$eq: ["$i.b", 5]}}]}));
    }

    // Any writes preceding the write that fails to parse are executed.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0}));
    assert.writeOK(coll.insert({_id: 1}));
    writeRes = db.runCommand({
        update: coll.getName(),
        updates: [{q: {_id: 0}, u: {$set: {b: 6}}}, {q: {$expr: "$$unbound"}, u: {$set: {b: 6}}}]
    });
    assert.commandWorkedIgnoringWriteErrors(writeRes);
    assert.eq(writeRes.writeErrors[0].code, 17276, tojson(writeRes));
    assert.eq(writeRes.n, 1, tojson(writeRes));
})();
