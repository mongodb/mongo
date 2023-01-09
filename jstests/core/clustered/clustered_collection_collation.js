/**
 * Tests that clustered collections respect collation for the _id field and any other fields
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   assumes_no_implicit_index_creation,
 *   does_not_support_stepdowns,
 *   requires_fcv_53,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropCollection.
load("jstests/libs/clustered_collections/clustered_collection_util.js");
load("jstests/libs/clustered_collections/clustered_collection_hint_common.js");

const collatedName = 'clustered_collection_with_collation';
const collated = db[collatedName];

assertDropCollection(db, collatedName);

const noncollatedName = 'clustered_collection_without_collation';
const noncollated = db[noncollatedName];

assertDropCollection(db, noncollatedName);

const defaultCollation = {
    locale: "en",
    strength: 2
};
const incompatibleCollation = {
    locale: "fr_CA",
    strength: 2
};

assert.commandWorked(db.createCollection(
    collatedName, {clusteredIndex: {key: {_id: 1}, unique: true}, collation: defaultCollation}));
assert.commandWorked(
    db.createCollection(noncollatedName, {clusteredIndex: {key: {_id: 1}, unique: true}}));

const expectedCollation = {
    locale: "en",
    caseLevel: false,
    caseFirst: "off",
    strength: 2,
    numericOrdering: false,
    alternate: "non-ignorable",
    maxVariable: "punct",
    normalization: false,
    backwards: false,
    version: "57.1"
};

// Verify clustered collection collation is reflected on the index spec.
const indexes = collated.getIndexes();
assert.eq(0,
          bsonWoCompare(indexes[0].collation, expectedCollation),
          "Default index doesn't match expected collation");

// No collation spec when it's set to "simple".
assertDropCollection(db, "simpleCollation");
assert.commandWorked(db.createCollection(
    "simpleCollation",
    {clusteredIndex: {key: {_id: 1}, unique: true}, collation: {locale: "simple"}}));
const indexSpec = db.simpleCollation.getIndexes()[0];
assert(!indexSpec.hasOwnProperty("collation"), "Default index has collation for \"simple\" locale");

const insertDocuments = function(coll) {
    assert.commandWorked(coll.insert({_id: 5}));
    assert.commandWorked(coll.insert({_id: 10}));

    assert.commandWorked(coll.insert({_id: {int: 5}}));
    assert.commandWorked(coll.insert({_id: {int: 10}}));

    assert.commandWorked(coll.insert({_id: {ints: [5, 10]}}));
    assert.commandWorked(coll.insert({_id: {ints: [15, 20]}}));

    assert.commandWorked(coll.insert({_id: "a"}));
    assert.commandWorked(coll.insert({_id: "b"}));

    assert.commandWorked(coll.insert({_id: {str: "a"}}));
    assert.commandWorked(coll.insert({_id: {str: "b"}}));

    assert.commandWorked(coll.insert({_id: {strs: ["a", "b"]}}));
    assert.commandWorked(coll.insert({_id: {strs: ["c", "d"]}}));

    assert.commandWorked(coll.insert({data: ["a", "b"]}));
    assert.commandWorked(coll.insert({data: ["c", "d"]}));
    // Non _id duplicates are always fine
    assert.commandWorked(coll.insert({data: ["C", "d"]}));
    assert.commandWorked(coll.insert({data: ["C", "D"]}));
};

const testCollatedDuplicates = function(coll, collatedShouldFail) {
    const checkCollated = function(res) {
        if (collatedShouldFail) {
            assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
        } else {
            assert.commandWorked(res);
        }
    };
    // Non string types should always fail
    assert.commandFailedWithCode(coll.insert({_id: 10}), ErrorCodes.DuplicateKey);
    assert.commandFailedWithCode(coll.insert({_id: {int: 10}}), ErrorCodes.DuplicateKey);
    assert.commandFailedWithCode(coll.insert({_id: {ints: [15, 20]}}), ErrorCodes.DuplicateKey);

    // These should only fail when there's a collation
    checkCollated(coll.insert({_id: "B"}));
    checkCollated(coll.insert({_id: {str: "B"}}));
    checkCollated(coll.insert({_id: {strs: ["C", "d"]}}));
    checkCollated(coll.insert({_id: {strs: ["C", "D"]}}));
};

const verifyHasBoundsAndFindsN = function(coll, expected, predicate, queryCollation) {
    const res = queryCollation === undefined
        ? assert.commandWorked(coll.find(predicate).explain())
        : assert.commandWorked(coll.find(predicate).collation(queryCollation).explain());
    const min = assert(res.queryPlanner.winningPlan.minRecord, "No min bound");
    const max = assert(res.queryPlanner.winningPlan.maxRecord, "No max bound");
    assert.eq(min, max, "COLLSCAN bounds are not equal");
    assert.eq(expected, coll.find(predicate).count(), "Didn't find the expected records");
};

const verifyNoBoundsAndFindsN = function(coll, expected, predicate, queryCollation) {
    const res = queryCollation === undefined
        ? assert.commandWorked(coll.find(predicate).explain())
        : assert.commandWorked(coll.find(predicate).collation(queryCollation).explain());
    assert.eq(null, res.queryPlanner.winningPlan.minRecord, "There's a min bound");
    assert.eq(null, res.queryPlanner.winningPlan.maxRecord, "There's a max bound");
    assert.eq(expected, coll.find(predicate).count(), "Didn't find the expected records");
};

const verifyNoTightBoundsAndFindsN = function(coll, expected, predicate, queryCollation) {
    const res = queryCollation === undefined
        ? assert.commandWorked(coll.find(predicate).explain())
        : assert.commandWorked(coll.find(predicate).collation(queryCollation).explain());
    const min = res.queryPlanner.winningPlan.minRecord;
    const max = res.queryPlanner.winningPlan.maxRecord;
    assert.neq(null, min, "No min bound");
    assert.neq(null, max, "No max bound");
    assert.neq(min, max, "COLLSCAN bounds are equal");
    assert.eq(expected, coll.find(predicate).count(), "Didn't find the expected records");
};

const testBounds = function(coll, expected, defaultCollation) {
    // Test non string types.
    verifyHasBoundsAndFindsN(coll, 1, {_id: 5});
    verifyHasBoundsAndFindsN(coll, 1, {_id: {int: 5}});
    verifyHasBoundsAndFindsN(coll, 1, {_id: {ints: [5, 10]}});

    // Test non string types with incompatible collations.
    verifyHasBoundsAndFindsN(coll, 1, {_id: 5}, incompatibleCollation);
    verifyHasBoundsAndFindsN(coll, 1, {_id: {int: 5}}, incompatibleCollation);
    verifyHasBoundsAndFindsN(coll, 1, {_id: {ints: [5, 10]}}, incompatibleCollation);

    // Test strings respect the collation.
    verifyHasBoundsAndFindsN(coll, expected, {_id: "A"});
    verifyHasBoundsAndFindsN(coll, expected, {_id: {str: "A"}});
    verifyHasBoundsAndFindsN(coll, expected, {_id: {strs: ["A", "b"]}});
    verifyHasBoundsAndFindsN(coll, expected, {_id: {strs: ["a", "B"]}});

    // Test strings not in the _id field
    verifyNoBoundsAndFindsN(coll, expected, {data: ["A", "b"]});
    verifyNoBoundsAndFindsN(coll, expected, {data: ["a", "B"]});

    // Test non compatible query collations don't generate bounds
    verifyNoTightBoundsAndFindsN(coll, expected, {_id: "A"}, incompatibleCollation);
    verifyNoTightBoundsAndFindsN(coll, expected, {_id: {str: "A"}}, incompatibleCollation);
    verifyNoTightBoundsAndFindsN(coll, expected, {_id: {strs: ["A", "b"]}}, incompatibleCollation);
    verifyNoTightBoundsAndFindsN(coll, expected, {_id: {strs: ["a", "B"]}}, incompatibleCollation);

    // Test compatible query collations generate bounds
    verifyHasBoundsAndFindsN(coll, expected, {_id: "A"}, defaultCollation);
    verifyHasBoundsAndFindsN(coll, expected, {_id: {str: "A"}}, defaultCollation);
    verifyHasBoundsAndFindsN(coll, expected, {_id: {strs: ["A", "b"]}}, defaultCollation);
    verifyHasBoundsAndFindsN(coll, expected, {_id: {strs: ["a", "B"]}}, defaultCollation);
};

insertDocuments(collated);
insertDocuments(noncollated);

testCollatedDuplicates(collated, true /* should fail */);
testCollatedDuplicates(noncollated, false /* shouldn't fail */);

