/**
 * Test creating and using partial indexes, on a time-series collection.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # During fcv upgrade/downgrade the index created might not be what we expect.
 * ]
 */
import {getTimeseriesCollForRawOps, kRawOperationSpec} from "jstests/core/libs/raw_operation_utils.js";
import {isShardedTimeseries} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getPlanStage, getPlanStages, getRejectedPlan, getRejectedPlans} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
const timeField = "time";
const metaField = "m";
let extraBucketIndexes = [];

function addCollation(baseSpec, collation) {
    return collation ? {...baseSpec, collation: collation} : baseSpec;
}

function resetCollection(collation) {
    coll.drop();
    extraBucketIndexes = [];

    const createTimeseriesSpec = {
        timeseries: {timeField, metaField},
    };
    assert.commandWorked(db.createCollection(coll.getName(), addCollation(createTimeseriesSpec, collation)));

    // If the collection is sharded, expect an implicitly-created index on time. It will appear
    // differently in listIndexes depending on whether you look at the user-visible index or the
    // raw index over the buckets.
    if (isShardedTimeseries(coll)) {
        const extraBucketIndexesShardedSpec = {
            "v": 2,
            "key": {"control.min.time": 1, "control.max.time": 1},
            "name": "time_1",
        };
        extraBucketIndexes.push(addCollation(extraBucketIndexesShardedSpec, collation));
    }

    // An index on {metaField, timeField} gets built by default on time-series collections.
    const extraBucketIndexesSpec = {
        "v": 2,
        "key": {"meta": 1, "control.min.time": 1, "control.max.time": 1},
        "name": "m_1_time_1",
    };
    extraBucketIndexes.push(addCollation(extraBucketIndexesSpec, collation));
}

resetCollection();

// Check that there is no collation in the default index.
assert.eq(coll.getIndexes()[0].collation, null);
assert.sameMembers(getTimeseriesCollForRawOps(coll).getIndexes(kRawOperationSpec), extraBucketIndexes);

assert.commandWorked(
    coll.insert([
        // In bucket A, some but not all documents match the partial filter.
        {[timeField]: ISODate("2000-01-01T00:00:00Z"), [metaField]: {bucket: "A"}, a: 0, b: 20},
        {[timeField]: ISODate("2000-01-01T00:00:01Z"), [metaField]: {bucket: "A"}, a: 1, b: 16},
        {[timeField]: ISODate("2000-01-01T00:00:02Z"), [metaField]: {bucket: "A"}, a: 2, b: 12},
        {[timeField]: ISODate("2000-01-01T00:00:03Z"), [metaField]: {bucket: "A"}, a: 3, b: 8},
        {[timeField]: ISODate("2000-01-01T00:00:04Z"), [metaField]: {bucket: "A"}, a: 4, b: 4},

        // In bucket B, no documents match the partial filter.
        {[timeField]: ISODate("2000-01-01T00:00:00Z"), [metaField]: {bucket: "B"}, a: 5, b: 99},
        {[timeField]: ISODate("2000-01-01T00:00:01Z"), [metaField]: {bucket: "B"}, a: 6, b: 99},
        {[timeField]: ISODate("2000-01-01T00:00:02Z"), [metaField]: {bucket: "B"}, a: 7, b: 99},
        {[timeField]: ISODate("2000-01-01T00:00:03Z"), [metaField]: {bucket: "B"}, a: 8, b: 99},
        {[timeField]: ISODate("2000-01-01T00:00:04Z"), [metaField]: {bucket: "B"}, a: 9, b: 99},

        // In bucket C, every document matches the partial filter.
        {[timeField]: ISODate("2000-01-01T00:00:00Z"), [metaField]: {bucket: "C"}, a: 10, b: 0},
        {[timeField]: ISODate("2000-01-01T00:00:01Z"), [metaField]: {bucket: "C"}, a: 11, b: 0},
        {[timeField]: ISODate("2000-01-01T00:00:02Z"), [metaField]: {bucket: "C"}, a: 12, b: 0},
        {[timeField]: ISODate("2000-01-01T00:00:03Z"), [metaField]: {bucket: "C"}, a: 13, b: 0},
        {[timeField]: ISODate("2000-01-01T00:00:04Z"), [metaField]: {bucket: "C"}, a: 14, b: 0},
    ]),
);
assert.eq(15, coll.count());
assert.eq(3, getTimeseriesCollForRawOps(coll).count({}, kRawOperationSpec));

