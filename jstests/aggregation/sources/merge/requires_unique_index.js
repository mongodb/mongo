// Tests that the $merge stage enforces that the "on" fields can be used to uniquely identify
// documents by checking that there is a supporting unique, non-partial, collator-compatible index
// in the index catalog.
//
// Note that this test does *not* use the drop shell helper but instead runs the drop command
// manually. This is to avoid implicit creation and sharding of the $merge target collections in the
// passthrough suites.
(function() {
"use strict";

load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode,
                                                      // assertMergeFailsWithoutUniqueIndex.

const testDB = db.getSiblingDB("merge_requires_unique_index");
assert.commandWorked(testDB.dropDatabase());

const source = testDB.source;
assert.commandWorked(source.insert([{_id: 0, a: 0}, {_id: 1, a: 1}]));

// Helper to drop a collection without using the shell helper, and thus avoiding the implicit
// recreation in the passthrough suites.
function dropWithoutImplicitRecreate(coll) {
    testDB.runCommand({drop: coll.getName()});
}

// Test that using {_id: 1} or not providing a unique key does not require any special indexes.
(function simpleIdOnFieldsOrDefaultShouldNotRequireIndexes() {
    function assertDefaultOnFieldsSucceeds({setupCallback, collName}) {
        withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
            // Skip the combination of merge modes which will fail depending on the contents of
            // the source and target collection, as this will cause the assertion below to trip.
            if (whenMatchedMode == "fail" || whenNotMatchedMode == "fail")
                return;

            setupCallback();
            assert.doesNotThrow(() => source.aggregate([{
                $merge: {
                    into: collName,
                    whenMatched: whenMatchedMode,
                    whenNotMatched: whenNotMatchedMode
                }
            }]));
            setupCallback();
            assert.doesNotThrow(() => source.aggregate([{
                $merge: {
                    into: collName,
                    on: "_id",
                    whenMatched: whenMatchedMode,
                    whenNotMatched: whenNotMatchedMode
                }
            }]));
        });
    }

    // Test that using "_id" or not specifying "on" fields works for a collection which does
    // not exist.
    const non_existent = testDB.non_existent;
    assertDefaultOnFieldsSucceeds({
        setupCallback: () => dropWithoutImplicitRecreate(non_existent),
        collName: non_existent.getName()
    });

    const unindexed = testDB.unindexed;
    assertDefaultOnFieldsSucceeds({
        setupCallback: () => {
            dropWithoutImplicitRecreate(unindexed);
            assert.commandWorked(testDB.runCommand({create: unindexed.getName()}));
        },
        collName: unindexed.getName()
    });
}());

// Test that a unique index on the "on" fields can be used to satisfy the requirement.
(function basicUniqueIndexWorks() {
    const target = testDB.regular_unique;
    dropWithoutImplicitRecreate(target);
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["_id", "a"], target: target});

    assert.commandWorked(testDB.runCommand({create: target.getName()}));
    assert.commandWorked(target.createIndex({a: 1, _id: 1}, {unique: true}));
    assert.doesNotThrow(() => source.aggregate([{
        $merge: {
            into: target.getName(),
            whenMatched: "replace",
            whenNotMatched: "insert",
            on: ["_id", "a"]
        }
    }]));
    assert.doesNotThrow(() => source.aggregate([{
        $merge: {
            into: target.getName(),
            whenMatched: "replace",
            whenNotMatched: "insert",
            on: ["a", "_id"]
        }
    }]));

    assertMergeFailsWithoutUniqueIndex(
        {source: source, onFields: ["_id", "a", "b"], target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["a", "b"], target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["b"], target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["a"], target: target});

    assert.commandWorked(target.dropIndex({a: 1, _id: 1}));
    assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
    assert.doesNotThrow(() => source.aggregate([{
        $merge: {into: target.getName(), whenMatched: "replace", whenNotMatched: "insert", on: "a"}
    }]));

    // Create a non-unique index and make sure that doesn't work.
    assert.commandWorked(target.dropIndex({a: 1}));
    assert.commandWorked(target.createIndex({a: 1}));
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "a", target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["_id", "a"], target: target});
}());

// Test that a unique index on the "on" fields cannot be used to satisfy the requirement if it
// is a partial index.
(function uniqueButPartialShouldNotWork() {
    const target = testDB.unique_but_partial_indexes;
    dropWithoutImplicitRecreate(target);
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "a", target: target});

    assert.commandWorked(
        target.createIndex({a: 1}, {unique: true, partialFilterExpression: {a: {$gte: 2}}}));
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "a", target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["_id", "a"], target: target});
}());

