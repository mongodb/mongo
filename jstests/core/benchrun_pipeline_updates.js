/**
 * Tests that benchRun can understand pipeline-style updates and findAndModifys.
 *
 * @tags: [uses_multiple_connections]
 */
(function() {
    "use strict";
    const coll = db.benchrun_pipeline_updates;
    coll.drop();

    assert.commandWorked(coll.insert({_id: 0, x: 0}));

    // Test that a basic pipeline can be used by an update op.
    let benchArgs = {
        ops: [
            {
              op: "update",
              ns: coll.getFullName(),
              query: {_id: 0},
              writeCmd: true,
              update: [{$set: {x: {$add: ["$x", 1]}}}]
            },
        ],
        parallel: 2,
        seconds: 1,
        host: db.getMongo().host,
    };
    if (jsTest.options().auth) {
        benchArgs['db'] = 'admin';
        benchArgs['username'] = jsTest.options().authUser;
        benchArgs['password'] = jsTest.options().authPassword;
    }
    let res = benchRun(benchArgs);
    assert.eq(res.errCount, 0);
    assert.gt(
        coll.findOne({_id: 0}).x, 2, "Expected at least one update to succeed and increment 'x'");

    // Now test that the pipeline is still subject to benchRun's keyword replacement.

    // Initialize x to something outside the range we'll expect it to be in below if the updates
    // succeed.
    assert.commandWorked(coll.updateOne({_id: 0}, {$set: {x: 100}}));
    benchArgs.ops = [{
        op: "update",
        ns: coll.getFullName(),
        query: {_id: 0},
        writeCmd: true,
        update: [{$project: {x: {$literal: {"#RAND_INT_PLUS_THREAD": [0, 2]}}}}]
    }];
    res = benchRun(benchArgs);
    assert.eq(res.errCount, 0);
    assert.lte(
        coll.findOne({_id: 0}).x, 3, "Expected 'x' to be no more than 3 after randInt replacement");
}());