// Expected partialFilterExpression to be an object.
assert.commandFailedWithCode(coll.createIndex({a: 1}, {partialFilterExpression: 123}), [10065]);

// Test creating and using a partial index.
{
    let ixscanInWinningPlan = 0;

    // Make sure the {a: 1} index was considered for this query.
    function checkPlan(predicate) {
        const explain = coll.find(predicate).explain();
        let scan = getPlanStage(explain, "IXSCAN");
        // If scan is not present, check rejected plans
        if (scan === null) {
            const rejectedPlans = getRejectedPlans(explain);
            if (rejectedPlans.length === 2) {
                let firstScan = getPlanStages(getRejectedPlan(rejectedPlans[0]), "IXSCAN");
                let secondScan = getPlanStages(getRejectedPlan(rejectedPlans[1]), "IXSCAN");
                // Both plans should have an "IXSCAN" stage and one stage should scan the index on
                // the 'a' field.
                if (firstScan.length === 1 && secondScan.length === 1) {
                    scan = firstScan[0];
                    if (secondScan[0]["indexName"] == "a_1") {
                        scan = secondScan[0];
                    }
                }
            }
        } else {
            ixscanInWinningPlan++;
        }
        const indexes = getTimeseriesCollForRawOps(coll).getIndexes(kRawOperationSpec);
        assert(
            scan,
            "Expected an index scan for predicate: " +
                tojson(predicate) +
                " but got: " +
                tojson(explain) +
                "\nAvailable indexes were: " +
                tojson(indexes),
        );
        assert.eq(scan.indexName, "a_1", scan);
    }
    // Make sure the query results match a collection-scan plan.
    function checkResults(predicate) {
        const result = coll.aggregate([{$match: predicate}], {hint: {a: 1}}).toArray();
        const unindexed = coll.aggregate([{$_internalInhibitOptimization: {}}, {$match: predicate}]).toArray();
        assert.sameMembers(result, unindexed);
    }
    function checkPlanAndResults(predicate) {
        checkPlan(predicate);
        checkResults(predicate);
    }
    const check = checkPlanAndResults;

    // Test some predicates on a metric field.
    {
        assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {b: {$lt: 12}}}));
        // Query predicate mentions partialFilterExpression exactly.
        // The extra predicate on 'a' is necessary for the multiplanner to think an {a: 1} index is
        // relevant.
        check({a: {$lt: 999}, b: {$lt: 12}});
        // Query predicate is a subset of partialFilterExpression.
        check({a: {$lt: 999}, b: {$lt: 11}});

        assert.commandWorked(coll.dropIndex({a: 1}));
        assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {b: {$lte: 12}}}));
        check({a: {$lt: 999}, b: {$lte: 12}});
        check({a: {$lt: 999}, b: {$lte: 11}});

        assert.commandWorked(coll.dropIndex({a: 1}));
        assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {b: {$gt: 12}}}));
        check({a: {$lt: 999}, b: {$gt: 12}});
        check({a: {$lt: 999}, b: {$gt: 13}});

        assert.commandWorked(coll.dropIndex({a: 1}));
        assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {b: {$gte: 12}}}));
        check({a: {$lt: 999}, b: {$gte: 12}});
        check({a: {$lt: 999}, b: {$gte: 13}});
    }

    // Test some predicates on the time field.
    {
        // Note on implicitly sharded collections this index is already made and this operation is a
        // no-op.
        assert.commandWorked(coll.createIndex({[timeField]: 1}));

        const t0 = ISODate("2000-01-01T00:00:00Z");
        const t1 = ISODate("2000-01-01T00:00:01Z");
        const t2 = ISODate("2000-01-01T00:00:02Z");

        assert.commandWorked(coll.dropIndex({a: 1}));
        assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {[timeField]: {$lt: t1}}}));
        check({a: {$lt: 999}, [timeField]: {$lt: t1}});
        check({a: {$lt: 999}, [timeField]: {$lt: t0}});

        assert.commandWorked(coll.dropIndex({a: 1}));
        assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {[timeField]: {$lte: t1}}}));
        check({a: {$lt: 999}, [timeField]: {$lte: t1}});
        check({a: {$lt: 999}, [timeField]: {$lte: t0}});

        assert.commandWorked(coll.dropIndex({a: 1}));
        assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {[timeField]: {$gt: t1}}}));
        check({a: {$lt: 999}, [timeField]: {$gt: t1}});
        check({a: {$lt: 999}, [timeField]: {$gt: t2}});

        assert.commandWorked(coll.dropIndex({a: 1}));
        assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {[timeField]: {$gte: t1}}}));
        check({a: {$lt: 999}, [timeField]: {$gte: t1}});
        check({a: {$lt: 999}, [timeField]: {$gte: t2}});

        // Drop the index, so it doesn't interfere with other tests.
        if (!isShardedTimeseries(coll)) {
            assert.commandWorked(coll.dropIndex({[timeField]: 1}));
        }
    }

    assert.commandWorked(coll.dropIndex({a: 1}));
    // Check that there is no collation in the default index.
    assert.eq(coll.getIndexes()[0].collation, null);
    assert.sameMembers(getTimeseriesCollForRawOps(coll).getIndexes(kRawOperationSpec), extraBucketIndexes);
    assert.gt(ixscanInWinningPlan, 0);
}

