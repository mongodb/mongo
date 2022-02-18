/**
 * Tests execution of pipeline-style update.
 * @tags: [
 *   requires_multi_updates,
 *   requires_non_retryable_writes,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

const collName = "update_with_pipeline";
const coll = db[collName];

assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({"y.$**": 1}));

/**
 * Confirms that an update returns the expected set of documents. 'nModified' documents from
 * 'resultDocList' must match. 'nModified' may be smaller then the number of elements in
 * 'resultDocList'. This allows for the case where there are multiple documents that could be
 * updated, but only one is actually updated due to a 'multi: false' argument. Constant values
 * to the update command are passed in the 'constants' argument.
 */
function testUpdate({
    query,
    initialDocumentList,
    update,
    resultDocList,
    nModified,
    options = {},
    constants = undefined
}) {
    assert.eq(initialDocumentList.length, resultDocList.length);
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert(initialDocumentList));
    const upd = Object.assign({q: query, u: update}, options);
    if (constants !== undefined) {
        upd.c = constants;
    }
    const res = assert.commandWorked(db.runCommand({update: collName, updates: [upd]}));
    assert.eq(nModified, res.nModified);

    let nMatched = 0;
    for (let i = 0; i < resultDocList.length; ++i) {
        if (0 === bsonWoCompare(coll.findOne(resultDocList[i]), resultDocList[i])) {
            ++nMatched;
        }
    }
    assert.eq(nModified, nMatched, `actual=${coll.find().toArray()}, expected=${resultDocList}`);
}

function testUpsertDoesInsert(query, update, resultDoc) {
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.update(query, update, {upsert: true}));
    assert.eq(coll.findOne({}), resultDoc, coll.find({}).toArray());
}

// This can be used to make sure pipeline-based updates generate delta oplog entries.
const largeStr = "x".repeat(1000);

// Update with existing document.
testUpdate({
    query: {_id: 1},
    initialDocumentList: [{_id: 1, x: 1, largeStr: largeStr}],
    update: [{$set: {foo: 4}}],
    resultDocList: [{_id: 1, x: 1, largeStr: largeStr, foo: 4}],
    nModified: 1
});
testUpdate({
    query: {_id: 1},
    initialDocumentList: [{_id: 1, x: 1, y: 1}],
    update: [{$project: {x: 1}}],
    resultDocList: [{_id: 1, x: 1}],
    nModified: 1
});
testUpdate({
    query: {_id: 1},
    initialDocumentList: [{_id: 1, x: 1, y: [{z: 1, foo: 1}], largeStr: largeStr}],
    update: [{$unset: ["x", "y.z"]}],
    resultDocList: [{_id: 1, y: [{foo: 1}], largeStr: largeStr}],
    nModified: 1
});
testUpdate({
    query: {_id: 1},
    initialDocumentList: [{_id: 1, x: 1, t: {u: {v: 1}, largeStr: largeStr}}],
    update: [{$replaceWith: "$t"}],
    resultDocList: [{_id: 1, u: {v: 1}, largeStr: largeStr}],
    nModified: 1
});

// Multi-update.
testUpdate({
    query: {x: 1},
    initialDocumentList: [{_id: 1, x: 1, largeStr: largeStr}, {_id: 2, x: 1, largeStr: largeStr}],
    update: [{$set: {bar: 4}}],
    resultDocList:
        [{_id: 1, x: 1, largeStr: largeStr, bar: 4}, {_id: 2, x: 1, largeStr: largeStr, bar: 4}],
    nModified: 2,
    options: {multi: true}
});

// This test will fail in a sharded cluster when the 2 initial documents live on different
// shards.
if (!FixtureHelpers.isMongos(db)) {
    testUpdate({
        query: {_id: {$in: [1, 2]}},
        initialDocumentList: [{_id: 1, x: 1}, {_id: 2, x: 2}],
        update: [{$set: {bar: 4}}],
        resultDocList: [{_id: 1, x: 1, bar: 4}, {_id: 2, x: 2, bar: 4}],
        nModified: 1,
        options: {multi: false}
    });
}

// Upsert performs insert.
testUpsertDoesInsert({_id: 1, x: 1}, [{$set: {foo: 4}}], {_id: 1, x: 1, foo: 4});
testUpsertDoesInsert({_id: 1, x: 1}, [{$project: {x: 1}}], {_id: 1, x: 1});
testUpsertDoesInsert({_id: 1, x: 1}, [{$project: {x: "foo"}}], {_id: 1, x: "foo"});
testUpsertDoesInsert({_id: 1, x: 1, y: 1}, [{$unset: ["x"]}], {_id: 1, y: 1});

// Upsert with 'upsertSupplied' inserts the given document and populates _id from the query.
assert.commandWorked(db.runCommand({
    update: coll.getName(),
    updates: [{
        q: {_id: "supplied_doc"},
        u: [{$set: {x: 1}}],
        upsert: true,
        upsertSupplied: true,
        c: {new: {suppliedDoc: true}}
    }]
}));
assert(coll.findOne({_id: "supplied_doc", suppliedDoc: true}));

// Update with 'upsertSupplied:true' fails if 'upsert' is false.
assert.commandFailedWithCode(db.runCommand({
    update: coll.getName(),
    updates: [{
        q: {_id: "supplied_doc"},
        u: [{$set: {x: 1}}],
        upsert: false,
        upsertSupplied: true,
        c: {new: {suppliedDoc: true}}
    }]
}),
                             ErrorCodes.FailedToParse);