testBounds(collated, 1 /* expected records */, defaultCollation);
testBounds(noncollated, 0 /*expected records, defaultCollation is undefined */);

/*
 *Test min/max hints
 */

const collatedEncodings = {
    "a": ")\u0001\u0005",
    "C": "-\u0001\u0005"
};

// Strings with default collation.
validateClusteredCollectionHint(collated, {
    expectedNReturned: 2,
    cmd: {find: collatedName, hint: {_id: 1}, min: {_id: "a"}, max: {_id: "C"}},
    expectedWinningPlanStats: {
        stage: "CLUSTERED_IXSCAN",
        direction: "forward",
        minRecord: collatedEncodings["a"],
        maxRecord: collatedEncodings["C"]
    }
});
assert.commandFailedWithCode(
    db.runCommand(
        {explain: {find: noncollatedName, hint: {_id: 1}, min: {_id: "a"}, max: {_id: "C"}}}),
    6137401);  // max() must be greater than min().
validateClusteredCollectionHint(noncollated, {
    expectedNReturned: 3,  // "a", "b" and "B"
    cmd: {find: noncollatedName, hint: {_id: 1}, min: {_id: "A"}, max: {_id: "c"}},
    expectedWinningPlanStats:
        {stage: "CLUSTERED_IXSCAN", direction: "forward", minRecord: "A", maxRecord: "c"}
});

