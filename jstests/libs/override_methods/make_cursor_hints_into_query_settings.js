import {sysCollNamePrefix} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {
    transformIndexHintsFromTimeseriesToView
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {
    getCollectionName,
    getCommandName,
    getExplainCommand,
    getInnerCommand,
    isSystemBucketNss
} from "jstests/libs/cmd_object_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {everyWinningPlan, isIdhackOrExpress} from "jstests/libs/query/analyze_plan.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

function isMinMaxQuery(cmdObj) {
    // When using min()/max() a hint of which index to use must be provided.
    return 'min' in cmdObj || 'max' in cmdObj;
}

function requestsResumeToken(cmdObj) {
    // '$_requestResumeToken' need to be accompanied by '$natural' hints, so query settings
    // can't be used in this case.
    return "$_requestResumeToken" in cmdObj && cmdObj["$_requestResumeToken"] === true;
}

function runCommandOverride(conn, dbName, _cmdName, cmdObj, clientFunction, makeFuncArgs) {
    // Execute the original command and return the result immediately if it failed. This allows test
    // cases which assert on thrown error codes to still pass.
    const originalResponse = clientFunction.apply(conn, makeFuncArgs(cmdObj));
    if (!originalResponse.ok) {
        return originalResponse;
    }

    // Check if the original command could use query settings instead of cursor hints. If not,
    // exit early with the original command response.
    const db = conn.getDB(dbName);
    const innerCmd = getInnerCommand(cmdObj);
    const shouldApplyQuerySettings =
        // Only intercept commands with cursor hints.
        "hint" in innerCmd &&
        // Only intercept command types supported by query settings.
        QuerySettingsUtils.isSupportedCommand(getCommandName(innerCmd)) &&
        !isMinMaxQuery(innerCmd) && !requestsResumeToken(innerCmd);
    if (!shouldApplyQuerySettings) {
        return originalResponse;
    }

    const isSystemBucketColl = isSystemBucketNss(innerCmd);

    // Construct the equivalent query settings, remove the hint from the original command object and
    // build the representative query.
    const allowedIndexes = isSystemBucketColl
        ? [transformIndexHintsFromTimeseriesToView(innerCmd.hint)]
        : [innerCmd.hint];
    delete innerCmd.hint;

    const explainCmd = getExplainCommand(innerCmd);
    const explain = assert.commandWorked(
        db.runCommand(explainCmd),
        `Failed running explain command ${
            tojson(explainCmd)} while transforming cursor hints into query settings`);
    const isIdHackQuery =
        explain && everyWinningPlan(explain, (winningPlan) => isIdhackOrExpress(db, winningPlan));
    if (isIdHackQuery) {
        // Query settings cannot be set over IDHACK or express queries.
        return originalResponse;
    }

    // If the collection used is a view, determine the underlying collection.
    const resolvedCollName = getCollectionName(db, innerCmd);
    const collectionName = isSystemBucketColl ? resolvedCollName.substring(sysCollNamePrefix.length)
                                              : resolvedCollName;
    if (!collectionName) {
        return originalResponse;
    }

    // Ensure that no open, hanging cursors are left so they do not interfere with
    // the rest of the test cases.
    if (originalResponse.cursor) {
        const hangingCursor = new DBCommandCursor(db, originalResponse);
        hangingCursor.close();
    }

    // Set the equivalent query settings, execute the original command without the hint, and finally
    // remove all the settings.
    const settings = {indexHints: {ns: {db: dbName, coll: collectionName}, allowedIndexes}};
    const qsutils = new QuerySettingsUtils(db, collectionName);
    const representativeQuery = qsutils.makeQueryInstance(
        isSystemBucketColl ? {...innerCmd, [getCommandName(innerCmd)]: collectionName} : innerCmd);
    return qsutils.withQuerySettings(
        representativeQuery, settings, () => clientFunction.apply(conn, makeFuncArgs(cmdObj)));
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/make_cursor_hints_into_query_settings.js");
