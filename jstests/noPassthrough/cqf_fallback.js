/**
 * Verify that expressions and operators are correctly routed to CQF where eligible. This decision
 * is based on several factors including the query text, collection metadata, etc..
 */
import {usedBonsaiOptimizer} from "jstests/libs/optimizer_utils.js";

let conn = MongoRunner.runMongod({setParameter: {featureFlagCommonQueryFramework: true}});
assert.neq(null, conn, "mongod was unable to start up");

let db = conn.getDB("test");
let coll = db[jsTestName()];
coll.drop();

// This test relies on the bonsai optimizer being enabled.
if (assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl == "forceClassicEngine") {
    jsTestLog("Skipping test due to forceClassicEngine");
    MongoRunner.stopMongod(conn);
    quit();
}

assert.commandWorked(
    db.adminCommand({configureFailPoint: 'enableExplainInBonsai', 'mode': 'alwaysOn'}));

// 'runWithParams' as defined in 'optimizer_utils.js' doesn't have access to the db defined in this
// test.
function runWithFastPathsDisabled(fn) {
    try {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalCascadesOptimizerDisableFastPath: true}));
        return fn();
    } finally {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalCascadesOptimizerDisableFastPath: false}));
    }
}

function assertSupportedByBonsaiFully(cmd) {
    // A supported stage must use the new optimizer.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
    const defaultExplain = assert.commandWorked(db.runCommand({explain: cmd}));
    assert(usedBonsaiOptimizer(defaultExplain), tojson(defaultExplain));

    assert.commandWorked(db.runCommand(cmd));
}

function assertSupportedByBonsaiExperimentally(cmd) {
    // Experimental features require the knob to be set to "tryBonsaiExperimental" or higher.
    // With "tryBonsai", these features should not use the new optimizer.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
    const defaultExplain = assert.commandWorked(db.runCommand({explain: cmd}));
    assert(!usedBonsaiOptimizer(defaultExplain), tojson(defaultExplain));

    // Non-explain should also work and use the fallback mechanism, but we cannnot verify exactly
    // this without looking at the logs.
    assert.commandWorked(db.runCommand(cmd));

    // Enable "experimental" features in bonsai and expect the query to use Bonsai and pass.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsaiExperimental"}));
    const explain = assert.commandWorked(db.runCommand({explain: cmd}));
    assert(usedBonsaiOptimizer(explain), tojson(explain));

    // Non-explain should still work.
    assert.commandWorked(db.runCommand(cmd));
}

function assertNotSupportedByBonsai(cmd, testOnly, database = db) {
    // An unsupported stage should not use the new optimizer.
    assert.commandWorked(
        database.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
    const defaultExplain = assert.commandWorked(database.runCommand({explain: cmd}));
    assert(!usedBonsaiOptimizer(defaultExplain), tojson(defaultExplain));

    // Non-explain should also work and use the fallback mechanism, but we cannnot verify exactly
    // this without looking at the logs.
    assert.commandWorked(database.runCommand(cmd));

    // Force the bonsai optimizer and expect the query to either fail if unsupported, or pass if
    // marked as "test only".
    assert.commandWorked(
        database.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
    if (testOnly) {
        const explain = assert.commandWorked(database.runCommand({explain: cmd}));
        assert(usedBonsaiOptimizer(explain), tojson(explain));
    } else {
        assert.commandFailedWithCode(database.runCommand(cmd),
                                     ErrorCodes.InternalErrorNotSupported);
    }

    // Forcing the classic engine should not use Bonsai.
    {
        assert.commandWorked(database.adminCommand(
            {setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
        const explain = assert.commandWorked(database.runCommand({explain: cmd}));
        assert(!usedBonsaiOptimizer(explain), tojson(explain));
        assert.commandWorked(
            database.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
    }
}

// Sanity check we use bonsai for supported cases.
assertSupportedByBonsaiFully({find: coll.getName()});

// Unsupported aggregation stage.
assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [{$sample: {size: 1}}], cursor: {}}, false);

// Test-only aggregation stage.
assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [{$group: {_id: null, a: {$sum: "$b"}}}], cursor: {}},
    true);

// Unsupported match expression.
assertNotSupportedByBonsai({find: coll.getName(), filter: {a: {$mod: [4, 0]}}}, false);
assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [{$match: {a: {$mod: [4, 0]}}}], cursor: {}}, false);
assertNotSupportedByBonsai({find: coll.getName(), filter: {a: {$in: [/^b/, 1]}}}, false);

