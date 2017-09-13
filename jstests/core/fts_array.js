/**
 * Tests for the interaction between FTS indexes and arrays.
 */
(function() {
    "use strict";

    let coll = db.jstests_fts_array;
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.b": 1, words: "text"}));

    // Verify that the leading field of the index cannot contain an array.
    assert.writeErrorWithCode(coll.insert({a: {b: []}, words: "omnibus"}),
                              ErrorCodes.CannotBuildIndexKeys);
    assert.writeErrorWithCode(coll.insert({a: {b: [1]}, words: "omnibus"}),
                              ErrorCodes.CannotBuildIndexKeys);
    assert.writeErrorWithCode(coll.insert({a: {b: [1, 2]}, words: "omnibus"}),
                              ErrorCodes.CannotBuildIndexKeys);
    assert.writeErrorWithCode(coll.insert({a: [], words: "omnibus"}),
                              ErrorCodes.CannotBuildIndexKeys);
    assert.writeErrorWithCode(coll.insert({a: [{b: 1}], words: "omnibus"}),
                              ErrorCodes.CannotBuildIndexKeys);
    assert.writeErrorWithCode(coll.insert({a: [{b: 1}, {b: 2}], words: "omnibus"}),
                              ErrorCodes.CannotBuildIndexKeys);

    coll.drop();
    assert.commandWorked(coll.createIndex({words: "text", "y.z": 1}));

    // Verify that the trailing field of the index cannot contain an array.
    assert.writeErrorWithCode(coll.insert({words: "macerate", y: {z: []}}),
                              ErrorCodes.CannotBuildIndexKeys);
    assert.writeErrorWithCode(coll.insert({words: "macerate", y: {z: [1]}}),
                              ErrorCodes.CannotBuildIndexKeys);
    assert.writeErrorWithCode(coll.insert({words: "macerate", y: {z: [1, 2]}}),
                              ErrorCodes.CannotBuildIndexKeys);
    assert.writeErrorWithCode(coll.insert({words: "macerate", y: []}),
                              ErrorCodes.CannotBuildIndexKeys);
    assert.writeErrorWithCode(coll.insert({words: "macerate", y: [{z: 1}]}),
                              ErrorCodes.CannotBuildIndexKeys);
    assert.writeErrorWithCode(coll.insert({words: "macerate", y: [{z: 1}, {z: 2}]}),
                              ErrorCodes.CannotBuildIndexKeys);

    // Verify that array fields are allowed when positionally indexed.
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.0": 1, words: "text"}));
    assert.writeOK(coll.insert({a: [0, 1, 2], words: "dander"}));
    assert.eq({a: [0, 1, 2], words: "dander"},
              coll.findOne({"a.0": 0, $text: {$search: "dander"}}, {_id: 0, a: 1, words: 1}));
    assert.writeErrorWithCode(coll.insert({a: [[8, 9], 1, 2], words: "dander"}),
                              ErrorCodes.CannotBuildIndexKeys);
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.0.1": 1, words: "text"}));
    assert.writeOK(coll.insert({a: [[8, 9], 1, 2], words: "dander"}));
    assert.eq({a: [[8, 9], 1, 2], words: "dander"},
              coll.findOne({"a.0.1": 9, $text: {$search: "dander"}}, {_id: 0, a: 1, words: 1}));
}());
