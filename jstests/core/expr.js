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
    assert.writeOK(coll.insert({a: 5}));
    assert.eq(1, coll.aggregate([{$match: {$expr: {$eq: ["$a", 5]}}}]).itcount());
    assert.throws(function() {
        coll.aggregate([{$match: {$expr: {$eq: ["$a", "$$unbound"]}}}]);
    });

    //
    // $expr in count.
    //

    coll.drop();
    assert.writeOK(coll.insert({a: 5}));
    assert.eq(1, coll.find({$expr: {$eq: ["$a", 5]}}).count());
    assert.throws(function() {
        coll.find({$expr: {$eq: ["$a", "$$unbound"]}}).count();
    });

    //
    // $expr in distinct.
    //

    coll.drop();
    assert.writeOK(coll.insert({a: 5}));
    assert.eq(1, coll.distinct("a", {$expr: {$eq: ["$a", 5]}}).length);
    assert.throws(function() {
        coll.distinct("a", {$expr: {$eq: ["$a", "$$unbound"]}});
    });

    //
    // $expr in find.
    //

    // $expr is allowed in query.
    coll.drop();
    assert.writeOK(coll.insert({a: 5}));
    assert.eq(1, coll.find({$expr: {$eq: ["$a", 5]}}).itcount());

    // $expr with unbound variable throws.
    assert.throws(function() {
        coll.find({$expr: {$eq: ["$a", "$$unbound"]}}).itcount();
    });

    // $expr is allowed in find with explain.
    assert.commandWorked(coll.find({$expr: {$eq: ["$a", 5]}}).explain());

    // $expr with unbound variable in find with explain throws.
    assert.throws(function() {
        coll.find({$expr: {$eq: ["$a", "$$unbound"]}}).explain();
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
    assert.writeOK(coll.insert({_id: 0, a: 5}));
    assert.eq({_id: 0, a: 5, b: 6},
              coll.findAndModify(
                  {query: {_id: 0, $expr: {$eq: ["$a", 5]}}, update: {$set: {b: 6}}, new: true}));

    // $expr with unbound variable throws.
    assert.throws(function() {
        coll.findAndModify(
            {query: {_id: 0, $expr: {$eq: ["$a", "$$unbound"]}}, update: {$set: {b: 6}}});
    });

    // $expr is not allowed in the query when upsert=true.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: 5}));
    assert.throws(function() {
        coll.findAndModify(
            {query: {_id: 0, $expr: {$eq: ["$a", 5]}}, update: {$set: {b: 6}}, upsert: true});
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
    // $expr in geoNear.
    //

    coll.drop();
    assert.writeOK(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, a: 5}));
    assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));
    assert.eq(1,
              assert
                  .commandWorked(db.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {$expr: {$eq: ["$a", 5]}}
                  }))
                  .results.length);
    assert.commandFailed(db.runCommand({
        geoNear: coll.getName(),
        near: {type: "Point", coordinates: [0, 0]},
        spherical: true,
        query: {$expr: {$eq: ["$a", "$$unbound"]}}
    }));

    //
    // $expr in group.
    //

    // The group command is not permitted in sharded collections.
    if (!isMongos) {
        coll.drop();
        assert.writeOK(coll.insert({a: 5}));
        assert.eq([{a: 5, count: 1}], coll.group({
            cond: {$expr: {$eq: ["$a", 5]}},
            key: {a: 1},
            initial: {count: 0},
            reduce: function(curr, result) {
                result.count += 1;
            }
        }));
        assert.throws(function() {
            coll.group({
                cond: {$expr: {$eq: ["$a", "$$unbound"]}},
                key: {a: 1},
                initial: {count: 0},
                reduce: function(curr, result) {
                    result.count += 1;
                }
            });
        });
    }

    //
    // $expr in mapReduce.
    //

    coll.drop();
    assert.writeOK(coll.insert({a: 5}));
    let mapReduceOut = coll.mapReduce(
        function() {
            emit(this.a, 1);
        },
        function(key, values) {
            return Array.sum(values);
        },
        {out: {inline: 1}, query: {$expr: {$eq: ["$a", 5]}}});
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

    //
    // $expr in remove.
    //

    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: 5}));
    let writeRes = coll.remove({_id: 0, $expr: {$eq: ["$a", 5]}});
    assert.writeOK(writeRes);
    assert.eq(1, writeRes.nRemoved);
    assert.writeError(coll.remove({_id: 0, $expr: {$eq: ["$a", "$$unbound"]}}));

    //
    // $expr in update.
    //

    // $expr is allowed in the query when upsert=false.
    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: 5}));
    assert.writeOK(coll.update({_id: 0, $expr: {$eq: ["$a", 5]}}, {$set: {b: 6}}));
    assert.eq({_id: 0, a: 5, b: 6}, coll.findOne({_id: 0}));

    // $expr with unbound variable fails.
    assert.writeError(coll.update({_id: 0, $expr: {$eq: ["$a", "$$unbound"]}}, {$set: {b: 6}}));

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
})();
