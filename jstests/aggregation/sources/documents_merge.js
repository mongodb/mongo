/**
 * This is the test for $documents stage along with $merge stage in an aggregation pipeline,
 * including verifying the bug in SERVER-85892 is addressed when the spec 'whenMatched' is not
 * empty.
 *
 * @tags: [ requires_fcv_80 ]
 */

import {
    dropWithoutImplicitRecreate,
    withEachMergeMode
} from "jstests/aggregation/extras/merge_helpers.js";

const outColl = db[`${jsTest.name()}_out`];
const outCollName = outColl.getName();
const expectedTotalDocs = 100;

function assertDocsInsertedCorrectly(docs, pipeline) {
    const msg = `Failed with pipeline: ${JSON.stringify(pipeline, null, 2)}`;

    assert.eq(expectedTotalDocs, docs.length, msg);
    for (let i = 0; i < expectedTotalDocs; i++) {
        assert.eq(docs[i].x, i, msg);
    }
}

const documentsStage = {
    $documents: {$map: {input: {$range: [0, expectedTotalDocs]}, in : {x: "$$this"}}}
};

function testFn(pipeline, assertFn) {
    // Creates an index as $merge requires a unique index with the 'on' identifier field. Then
    // inserts a document allowed to be matched.
    dropWithoutImplicitRecreate(outCollName);
    assert.commandWorked(outColl.createIndex({x: 1}, {unique: true}));
    assert.commandWorked(outColl.insert({x: 10}));

    assert.doesNotThrow(() => db.aggregate(pipeline));
    let res = outColl.find({}, {_id: 0}).sort({x: 1}).toArray();
    assertDocsInsertedCorrectly(res, pipeline);
    assertFn(res);
}

{  // Tests $merge with non-empty pipeline along with let in whenMatched spec.
    const pipeline = [
        documentsStage,
        {
            $merge: {
                into: outCollName,
                let : {num: 123},
                whenMatched: [{$set: {number: "$$num"}}],
                on: "x"
            }
        }
    ];

    testFn(pipeline, res => {
        assert.eq(res.filter(elem => elem.number === 123).length, 1);
    });
}

{  // Tests $merge with non-empty pipeline in whenMatched spec.
    const pipeline = [
        documentsStage,
        {$merge: {into: outCollName, whenMatched: [{$set: {new: true}}], on: "x"}}
    ];

    testFn(pipeline, res => {
        assert.eq(res.filter(elem => elem.new === true).length, 1);
    });
}

// Tests each combination of merge modes.
withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
    const expectErrorCode = whenMatchedMode === "fail" ? ErrorCodes.DuplicateKey
        : whenNotMatchedMode === "fail"                ? ErrorCodes.MergeStageNoMatchingDocument
                                                       : null;

    // Creates an index as $merge requires a unique index with the 'on' identifier field. Then
    // inserts a document allowed to be matched.
    dropWithoutImplicitRecreate(outCollName);
    assert.commandWorked(outColl.createIndex({x: 1}, {unique: true}));
    assert.commandWorked(outColl.insert({x: 10, old: true}));

    const pipeline = [
        documentsStage,
        {
            $merge: {
                into: outCollName,
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode,
                on: "x",
            }
        }
    ];

    if (expectErrorCode) {
        assert.throwsWithCode(() => db.aggregate(pipeline), expectErrorCode);
        return;
    }

    assert.doesNotThrow(() => db.aggregate(pipeline));
    let res = outColl.find({}, {_id: 0}).sort({x: 1}).toArray();
    if (whenNotMatchedMode == "discard") {
        assert.eq(outColl.count(), 1);
    } else {
        assertDocsInsertedCorrectly(res, pipeline);
    }

    // Asserts if the old document is replaced when 'whenMatchedMode' is "replace".
    assert.eq(res.filter(elem => elem.old === true).length, whenMatchedMode == "replace" ? 0 : 1);
});