// Test-only match expression.
assertNotSupportedByBonsai({find: coll.getName(), filter: {$alwaysFalse: 1}}, true);
assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [{$match: {$alwaysFalse: 1}}], cursor: {}}, true);

// Test $match against null. When field paths are dotted, these have testOnly support. When the
// field paths are not dotted, these are fully supported.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {'a': {$eq: null}}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {'a': {$lte: null}}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {'a': {$gt: null}}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [{$match: {a: {$eq: null}}}], cursor: {}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {a: {$in: [1, 2, null, 3]}}})
assertSupportedByBonsaiFully({find: coll.getName(), filter: {a: {$elemMatch: {b: null}}}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {'a.c': {$elemMatch: {b: null}}}});

assertNotSupportedByBonsai({find: coll.getName(), filter: {'a.b': {$eq: null}}}, true);
assertNotSupportedByBonsai({find: coll.getName(), filter: {'a.b': {$lte: null}}}, true);
assertNotSupportedByBonsai({find: coll.getName(), filter: {'a.b': {$gt: null}}}, true);
assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [{$match: {'a.b.c': {$eq: null}}}], cursor: {}}, true);
assertNotSupportedByBonsai({find: coll.getName(), filter: {'a.b': {$in: [1, 2, null, 3]}}}, true);
assertNotSupportedByBonsai({find: coll.getName(), filter: {a: {$elemMatch: {'b.c': null}}}}, true);
assertNotSupportedByBonsai({find: coll.getName(), filter: {'a.c': {$elemMatch: {'b.c': null}}}},
                           true);

// Test $match on _id; these have only experimental support.
assertSupportedByBonsaiExperimentally({find: coll.getName(), filter: {_id: 1}});
assertSupportedByBonsaiExperimentally(
    {aggregate: coll.getName(), pipeline: [{$match: {_id: 1}}], cursor: {}});
assertSupportedByBonsaiExperimentally({find: coll.getName(), filter: {_id: {$lt: 10}}});
assertSupportedByBonsaiExperimentally(
    {aggregate: coll.getName(), pipeline: [{$match: {_id: {$lt: 10}}}], cursor: {}});
assertSupportedByBonsaiExperimentally({find: coll.getName(), filter: {'_id.a': 1}});
assertSupportedByBonsaiExperimentally(
    {aggregate: coll.getName(), pipeline: [{$match: {'_id.a': 1}}], cursor: {}});
assertSupportedByBonsaiExperimentally(
    {find: coll.getName(), filter: {$and: [{a: 10}, {_id: {$gte: 5}}]}});
assertSupportedByBonsaiExperimentally({
    aggregate: coll.getName(),
    pipeline: [{$match: {$and: [{a: 10}, {_id: {$gte: 5}}]}}],
    cursor: {}
});

// Test $project on _id. These are fully supported in bonsai unless the _id index is specifically
// hinted, which is only experimentally supported.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, projection: {_id: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [{$project: {_id: 1}}], cursor: {}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, projection: {_id: 1, a: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [{$project: {_id: 1, a: 1}}], cursor: {}});

assertSupportedByBonsaiExperimentally(
    {find: coll.getName(), filter: {}, projection: {_id: 1}, hint: {_id: 1}});
assertSupportedByBonsaiExperimentally(
    {aggregate: coll.getName(), pipeline: [{$project: {_id: 1}}], cursor: {}, hint: {_id: 1}});
