// Cannot implicitly shard accessed collections because unsupported use of sharded collection
// for output collection of aggregation pipeline.
//
// @tags: [
//   assumes_unsharded_collection,
//   assumes_write_concern_unchanged,
//   does_not_support_stepdowns,
//   references_foreign_collection,
//   requires_non_retryable_commands,
//   # TODO SERVER-88275: aggregation using a $mergeCursor stage can fail with QueryPlanKilled in
//   # suites with random migrations because moveCollection changes the collection UUID by dropping
//   # and re-creating the collection.
//   assumes_balancer_off,
//   requires_getmore,
// ]

/**
 * Tests related to the aggregate commands behavior with writeConcern and writeConcern + explain.
 */
const collName = "explain_agg_write_concern";
let coll = db[collName];
let outColl = db[collName + "_out"];
coll.drop();
outColl.drop();

assert.commandWorked(coll.insert({_id: 1}));

// Agg should accept write concern if the last stage is a $out.
assert.commandWorked(
    db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$out: outColl.getName()}],
        cursor: {},
        writeConcern: {w: 1},
    }),
);
assert.eq(1, outColl.find().itcount());
outColl.drop();

// Agg should accept writeConcern even if read-only.
assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: [], cursor: {}, writeConcern: {w: 1}}));

// Agg should succeed if the last stage is an $out and the explain flag is set.
assert.commandWorked(
    db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$out: outColl.getName()}],
        explain: true,
    }),
);
assert.eq(0, outColl.find().itcount());
outColl.drop();

// Agg should fail if the last stage is an $out and both the explain flag and writeConcern are
// set.
assert.commandFailed(
    db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$out: outColl.getName()}],
        explain: true,
        writeConcern: {w: 1},
    }),
);

// Agg explain helpers with all verbosities (or verbosity omitted) should fail if the last stage
// is an $out and writeConcern is set.
assert.throws(function () {
    coll.explain().aggregate([{$out: outColl.getName()}], {writeConcern: {w: 1}});
});
assert.throws(function () {
    coll.explain("queryPlanner").aggregate([{$out: outColl.getName()}], {writeConcern: {w: 1}});
});
assert.throws(function () {
    coll.explain("executionStats").aggregate([{$out: outColl.getName()}], {writeConcern: {w: 1}});
});
assert.throws(function () {
    coll.explain("allPlansExecution").aggregate([{$out: outColl.getName()}], {
        writeConcern: {w: 1},
    });
});
