// Tests the behaviour of the $merge stage with whenMatched=[pipeline] and whenNotMatched=fail.
//
// Cannot implicitly shard accessed collections because a collection can be implictly created and
// exists when none is expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
    load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.isMongos.

    const source = db[`${jsTest.name()}_source`];
    source.drop();
    const target = db[`${jsTest.name()}_target`];
    target.drop();
    const mergeStage = {
        $merge:
            {into: target.getName(), whenMatched: [{$addFields: {x: 2}}], whenNotMatched: "fail"}
    };
    const pipeline = [mergeStage];

    // Test $merge when some documents in the source collection don't have a matching document in
    // the target collection.
    (function testMergeFailsIfMatchingDocumentNotFound() {
        // Single document without a match.
        assert.commandWorked(source.insert([{_id: 1, a: 1}, {_id: 2, a: 2}, {_id: 3, a: 3}]));
        assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 3, b: 3}]));
        let error = assert.throws(() => source.aggregate(pipeline));
        assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [{_id: 1, b: 1, x: 2}, {_id: 3, b: 3, x: 2}]
        });

        // Multiple documents without a match.
        assert(target.drop());
        assert.commandWorked(target.insert([{_id: 1, b: 1}]));
        error = assert.throws(() => source.aggregate(pipeline));
        assert.commandFailedWithCode(error, ErrorCodes.MergeStageNoMatchingDocument);
        assertArrayEq({actual: target.find().toArray(), expected: [{_id: 1, b: 1, x: 2}]});
    })();

    // Test $merge when all documents in the source collection have a matching document in the
    // target collection.
    (function testMergeWhenAllDocumentsHaveMatch() {
        // Source has a single element with a match in the target.
        assert(source.drop());
        assert(target.drop());
        assert.commandWorked(source.insert({_id: 3, a: 3}));
        assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 3, b: 3}]));
        assert.doesNotThrow(() => source.aggregate(pipeline));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, b: 1}, {_id: 3, b: 3, x: 2}]});

        // Source has multiple documents with matches in the target.
        assert(target.drop());
        assert.commandWorked(source.insert([{_id: 1, a: 1}, {_id: 2, a: 2}]));
        assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 2, b: 2}, {_id: 3, b: 3}]));
        assert.doesNotThrow(() => source.aggregate(pipeline));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [{_id: 1, b: 1, x: 2}, {_id: 2, b: 2, x: 2}, {_id: 3, b: 3, x: 2}]
        });
    })();

    // Test $merge when the source collection is empty. The target collection should not be
    // modified.
    (function testMergeWhenSourceIsEmpty() {
        assert.commandWorked(source.deleteMany({}));
        assert(target.drop());
        assert.commandWorked(target.insert([{_id: 1, b: 1}, {_id: 2, b: 2}]));
        assert.doesNotThrow(() => source.aggregate(pipeline));
        assertArrayEq(
            {actual: target.find().toArray(), expected: [{_id: 1, b: 1}, {_id: 2, b: 2}]});
    })();
}());
