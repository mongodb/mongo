/**
 * Collection of helper functions for testing the $out aggregation stage.
 */

load("jstests/libs/fixture_helpers.js");  // For isSharded.

function withEachOutMode(callback) {
    callback("replaceCollection");
    callback("insertDocuments");
    callback("replaceDocuments");
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