assertSupportedByBonsaiExperimentally(
    {find: coll.getName(), filter: {}, projection: {_id: 1, a: 1}, hint: {_id: 1}});
assertSupportedByBonsaiExperimentally({
    aggregate: coll.getName(),
    pipeline: [{$project: {_id: 1, a: 1}}],
    cursor: {},
    hint: {_id: 1}
});

// $natural hints are fully supported in Bonsai.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: -1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: -1}});

// $natural hints allow running a predicate on _id.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {_id: 1}, hint: {$natural: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [{$match: {_id: 1}}], cursor: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {_id: 1}, hint: {$natural: -1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [{$match: {_id: 1}}], cursor: {}, hint: {$natural: -1}});

// The presence of a $natural hint does not allow an otherwise unsupported query to go through
// Bonsai.
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, sort: {a: 1}, hint: {$natural: 1}},
                           true);
assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [{$sort: {a: 1}}], cursor: {}, hint: {$natural: 1}},
    true);
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, sort: {a: 1}, hint: {$natural: -1}},
                           true);
assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [{$sort: {a: 1}}], cursor: {}, hint: {$natural: -1}},
    true);

// Unsupported projection expression.
assertNotSupportedByBonsai(
    {find: coll.getName(), filter: {}, projection: {a: {$concatArrays: [["$b"], ["suppported"]]}}},
    false);
assertNotSupportedByBonsai({
    aggregate: coll.getName(),
    pipeline: [{$project: {a: {$concatArrays: [["$b"], ["suppported"]]}}}],
    cursor: {}
},
                           false);
assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [{$project: {'a.b': '$c'}}], cursor: {}}, true);
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, projection: {'a.b': '$c'}}, true);

assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [{$addFields: {a: '$z'}}], cursor: {}}, true);

assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [{$project: {a: {$slice: ["$a", 0, 1]}}}], cursor: {}},
    false);
assertNotSupportedByBonsai(
    {find: coll.getName(), filter: {}, projection: {a: {$slice: ["$a", 0, 1]}}}, false);

assertNotSupportedByBonsai(
    {find: coll.getName(), filter: {}, projection: {a: {$concat: ["test", "-only"]}}}, true);
assertNotSupportedByBonsai({
    aggregate: coll.getName(),
    pipeline: [{$project: {a: {$concat: ["test", "-only"]}}}],
    cursor: {}
},
                           true);

// Numeric path components are not supported, either in a match expression or projection.
assertNotSupportedByBonsai({find: coll.getName(), filter: {'a.0': 5}});
assertNotSupportedByBonsai({find: coll.getName(), filter: {'a.0.b': 5}});
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, projection: {'a.0': 1}});
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, projection: {'a.5.c': 0}});

// Positional projection is not supported. Note that this syntax is only possible in the projection
// of a find command.
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, projection: {'a.$': 5}}, true);

// Test for unsupported expressions within a branching expression such as $or.
assertNotSupportedByBonsai({find: coll.getName(), filter: {$or: [{'a.0': 5}, {a: 1}]}});
assertNotSupportedByBonsai({find: coll.getName(), filter: {$or: [{a: 5}, {a: {$mod: [4, 0]}}]}});

// Unsupported command options.

// $_requestResumeToken
assertNotSupportedByBonsai({
    find: coll.getName(),
    filter: {},
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true
},
                           false);
// $_requestReshardingResumeToken
assertNotSupportedByBonsai({
    aggregate: db.getSiblingDB("local").oplog.rs.getName(),
    pipeline: [],
    cursor: {},
    $_requestReshardingResumeToken: true
},
                           false,
                           db.getSiblingDB("local"));
// $_resumeAfter
assertNotSupportedByBonsai({
    find: coll.getName(),
    filter: {},
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true,
    $_resumeAfter: {$recordId: NumberLong("50")},
},
                           false);