// Upsert with 'upsertSupplied' fails if no constants are provided.
assert.commandFailedWithCode(db.runCommand({
    update: coll.getName(),
    updates: [{q: {_id: "supplied_doc"}, u: [{$set: {x: 1}}], upsert: true, upsertSupplied: true}]
}),
                             ErrorCodes.FailedToParse);

// Upsert with 'upsertSupplied' fails if constants do not include a field called 'new'.
assert.commandFailedWithCode(db.runCommand({
    update: coll.getName(),
    updates:
        [{q: {_id: "supplied_doc"}, u: [{$set: {x: 1}}], upsert: true, upsertSupplied: true, c: {}}]
}),
                             ErrorCodes.FailedToParse);

// Upsert with 'upsertSupplied' fails if c.new is not an object.
assert.commandFailedWithCode(db.runCommand({
    update: coll.getName(),
    updates: [{
        q: {_id: "supplied_doc"},
        u: [{$set: {x: 1}}],
        upsert: true,
        upsertSupplied: true,
        c: {new: "string"}
    }]
}),
                             ErrorCodes.FailedToParse);

// Update fails when invalid stage is specified. This is a sanity check rather than an exhaustive
// test of all stages.
assert.commandFailedWithCode(coll.update({x: 1}, [{$match: {x: 1}}]), ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(coll.update({x: 1}, [{$sort: {x: 1}}]), ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(coll.update({x: 1}, [{$facet: {a: [{$match: {x: 1}}]}}]),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(
    coll.update(
        {x: 1}, [{
            $bucket: {groupBy: "$a", boundaries: [0, 1], default: "foo", output: {count: {$sum: 1}}}
        }]),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(
    coll.update({x: 1}, [{$lookup: {from: "foo", as: "as", localField: "a", foreignField: "b"}}]),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(
    coll.update(
        {x: 1}, [{
            $graphLookup:
                {from: "foo", startWith: "$a", connectFromField: "a", connectToField: "b", as: "as"}
        }]),
    ErrorCodes.InvalidOptions);

// $indexStats is not supported in a transaction passthrough and will fail with a different error.
assert.commandFailedWithCode(
    coll.update({x: 1}, [{$indexStats: {}}]),
    [ErrorCodes.InvalidOptions, ErrorCodes.OperationNotSupportedInTransaction]);

// Update fails when supported agg stage is specified outside of pipeline.
assert.commandFailedWithCode(coll.update({_id: 1}, {$addFields: {x: 1}}), ErrorCodes.FailedToParse);

// The 'arrayFilters' option is not valid for pipeline updates.
assert.commandFailedWithCode(
    coll.update({_id: 1}, [{$set: {x: 1}}], {arrayFilters: [{x: {$eq: 1}}]}),
    ErrorCodes.FailedToParse);

// Constants can be specified with pipeline-style updates.
testUpdate({
    query: {_id: 1},
    initialDocumentList: [{_id: 1, x: 1}],
    useUpdateCommand: true,
    constants: {foo: "bar"},
    update: [{$set: {foo: "$$foo"}}],
    resultDocList: [{_id: 1, x: 1, foo: "bar"}],
    nModified: 1
});
testUpdate({
    query: {_id: 1},
    initialDocumentList: [{_id: 1, x: 1}],
    useUpdateCommand: true,
    constants: {foo: {a: {b: {c: "bar"}}}},
    update: [{$set: {foo: "$$foo"}}],
    resultDocList: [{_id: 1, x: 1, foo: {a: {b: {c: "bar"}}}}],
    nModified: 1
});
testUpdate({
    query: {_id: 1},
    initialDocumentList: [{_id: 1, x: 1}],
    useUpdateCommand: true,
    constants: {foo: [1, 2, 3]},
    update: [{$set: {foo: {$arrayElemAt: ["$$foo", 2]}}}],
    resultDocList: [{_id: 1, x: 1, foo: 3}],
    nModified: 1
});

testUpdate({
    query: {_id: 1},
    initialDocumentList: [{_id: 1, x: 1}],
    useUpdateCommand: true,
    constants: {largeStr: largeStr},
    update: [{$set: {foo: "$$largeStr"}}],
    resultDocList: [{_id: 1, x: 1, foo: largeStr}],
    nModified: 1
});

// References to document fields are not resolved in constants.
testUpdate({
    query: {_id: 1},
    initialDocumentList: [{_id: 1, x: 1}],
    useUpdateCommand: true,
    constants: {foo: "$x"},
    update: [{$set: {foo: "$$foo"}}],
    resultDocList: [{_id: 1, x: 1, foo: "$x"}],
    nModified: 1
});

// Test that expressions within constants are treated as field names instead of expressions.
db.runCommand({
    update: collName,
    updates: [{q: {_id: 1}, u: [{$set: {x: "$$foo"}}], c: {foo: {$add: [1, 2]}}}]
});
assert.eq([{_id: 1, x: {$add: [1, 2]}, foo: "$x"}], coll.find({_id: 1}).toArray());

// Cannot use constants with regular updates.
assert.commandFailedWithCode(
    db.runCommand({update: collName, updates: [{q: {_id: 1}, u: {x: "$$foo"}, c: {foo: "bar"}}]}),
    51198);
assert.commandFailedWithCode(
    db.runCommand(
        {update: collName, updates: [{q: {_id: 1}, u: {$set: {x: "$$foo"}}, c: {foo: "bar"}}]}),
    51198);
assert.commandFailedWithCode(
    db.runCommand({update: collName, updates: [{q: {_id: 1}, u: {$set: {x: "1"}}, c: {}}]}), 51198);
})();
