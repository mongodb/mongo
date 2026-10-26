/**
 * Tests the express code path, which bypasses regular query planning and execution, for writes.
 * Verifies some basic eligibility restrictions such as match expression shape and index options,
 * and checks the query results.
 * @tags: [
 *   uses_explain,
 *   requires_fcv_81,
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   requires_getmore,
 *   # Primary-driven index builds must have batched writes enabled which config.image_collection
 *   # does not support.
 *   primary_driven_index_builds_incompatible_with_retryable_writes,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {isExpress} from "jstests/libs/query/analyze_plan.js";

// TODO (SERVER-117130): Remove the mongos pinning once the related issue is resolved.
// When a database is dropped, a stale router will report "database not found" error for
// deletes (instead of "ok") when pauseMigrationsDuringMultiUpdates is enabled.
if (TestData.pauseMigrationsDuringMultiUpdates) {
    TestData.pinToSingleMongos = true;
}

const collName = "express_write_coll";
const coll = db.getCollection(collName);
const docs = [
    {_id: 0, a: 0},
    {_id: "str", a: 1},
];

function runExpressTest({command, expectedDocs, usesExpress}) {
    // Reset the collection docs.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert(docs));

    // Run the command to make sure it succeeds.
    assert.commandWorked(db.runCommand(command));
    assertArrayEq({
        actual: coll.find().toArray(),
        expected: expectedDocs,
        extraErrorMsg: "Result set comparison failed for command: " + tojson(command),
    });

    // Reset the collection docs then run explain.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert(docs));
    const explain = assert.commandWorked(
        db.runCommand({explain: command, verbosity: "executionStats"}),
    );

    assert.eq(
        usesExpress,
        isExpress(db, explain),
        "Expected the query to " + (usesExpress ? "" : "not ") + "use express: " + tojson(explain),
    );
}

coll.drop();

// Cannot use express path when predicate is not a single equality or when a projection is present.
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: 0, a: 0}, limit: 0}]},
    usesExpress: false,
    expectedDocs: [docs[1]],
});
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: 0, a: 0}, limit: 1}]},
    usesExpress: false,
    expectedDocs: [docs[1]],
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: "str", a: 1}, update: [{$set: {c: 1}}]},
    usesExpress: false,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}],
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: 0}, remove: true, fields: {_id: 1, a: 1}},
    usesExpress: false,
    expectedDocs: [docs[1]],
});
runExpressTest({
    command: {
        findAndModify: collName,
        query: {_id: "str"},
        update: {$set: {c: 1}},
        fields: {_id: 1, a: 1, c: 1},
    },
    usesExpress: false,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}],
});
runExpressTest({
    command: {update: collName, updates: [{q: {_id: "str", a: 1}, u: {$set: {c: 1}}}]},
    usesExpress: false,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}],
});

// Cannot use express path when a hint is specified.
assert.commandWorked(coll.createIndex({a: 1}));
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: 0}, limit: 0, hint: {a: 1}}]},
    usesExpress: false,
    expectedDocs: [docs[1]],
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: "str"}, update: [{$set: {c: 1}}], hint: {a: 1}},
    usesExpress: false,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}],
});
runExpressTest({
    command: {update: collName, updates: [{q: {_id: "str"}, u: {$set: {c: 1}}, hint: {a: 1}}]},
    usesExpress: false,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}],
});
coll.dropIndexes();

// Can use express path use for delete commands when the predicate is a single equality on _id,
// regardless of the 'limit' value.
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: 0}, limit: 0}]},
    usesExpress: true,
    expectedDocs: [docs[1]],
});
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: 0}, limit: 1}]},
    usesExpress: true,
    expectedDocs: [docs[1]],
});
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: "str"}, limit: 0}]},
    usesExpress: true,
    expectedDocs: [docs[0]],
});
// Same, but with query shape {_id: {$eq: <val>}}.
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: {$eq: 0}}, limit: 0}]},
    usesExpress: true,
    expectedDocs: [docs[1]],
});
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: {$eq: 0}}, limit: 1}]},
    usesExpress: true,
    expectedDocs: [docs[1]],
});
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: {$eq: "str"}}, limit: 0}]},
    usesExpress: true,
    expectedDocs: [docs[0]],
});

// Can use express path use for findAndModify commands when the predicate is a single equality on
// _id. This is true whether the findAndModify represents an update or a delete.
runExpressTest({
    command: {findAndModify: collName, query: {_id: "str"}, update: [{$set: {c: 1}}]},
    usesExpress: true,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}],
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: 0}, update: [{$set: {c: 1}}]},
    usesExpress: true,
    expectedDocs: [{_id: 0, a: 0, c: 1}, docs[1]],
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: 0}, remove: true},
    usesExpress: true,
    expectedDocs: [docs[1]],
});
// Same, but with query shape {_id: {$eq: <val>}}.
runExpressTest({
    command: {findAndModify: collName, query: {_id: {$eq: "str"}}, update: [{$set: {c: 1}}]},
    usesExpress: true,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}],
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: {$eq: 0}}, update: [{$set: {c: 1}}]},
    usesExpress: true,
    expectedDocs: [{_id: 0, a: 0, c: 1}, docs[1]],
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: {$eq: 0}}, remove: true},
    usesExpress: true,
    expectedDocs: [docs[1]],
});

// Can use express path use for update commands when the predicate is a single equality on _id.
runExpressTest({
    command: {update: collName, updates: [{q: {_id: "str"}, u: {$set: {c: 1}}}]},
    usesExpress: true,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}],
});
runExpressTest({
    command: {update: collName, updates: [{q: {_id: 0}, u: {$set: {c: 1}}}]},
    usesExpress: true,
    expectedDocs: [{_id: 0, a: 0, c: 1}, docs[1]],
});
// Same, but with query shape {_id: {$eq: <val>}}.
runExpressTest({
    command: {update: collName, updates: [{q: {_id: {$eq: "str"}}, u: {$set: {c: 1}}}]},
    usesExpress: true,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}],
});
runExpressTest({
    command: {update: collName, updates: [{q: {_id: {$eq: 0}}, u: {$set: {c: 1}}}]},
    usesExpress: true,
    expectedDocs: [{_id: 0, a: 0, c: 1}, docs[1]],
});

docs.push({_id: 2, arr: [99, 100, 101]});
runExpressTest({
    command: {
        update: collName,
        updates: [
            {
                q: {_id: 2},
                u: {$set: {"arr.$[element]": 100}},
                arrayFilters: [{"element": {$gte: 100}}],
            },
        ],
    },
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], {_id: 2, arr: [99, 100, 100]}],
});

// Same, but with query shape {_id: {$eq: <val>}}.
runExpressTest({
    command: {
        update: collName,
        updates: [
            {
                q: {_id: {$eq: 2}},
                u: {$set: {"arr.$[element]": 100}},
                arrayFilters: [{"element": {$gte: 100}}],
            },
        ],
    },
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], {_id: 2, arr: [99, 100, 100]}],
});

// A DBRef is a legal '_id' value, and it is the one common case where the '_id' sub-object of a
// query has a dollar-prefixed first field name. For the {_id: {$eq: <dbref>}} shape the express
// write path unwraps the '$eq' before checking the filter's shape, so it must not then mistake the
// leading '$ref' for a query operator.
const dbref = {$ref: "other", $id: 1};
docs.push({_id: dbref, a: 3});

runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: {$eq: dbref}}, limit: 1}]},
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], docs[2]],
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: {$eq: dbref}}, update: {$set: {c: 1}}},
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], docs[2], {_id: dbref, a: 3, c: 1}],
});
runExpressTest({
    command: {update: collName, updates: [{q: {_id: {$eq: dbref}}, u: {$set: {c: 1}}}]},
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], docs[2], {_id: dbref, a: 3, c: 1}],
});

// The same predicate without the '$eq' wrapper has more than one field in its '_id' sub-object, so
// it is not eligible for the express path, but it must still return the right result.
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: dbref}, limit: 1}]},
    usesExpress: false,
    expectedDocs: [docs[0], docs[1], docs[2]],
});

// An ObjectId is another legal '_id' value
const objId = ObjectId("6799556f337feea725bf9839");
docs.push({_id: objId, a: 3});

runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: {$eq: objId}}, limit: 1}]},
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], docs[2], docs[3]],
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: {$eq: objId}}, update: {$set: {c: 1}}},
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], docs[2], docs[3], {_id: objId, a: 3, c: 1}],
});
runExpressTest({
    command: {update: collName, updates: [{q: {_id: {$eq: objId}}, u: {$set: {c: 1}}}]},
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], docs[2], docs[3], {_id: objId, a: 3, c: 1}],
});
// Should be the same result without the extra $eq
runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: objId}, limit: 1}]},
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], docs[2], docs[3]],
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: objId}, update: {$set: {c: 1}}},
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], docs[2], docs[3], {_id: objId, a: 3, c: 1}],
});
runExpressTest({
    command: {update: collName, updates: [{q: {_id: objId}, u: {$set: {c: 1}}}]},
    usesExpress: true,
    expectedDocs: [docs[0], docs[1], docs[2], docs[3], {_id: objId, a: 3, c: 1}],
});
