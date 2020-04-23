/**
 * Test projections which have conflicts in them.
 */
(function() {
"use strict";

const coll = db.projection_conflicts;
coll.drop();

assert.commandWorked(coll.insert({a: {b: 1}}));

function checkProjectionFailsWithCode(proj, codes) {
    const collisionErrorCodes = [31250, 31249];

    const err = assert.throws(() => coll.find({}, proj).toArray());
    assert.contains(err.code, collisionErrorCodes, proj);

    const aggErr = assert.throws(() => coll.aggregate({$project: proj}));
    assert.contains(err.code, collisionErrorCodes, proj);
}

// Can't test cases like {a: 1, a: "$$REMOVE"} because JS will remove the duplicate keys.
checkProjectionFailsWithCode({"a.b": 1, a: {b: "$$REMOVE"}});

// Inclusion only.
checkProjectionFailsWithCode({"a.b": 1, a: {b: {c: 1}}});
checkProjectionFailsWithCode({a: {b: {c: 1}}, "a.b": 1});

checkProjectionFailsWithCode({a: 1, "a.b": 1});
checkProjectionFailsWithCode({"a.b": 1, a: 1});

checkProjectionFailsWithCode({"a.b": 1, "a.b.c": 1});

// Exclusion only.
checkProjectionFailsWithCode({"a.b": 0, a: {b: {c: 0}}});
checkProjectionFailsWithCode({a: {b: {c: 0}}, "a.b": 0});

checkProjectionFailsWithCode({a: 0, "a.b": 0});
checkProjectionFailsWithCode({"a.b": 0, a: 0});

checkProjectionFailsWithCode({"a.b": 0, "a.b.c": 0});
})();
