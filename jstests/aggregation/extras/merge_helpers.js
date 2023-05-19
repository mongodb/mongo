/**
 * Collection of helper functions for testing the $merge aggregation stage.
 */

load("jstests/libs/fixture_helpers.js");  // For isSharded.

function withEachKindOfWriteStage(targetColl, callback) {
    callback({$out: targetColl.getName()});
    callback({$merge: {into: targetColl.getName()}});
}

/**
 * Executes the callback function with each valid combination of 'whenMatched' and 'whenNotMatched'
 * modes (as named arguments). Note that one mode is a pipeline.
 */
function withEachMergeMode(callback) {
    callback({whenMatchedMode: "replace", whenNotMatchedMode: "insert"});
    callback({whenMatchedMode: "replace", whenNotMatchedMode: "fail"});
    callback({whenMatchedMode: "replace", whenNotMatchedMode: "discard"});

    callback({whenMatchedMode: "merge", whenNotMatchedMode: "insert"});
    callback({whenMatchedMode: "merge", whenNotMatchedMode: "fail"});
    callback({whenMatchedMode: "merge", whenNotMatchedMode: "discard"});

    callback({whenMatchedMode: "fail", whenNotMatchedMode: "insert"});

    callback({whenMatchedMode: "keepExisting", whenNotMatchedMode: "insert"});

    callback({whenMatchedMode: [], whenNotMatchedMode: "insert"});
    callback({whenMatchedMode: [], whenNotMatchedMode: "fail"});
    callback({whenMatchedMode: [], whenNotMatchedMode: "discard"});
}

function assertMergeFailsForAllModesWithCode(
    {source, target, onFields, options, prevStages = [], errorCodes}) {
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        const mergeStage = {
            into: {db: target.getDB().getName(), coll: target.getName()},
            whenMatched: whenMatchedMode,
            whenNotMatched: whenNotMatchedMode
        };
        if (onFields) {
            mergeStage.on = onFields;
        }
        const pipeline = prevStages.concat([{$merge: mergeStage}]);

        // In sharded passthrough suites, the error code may be different depending on where we
        // extract the "on" fields.
        const cmd = {aggregate: source.getName(), pipeline: pipeline, cursor: {}};
        assert.commandFailedWithCode(source.getDB().runCommand(Object.merge(cmd, options)),
                                     errorCodes);
    });
}

function assertMergeFailsWithoutUniqueIndex({source, target, onFields, options, prevStages}) {
    assertMergeFailsForAllModesWithCode(
        {source, target, onFields, options, prevStages, errorCodes: [51183, 51190]});
}

function assertMergeSucceedsWithExpectedUniqueIndex(
    {source, target, onFields, options, prevStages = []}) {
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        // Skip the combination of merge modes which will fail depending on the contents of the
        // source and target collection, as this will cause the assertion below to trip.
        if (whenMatchedMode == "fail" || whenNotMatchedMode == "fail")
            return;

        const mergeStage = {
            into: {db: target.getDB().getName(), coll: target.getName()},
            whenMatched: whenMatchedMode,
            whenNotMatched: whenNotMatchedMode
        };

        // Do not include the "on" fields in the command if the caller did not specify it.
        if (onFields) {
            mergeStage.on = onFields;
        }
        const pipeline = prevStages.concat([{$merge: mergeStage}]);

        assert.commandWorked(target.remove({}));
        assert.doesNotThrow(() => source.aggregate(pipeline, options));
    });
}

// Helper to drop a collection without using the shell helper, and thus avoiding the implicit
// recreation in the sharded collections passthrough suites.
function dropWithoutImplicitRecreate(collName) {
    db.runCommand({drop: collName});
}
