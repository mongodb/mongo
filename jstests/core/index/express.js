/**
 * Tests the express code path, which bypasses regular query planning and execution. Verifies some
 * basic eligibility restrictions such as match expression shape and index options, and checks
 * the query results.
 * @tags: [
 *   requires_fcv_80,
 *   # "Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results"
 *   does_not_support_stepdowns,
 *   # "Explain for the aggregate command cannot run within a multi-document transaction"
 *   does_not_support_transactions,
 *   # This test uses QuerySettingsUtils to set query settings on a namespace. The utils do not
 *   # support injecting tenant ID, which is required by certain passthrough suites.
 *   simulate_atlas_proxy_incompatible,
 *   # Setting query settings directly against shardsvrs is not allowed.
 *   directly_against_shardsvrs_incompatible,
 *   # setParameter not permitted with security tokens
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getPlanStage, getWinningPlanFromExplain, isExpress} from "jstests/libs/query/analyze_plan.js";
import {runExpressTest} from "jstests/libs/query/express_utils.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const coll = db.getCollection("express_coll");

function runWithParamsAllNodes(db, keyValPairs, fn) {
    let prevVals = [];

    try {
        for (let i = 0; i < keyValPairs.length; i++) {
            const flag = keyValPairs[i].key;
            const valIn = keyValPairs[i].value;
            const val = typeof valIn === "object" ? JSON.stringify(valIn) : valIn;

            let getParamObj = {};
            getParamObj["getParameter"] = 1;
            getParamObj[flag] = 1;
            const prevVal = db.adminCommand(getParamObj);
            prevVals.push(prevVal[flag]);

            setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), flag, val);
        }

        return fn();
    } finally {
        for (let i = 0; i < keyValPairs.length; i++) {
            const flag = keyValPairs[i].key;

            setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), flag, prevVals[i]);
        }
    }
}

const docs = [
    {_id: 0, a: 0, b: 0},
    {_id: 1, a: "string"},
    {_id: 2, a: {bar: 1}},
    {_id: 3, a: null},
    {_id: 4, a: [1, 2, 3]},
];
let isShardedColl = false;
function recreateCollWith(documents) {
    coll.drop();
    assert.commandWorked(coll.insert(documents));
    isShardedColl = FixtureHelpers.isSharded(coll);
}
recreateCollWith(docs);

// Cannot use express path when no indexes exist.
runExpressTest({coll, filter: {a: 1}, limit: 1, result: [{_id: 4, a: [1, 2, 3]}], usesExpress: false});

// Cannot use express path when predicate is not a single equality.
assert.commandWorked(coll.createIndex({a: 1}));
runExpressTest({coll, filter: {a: {$lte: -1}}, limit: 1, result: [], usesExpress: false});
runExpressTest({coll, filter: {a: 0, b: 0}, limit: 1, result: [{_id: 0, a: 0, b: 0}], usesExpress: false});

// Cannot use express path when the query field is contained in the index, but it is not a prefix.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
runExpressTest({coll, filter: {b: 0}, limit: 1, result: [{_id: 0, a: 0, b: 0}], usesExpress: false});

// Cannot use express path when the index is not a regular B-tree index. Here we drop the collection
// since hashed indexes don't support array values.
recreateCollWith([{_id: "hashed", a: 0}]);
assert.commandWorked(coll.createIndex({a: "hashed"}));
runExpressTest({coll, filter: {a: 0}, limit: 1, result: [{_id: "hashed", a: 0}], usesExpress: false});
coll.dropIndexes();
assert.commandWorked(coll.createIndex({"$**": 1}));
runExpressTest({coll, filter: {a: 0}, limit: 1, result: [{_id: "hashed", a: 0}], usesExpress: false});

// Cannot use express path when a hint is specified.
recreateCollWith(docs);
coll.dropIndexes();
coll.createIndex({a: 1, b: 1, c: 1});
let explain = coll.find({a: 1}).limit(1).hint({a: 1, b: 1, c: 1}).explain();
assert(!isExpress(db, explain), tojson(explain));

// Single-field equality with limit 1 can take advantage of express path with single field index.
// The index can be ascending, descending, and/or compound (if the filter is on the prefix field).
for (let index of [{a: 1}, {a: -1}, {a: 1, b: 1}, {a: 1, b: -1}, {a: -1, b: 1}, {a: -1, b: -1}]) {
    coll.dropIndexes();
    assert.commandWorked(coll.createIndex(index));
    runExpressTest({coll, filter: {a: 10}, limit: 1, result: [], usesExpress: !isShardedColl});
    runExpressTest({
        coll,
        filter: {a: 1},
        limit: 1,
        result: [{_id: 4, a: [1, 2, 3]}],
        usesExpress: !isShardedColl,
    });
}

// When the index is not dotted, queries against nested fields do not use express unless they look
// for an exact match.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1}));
runExpressTest({coll, filter: {"a.b": 0}, limit: 1, result: [], usesExpress: false});
runExpressTest({
    coll,
    filter: {"a": {bar: 1}},
    limit: 1,
    result: [{_id: 2, a: {bar: 1}}],
    usesExpress: !isShardedColl,
});

// When the index is dotted, queries against the dotted field can use the express path.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({"a.bar": 1}));
runExpressTest({coll, filter: {"a.bar": 10}, limit: 1, result: [], usesExpress: !isShardedColl});
runExpressTest({
    coll,
    filter: {"a.bar": 1},
    limit: 1,
    result: [{_id: 2, a: {bar: 1}}],
    usesExpress: !isShardedColl,
});
runExpressTest({coll, filter: {"a.bar.c": 10}, limit: 1, result: [], usesExpress: false});
runExpressTest({coll, filter: {"a": 10}, limit: 1, result: [], usesExpress: false});

if (!isShardedColl) {
    // Single-field equality with a unique index on that field can take advantage of express path
    // with single field index, without a limit 1.
    coll.dropIndexes();
    assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
    runExpressTest({coll, filter: {a: 10}, result: [], usesExpress: true});
    runExpressTest({coll, filter: {a: 1}, result: [{_id: 4, a: [1, 2, 3]}], usesExpress: true});

    // It is not eligible for the express path if it's a unique, compound index.
    coll.dropIndexes();
    assert.commandWorked(coll.createIndex({a: 1, b: 1}, {unique: true}));
    runExpressTest({coll, filter: {a: 10}, result: [], usesExpress: false});
}

// Special case of above, which works in the sharded case: _id equality can take advantage of
// express path in shorthand and expanded form.
runExpressTest({coll, filter: {_id: -1}, result: [], usesExpress: true});
runExpressTest({coll, filter: {_id: 1}, result: [{_id: 1, a: "string"}], usesExpress: true});
runExpressTest({coll, filter: {_id: {$eq: 1}}, result: [{_id: 1, a: "string"}], usesExpress: true});

// When the query simplifies to a single-field equality, it can use the express path.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1}));
runExpressTest({
    coll,
    filter: {$or: [{a: 0}, {a: 0}]},
    limit: 1,
    result: [{_id: 0, a: 0, b: 0}],
    usesExpress: !isShardedColl,
});

// Partial/sparse indexes are eligible to use the express path if the query matches the partial
// filter.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {a: {$lt: 1}}}));
runExpressTest({coll, filter: {a: -1}, limit: 1, result: [], usesExpress: !isShardedColl});
runExpressTest({coll, filter: {a: 0}, limit: 1, result: [{_id: 0, a: 0, b: 0}], usesExpress: !isShardedColl});
runExpressTest({coll, filter: {a: 1}, limit: 1, result: [{_id: 4, a: [1, 2, 3]}], usesExpress: false});

coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1}, {sparse: true}));
runExpressTest({coll, filter: {a: -1}, limit: 1, result: [], usesExpress: !isShardedColl});
runExpressTest({coll, filter: {a: 0}, limit: 1, result: [{_id: 0, a: 0, b: 0}], usesExpress: !isShardedColl});
runExpressTest({coll, filter: {a: null}, limit: 1, result: [{_id: 3, a: null}], usesExpress: false});

// Indexes with collation that differs from the collection collation are elgible for use in the
// express path if query collation matches the index collation.
const caseInsensitive = {
    locale: "en_US",
    strength: 2,
};
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1}, {collation: caseInsensitive}));
runExpressTest({
    coll,
    filter: {a: "STRING"},
    limit: 1,
    collation: caseInsensitive,
    result: [{_id: 1, a: "string"}],
    usesExpress: !isShardedColl,
});
runExpressTest({
    coll,
    filter: {a: "noMatchString"},
    limit: 1,
    collation: caseInsensitive,
    result: [],
    usesExpress: !isShardedColl,
});
runExpressTest({coll, filter: {a: "STRING"}, limit: 1, result: [], usesExpress: false});

// Same as above, but with a dotted path.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({"a.b": 1}, {collation: caseInsensitive}));
runExpressTest({
    coll,
    filter: {"a.b": 1},
    limit: 1,
    result: [],
    collation: caseInsensitive,
    usesExpress: !isShardedColl,
});

// If there is more than one eligible index, we choose the shortest one.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1}));

explain = coll.find({a: 1}).limit(1).explain();
if (isShardedColl) {
    assert(!isExpress(db, explain), tojson(explain));
} else {
    assert(isExpress(db, explain), tojson(explain));
    let express = getPlanStage(getWinningPlanFromExplain(explain), "EXPRESS_IXSCAN");
    assert(express && express.indexName == "a_1_b_1", tojson(explain));
}

// We respect query settings such as allowedIndexes when they are specified.
// TODO SERVER-87016: change this if statement to allow sharded collections.
if (!isShardedColl && !FixtureHelpers.isStandalone(db)) {
    jsTestLog("Running query settings test");

    const qsutils = new QuerySettingsUtils(db, coll.getName());
    const query = qsutils.makeFindQueryInstance({filter: {a: 1}, limit: 1});

    // The express path will only choose an index allowed by the query settings for the query.
    const allowedIndex = {
        indexHints: {ns: {db: db.getName(), coll: coll.getName()}, allowedIndexes: ["a_1_b_1_c_1"]},
    };
    qsutils.withQuerySettings(query, allowedIndex, () => {
        explain = assert.commandWorked(db.runCommand({explain: {find: coll.getName(), filter: {a: 1}, limit: 1}}));
        assert(isExpress(db, explain), tojson(explain));
        let express = getPlanStage(getWinningPlanFromExplain(explain), "EXPRESS_IXSCAN");
        assert(express && express.indexName == "a_1_b_1_c_1", tojson(explain));
    });

    // The same query as above (eligible for express) will fail if the query settings reject it.
    qsutils.withQuerySettings(query, {reject: true}, () => {
        assert.commandFailedWithCode(
            db.runCommand({find: coll.getName(), filter: {a: 1}, limit: 1}),
            ErrorCodes.QueryRejectedBySettings,
        );
    });

    // If a framework control is set in query settings, we will not use the express path.
    qsutils.withQuerySettings(query, {queryFramework: "classic"}, () => {
        explain = assert.commandWorked(db.runCommand({explain: {find: coll.getName(), filter: {a: 1}, limit: 1}}));
        assert(!isExpress(db, explain), tojson(explain));
    });
} else {
    jsTestLog(
        "Skipping query settings test because the collection is sharded, we are running against" +
            " a standalone, or query settings is not enabled",
    );
}

// Aggregations that are pushed down to find are eligible for the express path.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1}));
explain = coll.explain().aggregate([{$match: {a: 1}}, {$limit: 1}]);
if (isShardedColl) {
    assert(!isExpress(db, explain), tojson(explain));
} else {
    assert(isExpress(db, explain), tojson(explain));
}

// Sharded $lookup is not allowed in multi-doc transaction.
// The express path may be used on the inner side of a $lookup, but it's hard to assert
// on the query plans chosen for the inner side. Here we just assert on the result set.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1}));
let res = coll
    .aggregate([
        {$match: {a: 0}},
        {
            $lookup: {
                from: coll.getName(),
                as: "res",
                localField: "a",
                foreignField: "a",
                pipeline: [{$limit: 1}],
            },
        },
    ])
    .toArray();
assert.eq(res, [{_id: 0, a: 0, b: 0, res: [{_id: 0, a: 0, b: 0}]}]);

// Demonstrate use of internalQueryDisableSingleFieldExpressExecutor.
recreateCollWith(docs);
coll.createIndex({a: 1});
runWithParamsAllNodes(db, [{key: "internalQueryDisableSingleFieldExpressExecutor", value: false}], () => {
    runExpressTest({coll, filter: {a: 10}, limit: 1, result: [], usesExpress: !isShardedColl});
    runExpressTest({coll, filter: {_id: 10}, limit: 1, result: [], usesExpress: true});
});
runWithParamsAllNodes(db, [{key: "internalQueryDisableSingleFieldExpressExecutor", value: true}], () => {
    runExpressTest({coll, filter: {a: 10}, limit: 1, result: [], usesExpress: false});
    runExpressTest({coll, filter: {_id: 10}, limit: 1, result: [], usesExpress: true});
});
