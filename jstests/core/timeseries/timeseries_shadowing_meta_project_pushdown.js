/**
 * Test the behavior of $project / $addFields that shadows the original metaField of a timeseries
 * collection.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

const timeField = "t";
const metaField = "meta1";
const coll = db.timeseries_shadowing_meta_project_pushdown;
const date1 = new Date();

function runTest({docs, pipeline, expectedResults}) {
    coll.drop();
    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));
    assert.commandWorked(coll.insertMany(docs));
    const results = coll.aggregate(pipeline).toArray();
    assert.eq(expectedResults.length,
              results.length,
              `Expected ${tojson(expectedResults)} but got ${tojson(results)}`);
    for (let i = 0; i < results.length; ++i) {
        assert.docEq(expectedResults[i],
                     results[i],
                     `Expected ${tojson(expectedResults)} but got ${tojson(results)}`);
    }
}

//
// The following tests verify that the metaField that is shadowed by itself should work as expected.
//
(function testAddFieldsShadowingMetaByItself() {
    runTest({
        docs: [{_id: 1, [timeField]: date1, [metaField]: 2, meta: 100}],
        pipeline: [{$addFields: {[metaField]: {$getField: metaField}}}],
        expectedResults: [{[timeField]: date1, [metaField]: 2, _id: 1, meta: 100}]
    });
})();

(function testAddFieldsShadowingMetaByItselfWithOtherFields() {
    runTest({
        docs: [{_id: 1, [timeField]: date1, [metaField]: 2, meta: 100}],
        pipeline: [{
            $addFields: {[metaField]: `\$${metaField}`, [timeField]: `\$${timeField}`, _id: "$_id"}
        }],
        expectedResults: [{[timeField]: date1, [metaField]: 2, _id: 1, meta: 100}]
    });
})();

(function testProjectShadowingMetaByItself() {
    runTest({
        docs: [{_id: 1, [timeField]: date1, [metaField]: 2, meta: 100}],
        pipeline: [{$project: {[metaField]: {$getField: metaField}, _id: 0}}],
        expectedResults: [{[metaField]: 2}]
    });
})();

(function testProjectShadowingMetaByItselfWithOtherFields() {
    runTest({
        docs: [{_id: 1, [timeField]: date1, [metaField]: 2, meta: 100}],
        pipeline: [
            {$project: {[metaField]: `\$${metaField}`, [timeField]: `\$${timeField}`, _id: "$_id"}}
        ],
        expectedResults: [{[metaField]: 2, [timeField]: date1, _id: 1}]
    });
})();

//
// The following tests verify that the metaField that is shadowed by a subfield should work as
// expected.
//
(function testAddFieldsShadowingMetaBySubfield() {
    runTest({
        docs: [{_id: 1, [timeField]: date1, [metaField]: {s: "string"}, meta: 100}],
        pipeline: [{$addFields: {[metaField]: `\$${metaField}.s`}}],
        expectedResults: [{[timeField]: date1, [metaField]: "string", _id: 1, meta: 100}]
    });
})();

(function testProjectShadowingMetaBySubfield() {
    runTest({
        docs: [{_id: 1, [timeField]: date1, [metaField]: {s: "string"}, meta: 100}],
        pipeline: [{$project: {[metaField]: `\$${metaField}.s`, meta: 1, _id: 0}}],
        expectedResults: [{[metaField]: "string", meta: 100}]
    });
})();

//
// The following tests verify that the metaField that is shadowed by an expression for meta
// subfields should work as expected.
//
(function testAddFieldsShadowingMetaByExpr() {
    runTest({
        docs: [{_id: 1, [timeField]: date1, [metaField]: {a: 1, b: 2}, meta: 100}],
        pipeline: [{$addFields: {[metaField]: {$add: [`\$${metaField}.a`, `\$${metaField}.b`]}}}],
        expectedResults: [{[timeField]: date1, [metaField]: 3, _id: 1, meta: 100}]
    });
})();

(function testProjectShadowingMetaByExpr() {
    runTest({
        docs: [{_id: 1, [timeField]: date1, [metaField]: {a: 1, b: 2}, meta: 100}],
        pipeline: [{
            $project:
                {[metaField]: {$add: [`\$${metaField}.a`, `\$${metaField}.b`]}, meta: 1, _id: 0}
        }],
        expectedResults: [{[metaField]: 3, meta: 100}]
    });
})();

//
// The following tests verify that the metaField that is shadowed by an expression for non meta
// field should work as expected.
//
(function testAddFieldsShadowingMetaByExpr() {
    runTest({
        docs: [{_id: 1, [timeField]: date1, [metaField]: {a: 1, b: 2}, meta: 100}],
        pipeline: [{$addFields: {[metaField]: "$meta"}}],
        expectedResults: [{[timeField]: date1, [metaField]: 100, _id: 1, meta: 100}]
    });
})();

(function testProjectShadowingMetaByExpr() {
    runTest({
        docs: [{_id: 1, [timeField]: date1, [metaField]: {a: 1, b: 2}, meta: 100}],
        pipeline: [{$project: {[metaField]: "$meta", meta: 1, _id: 0}}],
        expectedResults: [{[metaField]: 100, meta: 100}]
    });
})();
