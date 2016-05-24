/**
 * apply_ops_errors.js
 *
 * This file tests the output of the applyOps command when an exception occurs.
 * This test checks two scenarios, one where the only command fails, and one where
 * the second of three commands fails.
 * The expected outcome is that applyOps terminates and returns the error code,
 * the error message, and a list of booleans specifying which operations succeeded
 * and which ones failed. In the first case the one operation's result returns
 * false in the results array. In the second case the first operation should return
 * true, the second should return false, and the third should not be run.
 * This is all in accordance with SERVER-10771.
 */

(function() {
    "use strict";
    var coll = db.apply_ops_errors;
    coll.drop();

    // Scenario 1: only one operation
    assert.eq(0, coll.find().count(), "test collection not empty");
    coll.ensureIndex({x: 1}, {unique: true});
    coll.insert({_id: 1, x: "init"});

    var res = db.runCommand({
        applyOps: [
            {op: "i", ns: coll.getFullName(), o: {_id: 2, x: "init"}},
        ]
    });

    assert.eq(1, res.applied);
    assert(res.code);
    assert(res.errmsg);
    assert.eq([false], res.results);
    assert.eq(0, res.ok);

    coll.drop();

    // Scenario 2: Three operations, first two should run, second should fail.
    assert.eq(0, coll.find().count(), "test collection not empty");
    coll.ensureIndex({x: 1}, {unique: true});
    coll.insert({_id: 1, x: "init"});

    var res = db.runCommand({
        applyOps: [
            {op: "i", ns: coll.getFullName(), o: {_id: 3, x: "not init"}},
            {op: "i", ns: coll.getFullName(), o: {_id: 4, x: "init"}},
            {op: "i", ns: coll.getFullName(), o: {_id: 5, x: "not init again"}},
        ]
    });

    assert.eq(2, res.applied);
    assert(res.code);
    assert(res.errmsg);
    assert.eq([false, false], res.results);
    assert.eq(0, res.ok);
})();
