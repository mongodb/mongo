/**
 * Tests that the unpacking stage has correct unpacking behaviour when $match is pushed into it.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_fcv_62,
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
import {getAggPlanStages, getEngine, getPlanStage} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";

const coll = db[jsTestName()];
const timeField = 'time';
const metaField = 'm';
const aTime = ISODate('2022-01-01T00:00:00');
const sbeEnabledForUnpackPushdown = checkSbeRestrictedOrFullyEnabled(db);

/**
 * Runs a 'pipeline', asserts the bucket unpacking 'behaviour' (either include or exclude) is
 * expected.
 */
const runTest = function({docs, pipeline, behaviour, expectedResult}) {
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField, metaField}}));
    assert.commandWorked(coll.insertMany(docs));

    const explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    let unpackStage = null;
    if (getEngine(explain) === "classic") {
        // In the classic engine $_internalUnpackBucket is run as an aggregation stage.
        const unpackStages = getAggPlanStages(explain, '$_internalUnpackBucket');
        assert.eq(1,
                  unpackStages.length,
                  "Should only have a single $_internalUnpackBucket stage: " + tojson(explain));
        unpackStage = unpackStages[0].$_internalUnpackBucket;
    } else if (sbeEnabledForUnpackPushdown) {
        // In the case when only unpack is pushed down to SBE, the explain has it in agg stages.
        const unpackStages = getAggPlanStages(explain, "UNPACK_TS_BUCKET");
        assert.eq(1,
                  unpackStages.length,
                  "Should only have a single UNPACK_TS_BUCKET stage: " + tojson(explain));
        unpackStage = unpackStages[0];
    }
    assert(unpackStage, `Should have unpack stage in ${tojson(explain)}`);

    if (behaviour.include) {
        assert(unpackStage.include,
               `Unpacking stage should have 'include' behaviour for pipeline ${
                   tojson(pipeline)} but got ${tojson(explain)}`);
        assert.sameMembers(behaviour.include, unpackStage.include, "Includes of unpack stage");
    }
    if (behaviour.exclude) {
        assert(unpackStage.exclude,
               `Unpacking stage should have 'exclude' behaviour for pipeline ${
                   tojson(pipeline)} but got ${tojson(explain)}`);
        assert.sameMembers(behaviour.exclude, unpackStage.exclude, "Excludes of unpack stage");
    }

    const res = coll.aggregate(pipeline).toArray();
    assert.eq(res.length,
              expectedResult.length,
              `Incorrect number of results: ${tojson(res)} + for pipeline ${tojson(pipeline)}`);
    res.forEach((doc, i) => {
        assert.docEq(expectedResult[i],
                     doc,
                     `Incorrect result: ${tojson(res)} + for pipeline ${tojson(pipeline)}`);
    });
};

/**
 * Cases for $match then $project.
 * The separate $project stage might be subsumed by unpacking, but we won't check for it.
 **/

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {_id: 1}}],
    behaviour: {include: ['_id', 'a']},
    expectedResult: [{_id: 2}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, b: 10, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, b: 20, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {b: 1}}],
    behaviour: {include: ['_id', 'a', 'b']},
    expectedResult: [{b: 20, _id: 2}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, b: 10, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, b: 20, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {_id: 0, b: 1}}],
    behaviour: {include: ['a', 'b']},
    expectedResult: [{b: 20}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {a: 1}}],
    behaviour: {include: ['_id', 'a']},
    expectedResult: [{a: 2, _id: 2}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {_id: 0, a: 1}}],
    behaviour: {include: ['a']},
    expectedResult: [{a: 2}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {a: {$add: [10, "$a"]}}}],
    behaviour: {include: ['_id', 'a']},
    expectedResult: [{a: 12, _id: 2}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, b: 10, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, b: 20, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {a: {$add: [10, "$b"]}}}],
    behaviour: {include: ['_id', 'a', 'b']},
    expectedResult: [{a: 30, _id: 2}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, b: 10, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, b: 20, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {b: {$add: [10, "$a"]}}}],
    behaviour: {include: ['_id', 'a']},
    expectedResult: [{b: 12, _id: 2}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, b: 10, _id: 1},
        {[timeField]: aTime, [metaField]: 42, a: 2, b: 20, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {a: 0}}],
    behaviour: {exclude: []},
    expectedResult: [{[timeField]: aTime, [metaField]: 42, b: 20, _id: 2}],
});

// Missing 'metaField' shouldn't be generated.
runTest({
    docs: [
        {[timeField]: ISODate(), a: 1, _id: 1},
        {[timeField]: aTime, a: 2, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {a: 0}}],
    behaviour: {exclude: []},
    expectedResult: [{[timeField]: aTime, _id: 2}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, b: 10, _id: 1},
        {[timeField]: aTime, [metaField]: 42, a: 2, b: 20, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {b: 0}}],
    behaviour: {exclude: []},
    expectedResult: [{[timeField]: aTime, [metaField]: 42, a: 2, _id: 2}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, _id: 1},
        {[timeField]: aTime, [metaField]: 42, a: 2, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {[metaField]: 0}}],
    behaviour: {exclude: []},
    expectedResult: [{[timeField]: aTime, a: 2, _id: 2}],
});

runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, _id: 2},
    ],
    pipeline: [{$match: {a: {$eq: 2}}}, {$project: {[timeField]: 0}}],
    behaviour: {exclude: []},
    expectedResult: [{[metaField]: 42, a: 2, _id: 2}],
});

/**
 * Cases for $project then $match.
 *
 * After the $project is subsumed by '$_internalUnpackBucket', the $match can also be internalized.
 * This means that an $_internalUnpackBucket stage with a 'kInclude' set and an event filter is
 * equivalent to [{$_internalUnpackBucket},{$project},{$unmatch}] -- in _this_ order.
 **/

// Match on a discarded measurement field.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 2, b: 20, _id: 2},
    ],
    pipeline: [{$project: {b: 1}}, {$match: {a: {$eq: 2}}}],
    behaviour: {include: ['_id', 'b']},
    expectedResult: [],
});

// Match on a sub-field of a discarded measurment field.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: {sub: 2}, b: 20, _id: 2},
    ],
    pipeline: [{$project: {b: 1}}, {$match: {"a.sub": {$eq: 2}}}],
    behaviour: {include: ['_id', 'b']},
    expectedResult: [],
});

// Match for non-existence of a discarded measurement field.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 2, b: 20, _id: 2},
    ],
    pipeline: [{$project: {_id: 0, b: 1}}, {$match: {a: {$exists: false}}}],
    behaviour: {include: ['b']},
    expectedResult: [{b: 20}],
});

// Match on a retained measurement field.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, b: 10, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, b: 20, _id: 2},
    ],
    pipeline: [{$project: {a: 1}}, {$match: {a: {$eq: 2}}}],
    behaviour: {include: ['_id', 'a']},
    expectedResult: [{a: 2, _id: 2}],
});

// Match on a sub-field of a retained measurement field.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: {sub: 1}, b: 10, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: {sub: 2}, b: 20, _id: 2},
    ],
    pipeline: [{$project: {a: 1}}, {$match: {"a.sub": {$eq: 2}}}],
    behaviour: {include: ['_id', 'a']},
    expectedResult: [{a: {sub: 2}, _id: 2}],
});

// Match on an over-written measurement field.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, b: 10, _id: 1},
        {[timeField]: ISODate(), [metaField]: 42, a: 2, b: 20, _id: 2},
    ],
    pipeline: [{$project: {a: {$add: [1, "$a"]}}}, {$match: {a: {$eq: 2}}}],
    behaviour: {include: ['_id', 'a']},
    expectedResult: [{a: 2, _id: 1}],
});

// Match on discarded measurement field that is used in $project.
runTest({
    docs: [{[timeField]: ISODate(), [metaField]: 42, a: 2, b: 20, _id: 1}],
    pipeline: [{$project: {b: {$add: [1, "$a"]}}}, {$match: {a: {$eq: 2}}}],
    behaviour: {include: ['_id', 'a']},
    expectedResult: [],
});

// Match on the retained 'metaField'.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, _id: 1},
        {[timeField]: ISODate(), [metaField]: 43, a: 2, _id: 2},
    ],
    pipeline: [{$project: {[metaField]: 1}}, {$match: {[metaField]: {$eq: 42}}}],
    behaviour: {include: ['_id', metaField]},
    expectedResult: [{_id: 1, [metaField]: 42}],
});

// Match on the retained 'metaField' with no other fields retained. Even though the result contains
// no event-level fields, we still have to "unpack" to generate the correct number of records.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 1, _id: 1},
        {[timeField]: ISODate(), [metaField]: 43, a: 2, _id: 2},
    ],
    pipeline: [{$project: {[metaField]: 1, _id: 0}}, {$match: {[metaField]: {$eq: 42}}}],
    behaviour: {include: [metaField]},
    expectedResult: [{[metaField]: 42}],
});

// Match on the discarded 'metaField'.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 2, _id: 2},
    ],
    pipeline: [{$project: {a: 1, _id: 0}}, {$match: {[metaField]: {$eq: 42}}}],
    behaviour: {include: ['a']},
    expectedResult: [],
});

// Match for non-existence of the discarded 'metaField'.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: 42, a: 2, _id: 2},
    ],
    pipeline: [{$project: {a: 1, _id: 0}}, {$match: {[metaField]: {$exists: false}}}],
    behaviour: {include: ['a']},
    expectedResult: [{a: 2}],
});

// Match on a sub-field of the discarded 'metaField'.
runTest({
    docs: [
        {[timeField]: ISODate(), [metaField]: {sub: 42}, a: 2, _id: 2},
    ],
    pipeline: [{$project: {a: 1, _id: 0}}, {$match: {'[metaField].sub': {$eq: 42}}}],
    behaviour: {include: ['a']},
    expectedResult: [],
});
