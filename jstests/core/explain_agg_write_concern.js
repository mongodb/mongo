// Cannot implicitly shard accessed collections because unsupported use of sharded collection
// for output collection of aggregation pipeline.
// @tags: [assumes_unsharded_collection]

/**
 * Tests related to the aggregate commands behavior with writeConcern and writeConcern + explain.
 */
(function() {
    "use strict";

    let coll = db[jsTest.name()];
    let outColl = db[jsTest.name() + "_out"];
    coll.drop();
    outColl.drop();

    assert.writeOK(coll.insert({_id: 1}));

    // Agg should accept write concern if the last stage is a $out.
    assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$out: outColl.getName()}],
        cursor: {},
        writeConcern: {w: 1}
    }));
    assert.eq(1, outColl.find().itcount());
    outColl.drop();

    // Agg should reject writeConcern if the last stage is not an $out.
    assert.commandFailed(
        db.runCommand({aggregate: coll.getName(), pipeline: [], cursor: {}, writeConcern: {w: 1}}));

    // Agg should succeed if the last stage is an $out and the explain flag is set.
    assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$out: outColl.getName()}],
        explain: true,
    }));
    assert.eq(0, outColl.find().itcount());
    outColl.drop();

    // Agg should fail if the last stage is an $out and both the explain flag and writeConcern are
    // set.
    assert.commandFailed(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$out: outColl.getName()}],
        explain: true,
        writeConcern: {w: 1}
    }));

    // Agg explain helpers with all verbosities (or verbosity omitted) should fail if the last stage
    // is an $out and writeConcern is set.
    assert.throws(function() {
        coll.explain().aggregate([{$out: outColl.getName()}], {writeConcern: {w: 1}});
    });
    assert.throws(function() {
        coll.explain("queryPlanner").aggregate([{$out: outColl.getName()}], {writeConcern: {w: 1}});
    });
    assert.throws(function() {
        coll.explain("executionStats").aggregate([{$out: outColl.getName()}], {
            writeConcern: {w: 1}
        });
    });
    assert.throws(function() {
        coll.explain("allPlansExecution").aggregate([{$out: outColl.getName()}], {
            writeConcern: {w: 1}
        });
    });
}());