// Strings with incompatible collation.
assert.commandFailedWithCode(
    db.runCommand({
        explain: {
            find: collatedName,
            hint: {_id: 1},
            min: {_id: "a"},
            max: {_id: "C"},
            collation: incompatibleCollation
        }
    }),
    6137400);  // The clustered index is not compatible with the values provided for min/max
assert.commandFailedWithCode(
    db.runCommand({
        explain: {
            find: collatedName,
            hint: {_id: 1},
            min: {_id: "a"},
            max: {_id: "C"},
            collation: incompatibleCollation
        },

    }),
    6137400);  // The clustered index is not compatible with the values provided for min/max

// Numeric with default collation.
validateClusteredCollectionHint(collated, {
    expectedNReturned: 2,
    cmd: {find: collatedName, hint: {_id: 1}, min: {_id: 5}, max: {_id: 11}},
    expectedWinningPlanStats:
        {stage: "CLUSTERED_IXSCAN", direction: "forward", minRecord: 5, maxRecord: 11}
});
validateClusteredCollectionHint(noncollated, {
    expectedNReturned: 2,
    cmd: {find: noncollatedName, hint: {_id: 1}, min: {_id: 5}, max: {_id: 11}},
    expectedWinningPlanStats:
        {stage: "CLUSTERED_IXSCAN", direction: "forward", minRecord: 5, maxRecord: 11}
});

// Numeric with incompatible collation.
validateClusteredCollectionHint(collated, {
    expectedNReturned: 2,
    cmd: {
        find: collatedName,
        hint: {_id: 1},
        min: {_id: 5},
        max: {_id: 11},
        collation: incompatibleCollation
    },
    expectedWinningPlanStats:
        {stage: "CLUSTERED_IXSCAN", direction: "forward", minRecord: 5, maxRecord: 11}
});
validateClusteredCollectionHint(noncollated, {
    expectedNReturned: 2,
    cmd: {
        find: noncollatedName,
        hint: {_id: 1},
        min: {_id: 5},
        max: {_id: 11},
        collation: incompatibleCollation
    },
    expectedWinningPlanStats:
        {stage: "CLUSTERED_IXSCAN", direction: "forward", minRecord: 5, maxRecord: 11}
});
})();
