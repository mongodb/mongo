import {getQueryPlanners, getWinningPlan, isIdhackOrExpress} from "jstests/libs/analyze_plan.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

function hasSupportedHint(cmdObj) {
    return "hint" in cmdObj;
}

function getCommandType(cmdObj) {
    const supportedCommands = ["aggregate", "distinct", "find"];
    return supportedCommands.find((key) => (key in cmdObj));
}

function isSupportedCommandType(cmdObj) {
    return getCommandType(cmdObj) !== undefined;
}

function isMinMaxQuery(cmdObj) {
    // When using min()/max() a hint of which index to use must be provided.
    return 'min' in cmdObj || 'max' in cmdObj;
}

function requestsResumeToken(cmdObj) {
    // '$_requestResumeToken' need to be accompanied by '$natural' hints, so query settings
    // can't be used in this case.
    return "$_requestResumeToken" in cmdObj && cmdObj["$_requestResumeToken"] === true;
}

function isIdHackQuery(db, cmdObj) {
    const {hint, ...queryWithoutHint} = cmdObj;
    const explain = db.runCommand({explain: queryWithoutHint});
    const queryPlanners = getQueryPlanners(explain);
    return queryPlanners.every(queryPlanner => isIdhackOrExpress(db, getWinningPlan(queryPlanner)));
}

function getInnerCommand(cmdObj) {
    // In most cases, the command is wrapped in an explain object.
    return "explain" in cmdObj && typeof cmdObj.explain === "object" ? cmdObj.explain : cmdObj;
}

function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    // Execute the original command and return the result immediately if it failed. This allows test
    // cases which assert on thrown error codes to still pass.
    const originalResponse = clientFunction.apply(conn, makeFuncArgs(cmdObj));
    try {
        assert.commandWorked(
            originalResponse,
            "Skipped converting cursor hints into query settings because the original command ended in an error.");
    } catch (e) {
        return originalResponse;
    }

    // Check if the original command could use query settings instead of cursor hints. If not,
    // exit early with the original command response.
    const db = conn.getDB(dbName);
    const innerCmd = getInnerCommand(cmdObj);
    const shouldApplyQuerySettings = hasSupportedHint(innerCmd) &&
        isSupportedCommandType(innerCmd) && !requestsResumeToken(innerCmd) &&
        !isMinMaxQuery(innerCmd) && !isIdHackQuery(db, innerCmd);
    if (!shouldApplyQuerySettings) {
        return originalResponse;
    }

    // Construct the equivalent query settings, remove the hint from the original command object and
    // build the representative query.
    // TODO SERVER-85242 Remove the '$natural' hints once the fallback is re-implemented.
    const allowedIndexes = [innerCmd.hint, {$natural: 1}, {$natural: -1}];
    delete innerCmd.hint;
    const commandType = getCommandType(innerCmd);
    const collectionName = innerCmd[commandType];
    const settings = {indexHints: {allowedIndexes}};
    const qsutils = new QuerySettingsUtils(db, collectionName);
    const representativeQuery = (function() {
        switch (commandType) {
            case "find":
                return qsutils.makeFindQueryInstance(innerCmd);
            case "aggregate":
                return qsutils.makeAggregateQueryInstance(innerCmd);
            case "distinct":
                return qsutils.makeDistinctQueryInstance(innerCmd);
        }
    })();

    // Set the equivalent query settings, execute the original command without the hint, and finally
    // remove all the settings.
    return qsutils.withQuerySettings(
        representativeQuery, settings, () => clientFunction.apply(conn, makeFuncArgs(cmdObj)));
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/make_cursor_hints_into_query_settings.js");