// Check that partialFilterExpression can use a mixture of metadata, time, and measurement fields,
// and that this is translated to the bucket-level predicate we expect.
assert.commandWorked(
    coll.createIndex(
        {a: 1},
        {
            partialFilterExpression: {
                $and: [{time: {$gt: ISODate("2000-01-01")}}, {[metaField + ".bucket"]: {$gte: "B"}}, {b: {$gte: 0}}],
            },
        },
    ),
);
const actualBucketIndexes = getTimeseriesCollForRawOps(coll).getIndexes(kRawOperationSpec);
assert.sameMembers(
    actualBucketIndexes,
    extraBucketIndexes.concat([
        {
            "v": 2,
            "key": {"control.min.a": 1, "control.max.a": 1},
            "name": "a_1",

            "partialFilterExpression": {
                // Meta predicates are pushed down verbatim.
                ["meta.bucket"]: {"$gte": "B"},
                $and: [
                    {
                        $and: [
                            // $gt on time creates a bound on the max time.
                            {"control.max.time": {"$_internalExprGt": ISODate("2000-01-01T00:00:00Z")}},
                            // We also have a bound on the min time, derived from bucketMaxSpanSeconds.
                            {"control.min.time": {"$_internalExprGt": ISODate("1999-12-31T23:00:00Z")}},
                        ],
                    },
                    // $gt on a non-time field can only bound the control.max for that field.
                    {"control.max.b": {"$_internalExprGte": 0}},
                ],
            },
            "originalSpec": {
                key: {a: 1},
                name: "a_1",
                partialFilterExpression: {
                    $and: [
                        {time: {$gt: ISODate("2000-01-01")}},
                        {[metaField + ".bucket"]: {$gte: "B"}},
                        {b: {$gte: 0}},
                    ],
                },
                v: 2,
            },
        },
    ]),
);

