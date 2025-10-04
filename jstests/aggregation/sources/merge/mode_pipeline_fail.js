// Tests the behaviour of the $merge stage with whenMatched=[pipeline] and whenNotMatched=fail.
//
// Cannot implicitly shard accessed collections because a collection can be implictly created and
// exists when none is expected.
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const source = db[`${jsTest.name()}_source`];
source.drop();
const target = db[`${jsTest.name()}_target`];
target.drop();
const mergeStage = {
    $merge: {into: target.getName(), whenMatched: [{$addFields: {x: 2}}], whenNotMatched: "fail"},
};
const pipeline = [mergeStage];

// Test $merge when some documents in the source collection don't have a matching document in
// the target collection.
(function testMergeFailsIfMatchingDocumentNotFound() {
    // Single document without a match.
    assert.commandWorked(
        source.insert([
            {_id: 1, a: 1},
            {_id: 2, a: 2},
            {_id: 3, a: 3},
        ]),
    );
    assert.commandWorked(
        target.insert([
            {_id: 1, b: 1},
            {_id: 3, b: 3},
        ]),
    );
    let error = assert.throws(() => source.aggregate(pipeline));
    assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
    // Since there is no way to guarantee the ordering of the writes performed by $merge, it
    // follows that the contents of the target collection will depend on when the write which
    // triggers the MergeStageNoMatchingDocument error executes. As such, we test that the
    // target collection contains some combination of its original documents and expected
    // updates. In particular, it should be the case that each document has fields '_id' and
    // 'b', but may or not have field 'x'. Additionally, 'b' should have the same value as
    // '_id' and if 'x' is present, it should be equal to 2.
    let checkOutputDocument = function (document) {
        const hasB = document.hasOwnProperty("b");
        const hasX = document.hasOwnProperty("x");
        assert(hasB, document);
        const value = document["b"];
        assert.eq(value, document["_id"], document);
        if (hasX) assert.eq(2, document["x"], document);
    };

    let result = target.find().toArray();
    assert.eq(result.length, 2, result);
    for (let elem of result) {
        checkOutputDocument(elem);
    }

    // Multiple documents without a match.
    assert(target.drop());
    assert.commandWorked(target.insert([{_id: 1, b: 1}]));
    error = assert.throws(() => source.aggregate(pipeline));
    assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
    result = target.find().toArray();
    assert.eq(result.length, 1, result);
    checkOutputDocument(result[0]);
})();

// Test $merge when all documents in the source collection have a matching document in the
// target collection.
(function testMergeWhenAllDocumentsHaveMatch() {
    // Source has a single element with a match in the target.
    assert(source.drop());
    assert(target.drop());
    assert.commandWorked(source.insert({_id: 3, a: 3}));
    assert.commandWorked(
        target.insert([
            {_id: 1, b: 1},
            {_id: 3, b: 3},
        ]),
    );
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, b: 1},
            {_id: 3, b: 3, x: 2},
        ],
    });

    // Source has multiple documents with matches in the target.
    assert(target.drop());
    assert.commandWorked(
        source.insert([
            {_id: 1, a: 1},
            {_id: 2, a: 2},
        ]),
    );
    assert.commandWorked(
        target.insert([
            {_id: 1, b: 1},
            {_id: 2, b: 2},
            {_id: 3, b: 3},
        ]),
    );
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, b: 1, x: 2},
            {_id: 2, b: 2, x: 2},
            {_id: 3, b: 3, x: 2},
        ],
    });
})();

// Test $merge when the source collection is empty. The target collection should not be
// modified.
(function testMergeWhenSourceIsEmpty() {
    assert.commandWorked(source.deleteMany({}));
    assert(target.drop());
    assert.commandWorked(
        target.insert([
            {_id: 1, b: 1},
            {_id: 2, b: 2},
        ]),
    );
    assert.doesNotThrow(() => source.aggregate(pipeline));
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, b: 1},
            {_id: 2, b: 2},
        ],
    });
})();

// Test that variables referencing the fields in the source document can be specified in the
// 'let' argument and referenced in the update pipeline.
(function testMergeWithLetVariables() {
    assert(source.drop());
    assert(target.drop());
    assert.commandWorked(
        source.insert([
            {_id: 1, a: 1, b: 1},
            {_id: 2, a: 2, b: 2},
        ]),
    );
    assert.commandWorked(
        target.insert([
            {_id: 1, c: 1},
            {_id: 2, c: 2},
        ]),
    );

    assert.doesNotThrow(() =>
        source.aggregate([
            {
                $merge: {
                    into: target.getName(),
                    let: {x: "$a", y: "$b"},
                    whenMatched: [{$set: {z: {$add: ["$$x", "$$y"]}}}],
                    whenNotMatched: "fail",
                },
            },
        ]),
    );
    assertArrayEq({
        actual: target.find().toArray(),
        expected: [
            {_id: 1, c: 1, z: 2},
            {_id: 2, c: 2, z: 4},
        ],
    });
})();
