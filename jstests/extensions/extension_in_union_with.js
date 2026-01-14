/**
 * Tests extension stages in $unionWith.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const bread = [
    {_id: 0, breadType: "sourdough"},
    {_id: 1, breadType: "brioche"},
    {_id: 2, breadType: "rye"},
];
coll.insertMany(bread);

function makeUnionWithPipeline(pipeline) {
    return [{$unionWith: {coll: collName, pipeline}}];
}

function runTest(extensionStagePipeline, extensionStageResults) {
    // Top-level $unionWith pipeline.
    let results = coll.aggregate(makeUnionWithPipeline(extensionStagePipeline)).toArray();
    // We expect the contents of the collection followed by the extension stage's results.
    assert.eq(results.length, bread.length + extensionStageResults.length, results);
    assert.sameMembers(bread, results.slice(0, bread.length), results);
    assert.docEq(extensionStageResults, results.slice(bread.length), results);

    // Nested $unionWith pipeline.
    results = coll.aggregate(makeUnionWithPipeline(makeUnionWithPipeline(extensionStagePipeline))).toArray();
    // We expect the contents of the collection twice (for each union), followed by the extension stage's results last.
    assert.eq(results.length, bread.length * 2 + extensionStageResults.length, results);
    assert.sameMembers(bread, results.slice(0, bread.length), results);
    assert.sameMembers(bread, results.slice(bread.length, bread.length * 2), results);
    assert.docEq(extensionStageResults, results.slice(bread.length * 2), results);
}

(function testUnionWithSourceExtensionStage() {
    const extensionSourceStagePipeline = [{$toast: {temp: 350.0, numSlices: 2}}];
    runTest(extensionSourceStagePipeline, [
        {slice: 0, isBurnt: false},
        {slice: 1, isBurnt: false},
    ]);
})();

/*
TODO SERVER-116984 Enable this test.
(function testUnionWithDesugarExtensionStage() {
    // $readNDocuments desugars to an extension source stage + $_internalSearchIdLookup.
    const extensionDesugarStagePipeline = [{$readNDocuments: {numDocs: 2}}];
    runTest(extensionDesugarStagePipeline, bread.slice(0, 2));
})();
*/

(function testUnionWithTransformExtensionStage() {
    const extensionTransformStagePipeline = [{$sort: {_id: 1}}, {$extensionLimit: 2}];
    runTest(extensionTransformStagePipeline, bread.slice(0, 2));
})();
