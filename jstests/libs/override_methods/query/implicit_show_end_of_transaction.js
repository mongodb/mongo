/**
 * Loading this file overrides Mongo.prototype.runCommand with a function that adds
 * showEndOfTransaction: "required" to any $changeStream aggregation which does not already
 * specify the option. This forces the server to exercise the endOfTransaction event synthesis
 * and delivery paths in suites which enable the end-of-transaction feature flags. Streams which
 * explicitly request a reader version other than "v2" (e.g. "v1") are passed through as-is,
 * since the option is only supported for v2 change stream readers. Non-$changeStream commands
 * are passed through as-is.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {getChangeStreamStage} from "jstests/libs/query/change_stream_util.js";

// Returns the nested aggregate command if `cmdObj` is a `$changeStream` aggregate (either
// directly or wrapped in an `explain`), and the $changeStream stage specifies neither the
// 'showEndOfTransaction' option nor an explicit reader version other than "v2". Otherwise
// returns null.
function getChangeStreamAggregateWithoutShowEndOfTransaction(cmdObj) {
    if (!cmdObj) {
        return null;
    }
    const aggCmd = cmdObj.aggregate
        ? cmdObj
        : typeof cmdObj.explain === "object"
          ? cmdObj.explain
          : null;
    const changeStreamStage = getChangeStreamStage(aggCmd);
    if (
        typeof changeStreamStage === "object" &&
        changeStreamStage !== null &&
        changeStreamStage.constructor === Object &&
        !changeStreamStage.hasOwnProperty("showEndOfTransaction") &&
        (!changeStreamStage.hasOwnProperty("version") || changeStreamStage.version === "v2")
    ) {
        return aggCmd;
    }
    return null;
}

function runChangeStreamWithRequiredShowEndOfTransaction(
    conn,
    _dbName,
    _commandName,
    commandObj,
    func,
    makeFuncArgs,
) {
    const aggCmd = getChangeStreamAggregateWithoutShowEndOfTransaction(commandObj);
    if (aggCmd) {
        aggCmd.pipeline[0].$changeStream["showEndOfTransaction"] = "required";
    }
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query/implicit_show_end_of_transaction.js",
);
OverrideHelpers.overrideRunCommand(runChangeStreamWithRequiredShowEndOfTransaction);