// Test that a unique index on the "on" fields cannot be used to satisfy the requirement if it
// has a different collation.
(function indexMustMatchCollationOfOperation() {
    const target = testDB.collation_indexes;
    dropWithoutImplicitRecreate(target);
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "a", target: target});

    assert.commandWorked(target.createIndex({a: 1}, {unique: true, collation: {locale: "en_US"}}));
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "a", target: target});
    assertMergeFailsWithoutUniqueIndex(
        {source: source, onFields: "a", target: target, options: {collation: {locale: "en"}}});
    assertMergeFailsWithoutUniqueIndex(
        {source: source, onFields: "a", target: target, options: {collation: {locale: "simple"}}});
    assertMergeFailsWithoutUniqueIndex({
        source: source,
        onFields: "a",
        target: target,
        options: {collation: {locale: "en_US", strength: 1}}
    });
    assert.doesNotThrow(() => source.aggregate([{
                                                   $merge: {
                                                       into: target.getName(),
                                                       whenMatched: "replace",
                                                       whenNotMatched: "insert",
                                                       on: "a"
                                                   }
                                               }],
                                               {collation: {locale: "en_US"}}));

    // Test that a non-unique index with the same collation cannot be used.
    assert.commandWorked(target.dropIndex({a: 1}));
    assert.commandWorked(target.createIndex({a: 1}, {collation: {locale: "en_US"}}));
    assertMergeFailsWithoutUniqueIndex(
        {source: source, onFields: "a", target: target, options: {collation: {locale: "en_US"}}});

    // Test that a collection-default collation will be applied to the index, but not the
    // $merge's update or insert into that collection. The pipeline will inherit a
    // collection-default collation, but from the source collection, not the $merge's target
    // collection.
    dropWithoutImplicitRecreate(target);
    assert.commandWorked(
        testDB.runCommand({create: target.getName(), collation: {locale: "en_US"}}));
    assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
    assertMergeFailsWithoutUniqueIndex({
        source: source,
        onFields: "a",
        target: target,
    });
    assert.doesNotThrow(() => source.aggregate([{
                                                   $merge: {
                                                       into: target.getName(),
                                                       whenMatched: "replace",
                                                       whenNotMatched: "insert",
                                                       on: "a"
                                                   }
                                               }],
                                               {collation: {locale: "en_US"}}));

    // Test that when the source collection and foreign collection have the same default
    // collation, a unique index on the foreign collection can be used.
    const newSourceColl = testDB.new_source;
    dropWithoutImplicitRecreate(newSourceColl);
    assert.commandWorked(
        testDB.runCommand({create: newSourceColl.getName(), collation: {locale: "en_US"}}));
    assert.commandWorked(newSourceColl.insert([{_id: 1, a: 1}, {_id: 2, a: 2}]));
    // This aggregate does not specify a collation, but it should inherit the default collation
    // from 'newSourceColl', and therefore the index on 'target' should be eligible for use
    // since it has the same collation.
    assert.doesNotThrow(() => newSourceColl.aggregate([{
        $merge: {into: target.getName(), whenMatched: "replace", whenNotMatched: "insert", on: "a"}
    }]));

    // Test that an explicit "simple" collation can be used with an index without a collation.
    dropWithoutImplicitRecreate(target);
    assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
    assert.doesNotThrow(() => source.aggregate([{
                                                   $merge: {
                                                       into: target.getName(),
                                                       whenMatched: "replace",
                                                       whenNotMatched: "insert",
                                                       on: "a"
                                                   }
                                               }],
                                               {collation: {locale: "simple"}}));
    assertMergeFailsWithoutUniqueIndex(
        {source: source, onFields: "a", target: target, options: {collation: {locale: "en_US"}}});
}());

// Test that a unique index which is not simply ascending/descending fields cannot be used for
// the "on" fields.
(function testSpecialIndexTypes() {
    const target = testDB.special_index_types;
    dropWithoutImplicitRecreate(target);

    assert.commandWorked(target.createIndex({a: 1, text: "text"}, {unique: true}));
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "a", target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["a", "text"], target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "text", target: target});

    dropWithoutImplicitRecreate(target);
    assert.commandWorked(target.createIndex({a: 1, geo: "2dsphere"}, {unique: true}));
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "a", target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["a", "geo"], target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["geo", "a"], target: target});

    dropWithoutImplicitRecreate(target);
    assert.commandWorked(target.createIndex({geo: "2d"}, {unique: true}));
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["a", "geo"], target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "geo", target: target});

    dropWithoutImplicitRecreate(target);
    assert.commandWorked(
        target.createIndex({geo: "geoHaystack", a: 1}, {unique: true, bucketSize: 5}));
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["a", "geo"], target: target});
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: ["geo", "a"], target: target});

    dropWithoutImplicitRecreate(target);
    // MongoDB does not support unique hashed indexes.
    assert.commandFailedWithCode(target.createIndex({a: "hashed"}, {unique: true}), 16764);
    assert.commandWorked(target.createIndex({a: "hashed"}));
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "a", target: target});
}());