// allowPartialResults
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, allowPartialResults: true}, false);
// allowSpeculativeMajorityRead
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, allowSpeculativeMajorityRead: true},
                           false);
// awaitData
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, tailable: true, awaitData: true},
                           false);
// collation
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, collation: {}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, collation: {locale: "simple"}});
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, collation: {locale: "fr_CA"}}, false);
assertNotSupportedByBonsai({
    aggregate: coll.getName(),
    pipeline: [{$match: {$alwaysFalse: 1}}],
    collation: {locale: "fr_CA"},
    cursor: {}
},
                           false);

// When let variables are used in the query, it is experimentally supported.
assertNotSupportedByBonsai({find: coll.getName(), projection: {foo: "$$val"}, let : {val: 1}},
                           true);
assertNotSupportedByBonsai({
    aggregate: coll.getName(),
    pipeline: [{$match: {$expr: {$eq: ["$a", "$$val"]}}}],
    let : {val: 1},
    cursor: {}
},
                           true);

// When let variables are specified but unused in the query, it is eligible for CQF.
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [{$match: {a: 2}}], let : {val: 1}, cursor: {}});

// limit
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, limit: 1}, true);
assertNotSupportedByBonsai({aggregate: coll.getName(), pipeline: [{$limit: 1}], cursor: {}}, true);
// min/max
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, min: {a: 5}}, false);
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, max: {a: 5}}, false);
// noCursorTimeout
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, noCursorTimeout: true}, false);
// readOnce
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, readOnce: true}, false);
// returnKey
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, returnKey: true}, false);
// runtimeConstants
const rtc = {
    localNow: new Date(),
    clusterTime: new Timestamp(0, 0),
};
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, runtimeConstants: rtc}, false);
assertNotSupportedByBonsai(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, runtimeConstants: rtc, fromMongos: true},
    false);
// showRecordId
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, showRecordId: true}, false);
// singleBatch
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, singleBatch: true}, true);
// skip
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, skip: 1}, true);
// sort
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, sort: {a: 1}}, true);
// tailable
assertNotSupportedByBonsai({find: coll.getName(), filter: {}, tailable: true}, false);
// term
(function() {
// Testing the `term` parameter requires a replica set.
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            "featureFlagCommonQueryFramework": true,
            "failpoint.enableExplainInBonsai": tojson({mode: "alwaysOn"}),
        }
    }
});
rst.startSet();
rst.initiate();
const connRS = rst.getPrimary();
const dbRS = connRS.getDB("test");
const collRS = dbRS["termColl"];
collRS.drop();
assert.commandWorked(collRS.insert({a: 1}));
assert.eq(collRS.find().itcount(), 1);
assertNotSupportedByBonsai({find: collRS.getName(), filter: {}, term: NumberLong(1)}, false, dbRS);
rst.stopSet();
})();

// Unsupported index type (sparse).
assert.commandWorked(coll.createIndex({a: 1}, {sparse: true}));
// Fast path implementations assume a collection scan and therefore don't check indexes.
runWithFastPathsDisabled(() => {
    assertNotSupportedByBonsai({find: coll.getName(), filter: {}});
    assertNotSupportedByBonsai({aggregate: coll.getName(), pipeline: [], cursor: {}});
});

// Query with $natural on a collection with a sparse index (unsupported) is eligible for CQF.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: -1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: -1}});

// Unsupported index type (wildcard).
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.createIndex({"$**": 1}));
// Fast path implementations assume a collection scan and therefore don't check indexes.
runWithFastPathsDisabled(() => {
    assertNotSupportedByBonsai({find: coll.getName(), filter: {}});
    assertNotSupportedByBonsai({aggregate: coll.getName(), pipeline: [], cursor: {}});
});

// Query with $natural on a collection with a wildcard index (unsupported) is eligible for CQF.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: -1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: -1}});

