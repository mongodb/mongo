/**
 * Collection of helper functions for testing the $out aggregation stage.
 */

load("jstests/libs/fixture_helpers.js");  // For isSharded.

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
    // TODO SERVER-40439 callback({whenMatchedMode: "replaceWithNew", whenNotMatchedMode:
    // "discard"});

    callback({whenMatchedMode: "merge", whenNotMatchedMode: "insert"});
    callback({whenMatchedMode: "merge", whenNotMatchedMode: "fail"});
    // TODO SERVER-40439 callback({whenMatchedMode: "merge", whenNotMatchedMode: "discard"});

    callback({whenMatchedMode: "fail", whenNotMatchedMode: "insert"});

    callback({whenMatchedMode: "keepExisting", whenNotMatchedMode: "insert"});

    callback({whenMatchedMode: [], whenNotMatchedMode: "insert"});
    callback({whenMatchedMode: [], whenNotMatchedMode: "fail"});
    // TODO SERVER-40439 callback({whenMatchedMode: [{$addFields: {x: 1}}], whenNotMatchedMode:
    // "discard"});
}

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