// Test that a unique index with dotted field names can be used.
(function testDottedFieldNames() {
    const target = testDB.dotted_field_paths;
    dropWithoutImplicitRecreate(target);

    assert.commandWorked(target.createIndex({a: 1, "b.c.d": -1}, {unique: true}));
    assertMergeFailsWithoutUniqueIndex({source: source, onFields: "a", target: target});
    assert.doesNotThrow(() => source.aggregate([
        {$project: {_id: 1, a: 1, b: {c: {d: "x"}}}},
        {
            $merge: {
                into: target.getName(),
                whenMatched: "replace",
                whenNotMatched: "insert",
                on: ["a", "b.c.d"]
            }
        }
    ]));

    dropWithoutImplicitRecreate(target);
    assert.commandWorked(target.createIndex({"id.x": 1, "id.y": -1}, {unique: true}));
    assert.doesNotThrow(() => source.aggregate([
        {$group: {_id: {x: "$_id", y: "$a"}}},
        {$project: {id: "$_id"}},
        {
            $merge: {
                into: target.getName(),
                whenMatched: "replace",
                whenNotMatched: "insert",
                on: ["id.x", "id.y"]
            }
        }
    ]));
    assert.doesNotThrow(() => source.aggregate([
        {$group: {_id: {x: "$_id", y: "$a"}}},
        {$project: {id: "$_id"}},
        {
            $merge: {
                into: target.getName(),
                whenMatched: "replace",
                whenNotMatched: "insert",
                on: ["id.y", "id.x"]
            }
        }
    ]));

    // Test that we cannot use arrays with a dotted path within a $merge.
    dropWithoutImplicitRecreate(target);
    assert.commandWorked(target.createIndex({"b.c": 1}, {unique: true}));
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        assert.commandFailedWithCode(testDB.runCommand({
            aggregate: source.getName(),
            pipeline: [
                {$replaceRoot: {newRoot: {b: [{c: 1}, {c: 2}]}}},
                {
                    $merge: {
                        into: target.getName(),
                        whenMatched: whenMatchedMode,
                        whenNotMatched: whenNotMatchedMode,
                        on: "b.c"
                    }
                }
            ],
            cursor: {}
        }),
                                     [50905, 51132]);
    });
}());

// Test that a unique index that is multikey can still be used.
(function testMultikeyIndex() {
    const target = testDB.multikey_index;
    dropWithoutImplicitRecreate(target);

    assert.commandWorked(target.createIndex({"a.b": 1}, {unique: true}));
    assert.doesNotThrow(() => source.aggregate([
        {$project: {_id: 1, "a.b": "$a"}},
        {
            $merge: {
                into: target.getName(),
                whenMatched: "replace",
                whenNotMatched: "insert",
                on: "a.b"
            }
        }
    ]));
    assert.commandWorked(target.insert({_id: "TARGET", a: [{b: "hi"}, {b: "hello"}]}));
    assert.commandWorked(source.insert({a: "hi", proofOfUpdate: "PROOF"}));
    assert.doesNotThrow(() => source.aggregate([
        {$project: {_id: 0, proofOfUpdate: "PROOF", "a.b": "$a"}},
        {
            $merge: {
                into: target.getName(),
                whenMatched: "replace",
                whenNotMatched: "insert",
                on: "a.b"
            }
        }
    ]));
    assert.docEq(target.findOne({"a.b": "hi", proofOfUpdate: "PROOF"}),
                 {_id: "TARGET", a: {b: "hi"}, proofOfUpdate: "PROOF"});
}());

// Test that a unique index that is sparse can still be used.
(function testSparseIndex() {
    const target = testDB.multikey_index;
    dropWithoutImplicitRecreate(target);

    assert.commandWorked(target.createIndex({a: 1}, {unique: true, sparse: true}));
    assert.doesNotThrow(() => source.aggregate([{
        $merge: {into: target.getName(), whenMatched: "replace", whenNotMatched: "insert", on: "a"}
    }]));
    assert.commandWorked(target.insert([{b: 1, c: 1}, {a: null}, {d: 4}]));
    assert.doesNotThrow(() => source.aggregate([{
        $merge: {into: target.getName(), whenMatched: "replace", whenNotMatched: "insert", on: "a"}
    }]));
}());
}());