// TTL index is not supported.
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}, {expireAfterSeconds: 50}));
// Fast path implementations assume a collection scan and therefore don't check indexes.
runWithFastPathsDisabled(() => {
    assertNotSupportedByBonsai({find: coll.getName(), filter: {}});
    assertNotSupportedByBonsai({aggregate: coll.getName(), pipeline: [], cursor: {}});
});

// Query with $natural on a collection with a TTL index (unsupported) is eligible for CQF.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: -1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: -1}});

// Unsupported index with non-simple collation.
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "fr_CA"}}));
// Fast path implementations assume a collection scan and therefore don't check indexes.
runWithFastPathsDisabled(() => {
    assertNotSupportedByBonsai({find: coll.getName(), filter: {}});
    assertNotSupportedByBonsai({aggregate: coll.getName(), pipeline: [], cursor: {}});
});

// Query with $natural on a collection with a non-simple collation index (unsupported) is eligible
// for CQF.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: -1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: -1}});

// A simple collation on an index should only have experimental support in CQF.
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "simple"}}));
assertSupportedByBonsaiExperimentally({find: coll.getName(), filter: {}});
assertSupportedByBonsaiExperimentally({aggregate: coll.getName(), pipeline: [], cursor: {}});

// Query with $natural on a collection with a simple collation index (experimental) is eligible for
// CQF.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: -1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: -1}});

// A query against a collection with a secondary index should be eligible in the presence of a
// $natural hint.
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: -1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: -1}});

// A query against a collection with a hidden index should be eligible for CQF.
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}, {hidden: true}));
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}});
assertSupportedByBonsaiFully({aggregate: coll.getName(), pipeline: [], cursor: {}});

// Unhiding the index means the query only has experimental support in CQF once again.
coll.unhideIndex({a: 1});
assertSupportedByBonsaiExperimentally({find: coll.getName(), filter: {}});
assertSupportedByBonsaiExperimentally({aggregate: coll.getName(), pipeline: [], cursor: {}});

// A query against a collection with a hidden index should be eligible for CQF even if the
// underlying index is not supported.
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}, {hidden: true, sparse: true}));
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}});
assertSupportedByBonsaiFully({aggregate: coll.getName(), pipeline: [], cursor: {}});

// Unhiding the unsupported index means the query is not eligible for CQF.
coll.unhideIndex({a: 1});
// Fast path implementations assume a collection scan and therefore don't check indexes.
runWithFastPathsDisabled(() => {
    assertNotSupportedByBonsai({find: coll.getName(), filter: {}});
    assertNotSupportedByBonsai({aggregate: coll.getName(), pipeline: [], cursor: {}});
});

// Test-only index type.
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {a: {$gt: 0}}}));
assertNotSupportedByBonsai({find: coll.getName(), filter: {}}, true);
assertNotSupportedByBonsai({aggregate: coll.getName(), pipeline: [], cursor: {}}, true);

// Query with $natural on a collection with a partial index (unsupported) is eligible for CQF.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: -1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: -1}});

// Queries on a collection with a hashed index are supported.
coll.drop();
assert.commandWorked(coll.createIndex({"_id": "hashed"}));
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}});
assertSupportedByBonsaiFully({aggregate: coll.getName(), pipeline: [], cursor: {}});

// Queries on a collection with a hashed index that reference _id only experimentally supported,
// tryBonsai falls back to classic engine. Use batchSize param to avoid triggering EXPRESS path.
assertSupportedByBonsaiExperimentally({find: coll.getName(), filter: {_id: 1}, batchSize: 200},
                                      false);
assertSupportedByBonsaiExperimentally(
    {aggregate: coll.getName(), pipeline: [{$match: {_id: 1}}], cursor: {}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {a: 1}});

// Query with $natural on a collection with a hashed index on _id is only eligible for CQF with a
// $natural hint.
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully({find: coll.getName(), filter: {}, hint: {$natural: -1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: 1}});
assertSupportedByBonsaiFully(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, hint: {$natural: -1}});

