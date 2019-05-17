/**
 * Collection of helper functions for testing the $out aggregation stage.
 */

load("jstests/libs/fixture_helpers.js");  // For isSharded.

// TODO SERVER-40432: Remove this helper as it shouldn't be needed anymore.
function withEachOutMode(callback) {
    callback("replaceCollection");
    callback("insertDocuments");
    callback("replaceDocuments");
}

/**
 * Executes the callback function with each valid combination of 'whenMatched' and 'whenNotMatched'
 * modes (as named arguments). Note that one mode is a pipeline.
 */
function withEachMergeMode(callback) {
    callback({whenMatchedMode: "replaceWithNew", whenNotMatchedMode: "insert"});
    callback({whenMatchedMode: "replaceWithNew", whenNotMatchedMode: "fail"});
    callback({whenMatchedMode: "replaceWithNew", whenNotMatchedMode: "discard"});

    callback({whenMatchedMode: "merge", whenNotMatchedMode: "insert"});
    callback({whenMatchedMode: "merge", whenNotMatchedMode: "fail"});
    callback({whenMatchedMode: "merge", whenNotMatchedMode: "discard"});

    callback({whenMatchedMode: "fail", whenNotMatchedMode: "insert"});

    callback({whenMatchedMode: "keepExisting", whenNotMatchedMode: "insert"});

    callback({whenMatchedMode: [], whenNotMatchedMode: "insert"});
    callback({whenMatchedMode: [], whenNotMatchedMode: "fail"});
    callback({whenMatchedMode: [], whenNotMatchedMode: "discard"});
}

// TODO SERVER-40432: Remove this helper as it shouldn't be needed anymore.
function assertUniqueKeyIsInvalid({source, target, uniqueKey, options, prevStages}) {
    withEachOutMode((mode) => {
        if (mode === "replaceCollection" && FixtureHelpers.isSharded(target))
            return;

        prevStages = (prevStages || []);
        const pipeline = prevStages.concat([{
            $out: {
                db: target.getDB().getName(),
                to: target.getName(),
                mode: mode,
                uniqueKey: uniqueKey
            }
        }]);

        const cmd = {aggregate: source.getName(), pipeline: pipeline, cursor: {}};
        assert.commandFailedWithCode(source.getDB().runCommand(Object.merge(cmd, options)), 50938);
    });
}

// TODO SERVER-40432: Remove this helper as it shouldn't be needed anymore.
function assertUniqueKeyIsValid({source, target, uniqueKey, options, prevStages}) {
    withEachOutMode((mode) => {
        if (mode === "replaceCollection" && FixtureHelpers.isSharded(target))
            return;

        prevStages = (prevStages || []);
        let outStage = {
            db: target.getDB().getName(),
            to: target.getName(),
            mode: mode,
        };

        // Do not include the uniqueKey in the command if the caller did not specify it.
        if (uniqueKey !== undefined) {
            outStage = Object.extend(outStage, {uniqueKey: uniqueKey});
        }
        const pipeline = prevStages.concat([{$out: outStage}]);

        assert.commandWorked(target.remove({}));
        assert.doesNotThrow(() => source.aggregate(pipeline, options));
    });
}

function assertFailsWithoutUniqueIndex({source, target, onFields, options, prevStages}) {
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        prevStages = (prevStages || []);
        const pipeline = prevStages.concat([{
            $merge: {
                into: {db: target.getDB().getName(), coll: target.getName()},
                on: onFields,
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }]);

        const cmd = {aggregate: source.getName(), pipeline: pipeline, cursor: {}};
        assert.commandFailedWithCode(source.getDB().runCommand(Object.merge(cmd, options)), 51190);
    });
}

function assertSucceedsWithExpectedUniqueIndex({source, target, onFields, options, prevStages}) {
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        // Skip the combination of merge modes which will fail depending on the contents of the
        // source and target collection, as this will cause the assertion below to trip.
        if (whenMatchedMode == "fail" || whenNotMatchedMode == "fail")
            return;

        prevStages = (prevStages || []);
        let mergeStage = {
            into: {db: target.getDB().getName(), coll: target.getName()},
            whenMatched: whenMatchedMode,
            whenNotMatched: whenNotMatchedMode
        };

        // Do not include the "on" fields in the command if the caller did not specify it.
        if (onFields !== undefined) {
            mergeStage = Object.extend(mergeStage, {on: onFields});
        }
        const pipeline = prevStages.concat([{$merge: mergeStage}]);

        assert.commandWorked(target.remove({}));
        assert.doesNotThrow(() => source.aggregate(pipeline, options));
    });
}
