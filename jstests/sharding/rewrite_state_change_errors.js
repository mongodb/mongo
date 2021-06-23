/**
 * Check that state change errors are suppressed by mongos.
 * Those injected by the failCommand FailPoint are left alone.
 *
 * A naturally-occurring state-change error `ShutdownInProgress` in the
 * `code` or `writeConcernError` of a response would be rewritten by a mongos, but
 * because it was injected by a mongos failCommand failpoint, it should be
 * left alone.
 *
 * This behavior can be overridden by adding the bool `allowRewriteStateChange`
 * to the failpoint's configuration object.
 */

(function() {
'use strict';

var st = new ShardingTest({shards: 1, mongos: 1});
var mongos = st.s;
var db = mongos.getDB("test");

const doesRewrite = ErrorCodes.probeMongosRewrite(mongos);

const injected = ErrorCodes.ShutdownInProgress;

function merge(x, y) {
    for (const k in y)
        x[k] = y[k];
}

function runInsertScenarios(injectCodeField, extractCode, message) {
    for (const testCase of [
             [{}, false, "Not rewritten by default."],
             [{allowRewriteStateChange: false}, false, "Explicitly disallowing rewrites."],
             [{allowRewriteStateChange: true}, true, "Explicitly allowing rewrites."],
    ]) {
        const [allowConfigFields, rewriteAllowed, desc] = testCase;
        const summary = message + ": " + desc;

        jsTestLog(summary);

        var fpData = {failCommands: ["insert"]};

        injectCodeField(fpData, injected);
        merge(fpData, allowConfigFields);

        mongos.adminCommand({
            configureFailPoint: "failCommand",
            mode: {times: 1},
            data: fpData,
        });
        const res = db.runCommand({insert: db.coll.getName(), documents: [{x: 1}]});
        assert.eq(
            extractCode(res),
            rewriteAllowed ? ErrorCodes.doMongosRewrite(mongos, injected, doesRewrite) : injected,
            summary + ": " + tojson(testCase) + ": " + tojson(res));
    }
}

runInsertScenarios((obj, ec) => {
    obj.errorCode = ec;
}, res => res.code, "Injected errorCode");

// The code site where failCommand controls the injection of writeConcernError
// is separated from the place where failCommand controls the injection of
// more basic command errors, so it is tested separately here.

runInsertScenarios((obj, ec) => {
    obj.writeConcernError = {code: ec, errmsg: "Injected by failCommand"};
}, res => res.writeConcernError.code, "Injected writeConcernError");

st.stop();
})();