// Unsupported collection types. Note that a query against the user-facing timeseries collection
// will fail due to the unsupported $unpackBucket stage.
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "time"}}));
assertNotSupportedByBonsai({find: coll.getName(), filter: {}}, false);
assertNotSupportedByBonsai({aggregate: coll.getName(), pipeline: [], cursor: {}}, false);

const bucketColl = db.getCollection('system.buckets.' + coll.getName());
assertNotSupportedByBonsai({find: bucketColl.getName(), filter: {}}, false);
assertNotSupportedByBonsai({aggregate: bucketColl.getName(), pipeline: [], cursor: {}}, false);

// Collection-default collation is not supported if non-simple.
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
assertNotSupportedByBonsai({find: coll.getName(), filter: {}}, false);
assertNotSupportedByBonsai({aggregate: coll.getName(), pipeline: [], cursor: {}}, false);

// Queries against capped collections are not supported.
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 1000}));
assertNotSupportedByBonsai({find: coll.getName(), filter: {}}, false);
assertNotSupportedByBonsai({aggregate: coll.getName(), pipeline: [], cursor: {}}, false);

// Queries over views are supported as long as the resolved pipeline is valid in CQF.
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(
    db.runCommand({create: "view", viewOn: coll.getName(), pipeline: [{$match: {a: 1}}]}));

// Unsupported expression on top of the view.
assertNotSupportedByBonsai({find: "view", filter: {a: {$mod: [4, 0]}}}, false);

// Supported expression on top of the view.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
assert.commandWorked(db.runCommand({find: "view", filter: {b: 4}}));

// Test-only expression on top of a view.
assertNotSupportedByBonsai({find: "view", filter: {$alwaysFalse: 1}}, true);

// Create a view with an unsupported expression.
assert.commandWorked(db.runCommand(
    {create: "invalidView", viewOn: coll.getName(), pipeline: [{$match: {a: {$mod: [4, 0]}}}]}));

// Any expression, supported or not, should not use CQF over the invalid view.
assertNotSupportedByBonsai({find: "invalidView", filter: {b: 4}}, false);

// Test only expression should also fail.
assertNotSupportedByBonsai({find: "invalidView", filter: {$alwaysFalse: 1}}, true);

// Unsupported commands.
assertNotSupportedByBonsai({count: coll.getName()}, false);
assertNotSupportedByBonsai({delete: coll.getName(), deletes: [{q: {}, limit: 1}]}, false);
assertNotSupportedByBonsai({distinct: coll.getName(), key: "a"}, false);
assertNotSupportedByBonsai({findAndModify: coll.getName(), update: {$inc: {a: 1}}}, false);
assertNotSupportedByBonsai({
    mapReduce: "c",
    map: function() {
        emit(this.a, this._id);
    },
    reduce: function(_key, vals) {
        return Array.sum(vals);
    },
    out: coll.getName()
},
                           false);
assertNotSupportedByBonsai({update: coll.getName(), updates: [{q: {}, u: {$inc: {a: 1}}}]}, false);

// Pipeline with an ineligible stage and an eligible prefix that could be pushed down to the
// find layer should not use Bonsai.
assertNotSupportedByBonsai({
    aggregate: coll.getName(),
    pipeline: [{$match: {a: {$gt: 1}}}, {$bucketAuto: {groupBy: "$a", buckets: 5}}],
    cursor: {}
},
                           false);

// Pipeline with an CQF-eligible sub-pipeline.
// Note: we have to use a failpoint to determine whether we used the CQF codepath because the
// explain output does not have enough information to deduce the query framework for the
// subpipeline.
assert.commandWorked(
    db.adminCommand({configureFailPoint: 'failConstructingBonsaiExecutor', 'mode': 'alwaysOn'}));
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $graphLookup: {
            from: coll.getName(),
            startWith: "$a",
            connectFromField: "a",
            connectToField: "b",
            as: "c"
        }
    }],
    cursor: {},
}));
assert.commandWorked(
    db.adminCommand({configureFailPoint: 'failConstructingBonsaiExecutor', 'mode': 'off'}));