// Test how partialFilterExpression interacts with collation.
{
    // Recreate the collection with a collation.
    const numericCollation = {locale: "en_US", numericOrdering: true};
    resetCollection(numericCollation);

    assert.commandWorked(
        coll.insert([
            {[timeField]: ISODate(), [metaField]: {x: "1000", y: 1}, a: "120"},
            {[timeField]: ISODate(), [metaField]: {x: "1000", y: 2}, a: "3"},
            {[timeField]: ISODate(), [metaField]: {x: "500", y: 3}, a: "120"},
            {[timeField]: ISODate(), [metaField]: {x: "500", y: 4}, a: "3"},
        ]),
    );

    // Queries on the collection use the collection's collation by default.
    assert.docEq(
        [{[metaField]: {x: "500"}}, {[metaField]: {x: "500"}}, {[metaField]: {x: "1000"}}, {[metaField]: {x: "1000"}}],
        coll
            .find({}, {_id: 0, [metaField + ".x"]: 1})
            .sort({[metaField + ".x"]: 1})
            .toArray(),
    );
    assert.docEq([{a: "3"}, {a: "3"}, {a: "120"}, {a: "120"}], coll.find({}, {_id: 0, a: 1}).sort({a: 1}).toArray());

    // Specifying a collation and partialFilterExpression together fails, even if the collation
    // matches the collection's default collation.
    assert.commandFailedWithCode(
        coll.createIndex(
            {a: 1},
            {
                name: "a_lt_25_simple",
                collation: {locale: "simple"},
                partialFilterExpression: {a: {$lt: "25"}},
            },
        ),
        [ErrorCodes.IndexOptionsConflict],
    );
    assert.commandFailedWithCode(
        coll.createIndex(
            {a: 1},
            {
                name: "a_lt_25_numeric",
                collation: numericCollation,
                partialFilterExpression: {a: {$lt: "25"}},
            },
        ),
        // The default collation is also numeric, so this index is equivalent to the previous.
        [ErrorCodes.IndexOptionsConflict],
    );

    assert.commandWorked(
        coll.createIndex({a: 1}, {name: "a_lt_25_default", partialFilterExpression: {a: {$lt: "25"}}}),
    );

    // Verify that the index contains what we expect.
    assert.docEq([{a: "3"}, {a: "3"}], coll.find({}, {_id: 0, a: 1}).hint("a_lt_25_default").toArray());

    // Verify that the index is used when possible.
    function checkPlanAndResult({predicate, collation, stageName, indexName, expectedResults}) {
        let cur = coll.find(predicate, {_id: 0, a: 1});
        if (collation) {
            cur.collation(collation);
        }

        const plan = cur.explain();
        const stage = getPlanStage(plan, stageName);
        assert(stage, "Expected a " + stageName + " stage: " + tojson(plan));
        if (indexName) {
            assert.eq(stage.indexName, indexName, stage);
        }

        const results = cur.toArray();
        assert.docEq(expectedResults, results);
    }

    // a < "25" can use the index, since the collations match.
    checkPlanAndResult({
        predicate: {a: {$lt: "25"}},
        collation: null,
        stageName: "IXSCAN",
        indexName: "a_lt_25_default",
        expectedResults: [{a: "3"}, {a: "3"}],
    });
    checkPlanAndResult({
        predicate: {a: {$lt: "25"}},
        collation: numericCollation,
        stageName: "IXSCAN",
        indexName: "a_lt_25_default",
        expectedResults: [{a: "3"}, {a: "3"}],
    });

    // Likewise a < "24" can use the index.
    checkPlanAndResult({
        predicate: {a: {$lt: "24"}},
        collation: null,
        stageName: "IXSCAN",
        indexName: "a_lt_25_default",
        expectedResults: [{a: "3"}, {a: "3"}],
    });
    checkPlanAndResult({
        predicate: {a: {$lt: "24"}},
        collation: numericCollation,
        stageName: "IXSCAN",
        indexName: "a_lt_25_default",
        expectedResults: [{a: "3"}, {a: "3"}],
    });

    // a < "30" can't use the index; it's not a subset.
    checkPlanAndResult({
        predicate: {a: {$lt: "30"}},
        collation: null,
        stageName: "COLLSCAN",
        indexName: null,
        expectedResults: [{a: "3"}, {a: "3"}],
    });
    checkPlanAndResult({
        predicate: {a: {$lt: "30"}},
        collation: numericCollation,
        stageName: "COLLSCAN",
        indexName: null,
        expectedResults: [{a: "3"}, {a: "3"}],
    });

    // a < "100" also can't use the index, because according to the numeric collation, "20" < "100".
    // Note that if we were using a simple collation we'd get the opposite outcome: "100" < "20",
    // because it would compare strings lexicographically instead of numerically.
    checkPlanAndResult({
        predicate: {a: {$lt: "100"}},
        collation: null,
        stageName: "COLLSCAN",
        indexName: null,
        expectedResults: [{a: "3"}, {a: "3"}],
    });
    checkPlanAndResult({
        predicate: {a: {$lt: "100"}},
        collation: numericCollation,
        stageName: "COLLSCAN",
        indexName: null,
        expectedResults: [{a: "3"}, {a: "3"}],
    });
}