MongoRunner.stopMongod(conn);

// Restart the mongod and verify that we never use the bonsai optimizer if the feature flag is not
// set.

conn = MongoRunner.runMongod({
    setParameter:
        {featureFlagCommonQueryFramework: false, internalQueryFrameworkControl: "trySbeEngine"}
});
assert.neq(null, conn, "mongod was unable to start up");

db = conn.getDB("test");
coll = db[jsTestName()];
coll.drop();

assert.commandWorked(
    db.adminCommand({configureFailPoint: 'enableExplainInBonsai', 'mode': 'alwaysOn'}));

const supportedExpression = {
    a: {$eq: 4}
};

let explain = coll.explain().find(supportedExpression).finish();
assert(!usedBonsaiOptimizer(explain), tojson(explain));

explain = coll.explain().aggregate([{$match: supportedExpression}]);
assert(!usedBonsaiOptimizer(explain), tojson(explain));

// Show that trying to set the framework to tryBonsai is not permitted when the feature flag is off,
// but tryBonsaiExperimental and forceBonsai are allowed (since test commands are enabled here by
// default).
assert.commandFailed(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
explain = coll.explain().find(supportedExpression).finish();
assert(!usedBonsaiOptimizer(explain), tojson(explain));

assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsaiExperimental"}));
explain = coll.explain().find(supportedExpression).finish();
assert(usedBonsaiOptimizer(explain), tojson(explain));

assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
explain = coll.explain().find(supportedExpression).finish();
assert(usedBonsaiOptimizer(explain), tojson(explain));

MongoRunner.stopMongod(conn);

// Show that we can't start a mongod with the framework control set to tryBonsaiExperimental when
// test commands are off.
TestData.enableTestCommands = false;
try {
    conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagCommonQueryFramework: false,
            internalQueryFrameworkControl: "tryBonsaiExperimental"
        }
    });
    MongoRunner.stopMongod(conn);
    assert(false, "MongoD was able to start up when it should have failed");
} catch (e) {
    // This is expected.
    assert.eq(e.returnCode,
              ErrorCodes.BadValue,
              "Expected a BadValue error, but encountered: " + e.message);
}

// Show that we can't start a mongod with the framework control set to tryBonsai
// when the feature flag is off.
TestData.enableTestCommands = true;
try {
    conn = MongoRunner.runMongod({
        setParameter:
            {featureFlagCommonQueryFramework: false, internalQueryFrameworkControl: "tryBonsai"}
    });
    MongoRunner.stopMongod(conn);
    assert(false, "MongoD was able to start up when it should have failed");
} catch (e) {
    // This is expected.
    assert.eq(e.returnCode,
              ErrorCodes.BadValue,
              "Expected a BadValue error, but encountered: " + e.message);
}

// Show that finds and aggregations against sharded collections are eligible for bonsai.
let shardingConn = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        shardOptions: {
            setParameter: {
                "failpoint.enableExplainInBonsai": tojson({mode: "alwaysOn"}),
                featureFlagCommonQueryFramework: true,
                internalQueryFrameworkControl: 'tryBonsai'
            }
        },
        mongosOptions: {
            setParameter: {
                featureFlagCommonQueryFramework: true,
            }
        },
    }
});

db = shardingConn.getDB("test");
coll = db[jsTestName()];
coll.drop();

coll.insertMany([...Array(100).keys()].map(i => {
    return {_id: i, a: i};
}));
shardingConn.shardColl(coll.getName(), {_id: 1}, {_id: 50}, {_id: 51});

explain = coll.explain().aggregate({$match: {a: {$gt: 12}}});
assert(usedBonsaiOptimizer(explain), tojson(explain));

explain = coll.explain().find({a: {$gt: 12}}).finish();
assert(usedBonsaiOptimizer(explain), tojson(explain));

shardingConn.stop();