// Test which types of predicates are allowed, and test that the bucket-level
// partialFilterExpression is what we expect.
{
    assert.commandWorked(coll.dropIndex({a: 1}));
    // Check that there is collation in the default index (and that it matches the default
    // collation).
    assert.eq(coll.getIndexes()[0].collation.locale, "en_US");
    assert.eq(coll.getIndexes()[0].collation.numericOrdering, true);
    function checkPredicateDisallowed(predicate) {
        assert.commandFailedWithCode(coll.createIndex({a: 1}, {partialFilterExpression: predicate}), [5916301]);
    }
    function checkPredicateOK({input: predicate, output: expectedBucketPredicate}) {
        const name = "example_pushdown_index";
        assert.commandWorked(coll.createIndex({a: 1}, {name, partialFilterExpression: predicate}));
        const indexes = getTimeseriesCollForRawOps(coll)
            .getIndexes(kRawOperationSpec)
            .filter((ix) => ix.name === name);
        assert.eq(1, indexes.length, "Expected 1 index but got " + tojson(indexes));
        const actualBucketPredicate = indexes[0].partialFilterExpression;
        assert.eq(
            actualBucketPredicate,
            expectedBucketPredicate,
            "Expected the bucket-level predicate to be " +
                tojson(expectedBucketPredicate) +
                " but it was " +
                tojson(actualBucketPredicate),
        );
        assert.commandWorked(coll.dropIndex(name));
    }
    // A trivial, empty predicate is fine.
    // It doesn't seem useful but it's allowed on a normal collection.
    checkPredicateOK({
        input: {},
        output: {},
    });
    // Comparison with non-scalar.
    checkPredicateDisallowed({a: {}});
    checkPredicateDisallowed({a: {b: 3}});
    checkPredicateDisallowed({a: []});
    checkPredicateDisallowed({a: [1]});
    checkPredicateDisallowed({a: [1, 2]});
    // Always-false predicate on time (which is always a Date).
    checkPredicateDisallowed({time: 7});
    checkPredicateDisallowed({time: "abc"});

    // Scalar $eq is equivalent to a conjunction of $lte and $gte.
    checkPredicateOK({
        input: {a: 5},
        output: {
            $and: [{"control.min.a": {$_internalExprLte: 5}}, {"control.max.a": {$_internalExprGte: 5}}],
        },
    });

    // Comparisons with null/missing are not implemented. These would be slightly more complicated
    // because {$eq: null} actually means "null, or missing, or undefined",
    // while {$_internalExprEq: null} only matches null.
    checkPredicateDisallowed({a: null});
    checkPredicateDisallowed({a: {$eq: null}});
    checkPredicateDisallowed({a: {$ne: null}});

    // Regex queries are not allowed, but {$eq: /.../} is a simple scalar match, not a regex query.
    checkPredicateDisallowed({a: /a/});
    checkPredicateOK({
        input: {a: {$eq: /a/}},
        output: {
            $and: [{"control.min.a": {$_internalExprLte: /a/}}, {"control.max.a": {$_internalExprGte: /a/}}],
        },
    });

    // The min/max for a field is present iff at least one event in the bucket has that field.
    // So {$exists: true} queries can be mapped to the min/max for that field.
    // This can be used as an alternative to sparse indexes.
    checkPredicateOK({
        input: {a: {$exists: true}},
        output: {
            $and: [{"control.min.a": {$exists: true}}, {"control.max.a": {$exists: true}}],
        },
    });
    // However, this means we can't push down {$exists: false}.  A bucket where the min/max for a
    // field is non-missing may contain a mixture of missing / non-missing, so we can't exclude it
    // on the basis of the control fields.
    checkPredicateDisallowed({a: {$exists: false}});

    // $or on metadata, metric, or both.
    checkPredicateOK({
        input: {$or: [{[metaField + ".a"]: {$lt: 5}}, {[metaField + ".b"]: {$lt: 6}}]},
        output: {$or: [{"meta.a": {$lt: 5}}, {"meta.b": {$lt: 6}}]},
    });
    checkPredicateOK({
        input: {$or: [{"a": {$lt: 5}}, {b: {$lt: 6}}]},
        output: {
            $or: [{"control.min.a": {$_internalExprLt: 5}}, {"control.min.b": {$_internalExprLt: 6}}],
        },
    });
    checkPredicateOK({
        input: {$or: [{"a": {$lt: 5}}, {[metaField + ".b"]: {$lt: 6}}]},
        output: {
            $or: [{"control.min.a": {$_internalExprLt: 5}}, {"meta.b": {$lt: 6}}],
        },
    });

    // If any argument of the $or is disallowed, we report the error even when an always-true
    // predicate appears in it.
    checkPredicateDisallowed({$or: [{}, {a: {}}]});

    // $in on metadata is fine.
    checkPredicateOK({
        input: {[metaField + ".a"]: {$in: [1, 2, 5]}},
        output: {"meta.a": {$in: [1, 2, 5]}},
    });

    // $in on a metric is slightly complicated. $in is equivalent to a disjunction of {a: _}.
    // In a typical case this is the same as a disjunction of $eq:
    checkPredicateOK({
        input: {a: {$in: [1, 2, 5]}},
        output: {
            $or: [
                {
                    $and: [{"control.min.a": {$_internalExprLte: 1}}, {"control.max.a": {$_internalExprGte: 1}}],
                },
                {
                    $and: [{"control.min.a": {$_internalExprLte: 2}}, {"control.max.a": {$_internalExprGte: 2}}],
                },
                {
                    $and: [{"control.min.a": {$_internalExprLte: 5}}, {"control.max.a": {$_internalExprGte: 5}}],
                },
            ],
        },
    });
    // Since {a: null} is not implemented, neither is {$in: [null]}.
    checkPredicateDisallowed({a: {$in: [null]}});
    // {a: {$in: [/abc/]}} is equivalent to {a: /abc/} which executes the regex, and is not allowed.
    checkPredicateDisallowed({a: {$in: [/abc/]}});

    // Predicates on time are pushed down, being converted into predicates on the control field in
    // the process. These generated predicates don't necessarily result in fewer buckets being
    // indexed: we're just following the same rules for createIndex that we use when optimizing a
    // query.
    checkPredicateOK({
        input: {[timeField]: {$lt: ISODate("2020-01-01T00:00:00Z")}},
        output: {
            $and: [
                {"control.min.time": {"$_internalExprLt": ISODate("2020-01-01T00:00:00Z")}},
                {"control.max.time": {"$_internalExprLt": ISODate("2020-01-01T01:00:00Z")}},
            ],
        },
    });
    checkPredicateOK({
        input: {[timeField]: {$gt: ISODate("2020-01-01T00:00:00Z")}},
        output: {
            $and: [
                {"control.max.time": {"$_internalExprGt": ISODate("2020-01-01T00:00:00Z")}},
                {"control.min.time": {"$_internalExprGt": ISODate("2019-12-31T23:00:00Z")}},
            ],
        },
    });
    checkPredicateOK({
        input: {[timeField]: {$lte: ISODate("2020-01-01T00:00:00Z")}},
        output: {
            $and: [
                {"control.min.time": {"$_internalExprLte": ISODate("2020-01-01T00:00:00Z")}},
                {"control.max.time": {"$_internalExprLte": ISODate("2020-01-01T01:00:00Z")}},
            ],
        },
    });
    checkPredicateOK({
        input: {[timeField]: {$gte: ISODate("2020-01-01T00:00:00Z")}},
        output: {
            $and: [
                {"control.max.time": {"$_internalExprGte": ISODate("2020-01-01T00:00:00Z")}},
                {"control.min.time": {"$_internalExprGte": ISODate("2019-12-31T23:00:00Z")}},
            ],
        },
    });
    checkPredicateOK({
        input: {[timeField]: {$eq: ISODate("2020-01-01T00:00:00Z")}},
        output: {
            $and: [
                {"control.min.time": {"$_internalExprLte": ISODate("2020-01-01T00:00:00Z")}},
                {"control.min.time": {"$_internalExprGte": ISODate("2019-12-31T23:00:00Z")}},
                {"control.max.time": {"$_internalExprGte": ISODate("2020-01-01T00:00:00Z")}},
                {"control.max.time": {"$_internalExprLte": ISODate("2020-01-01T01:00:00Z")}},
            ],
        },
    });
}
