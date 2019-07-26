/**
 * Use prototype overrides to massage command objects and make them suitable to run for multi
 * statement transaction suites.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");

function runCommandInMultiStmtTxnPassthrough(
    conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    // If the command is in a wrapped form, then we look for the actual command object inside
    // the query/$query object.
    let commandObjUnwrapped = commandObj;
    if (commandName === "query" || commandName === "$query") {
        commandObjUnwrapped = commandObj[commandName];
        commandName = Object.keys(commandObjUnwrapped)[0];
    }

    // Ignore all commands that are part of multi statement transactions.
    if (commandObj.hasOwnProperty("autocommit")) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    const majority = {w: 'majority'};
    let massagedCmd = Object.extend(commandObjUnwrapped, {});

    // Adjust mapReduce and drop to use { w: majority } to make sure that all pending drops that
    // occurred while running these commands are finished after the command returns. This
    // is done to make sure that the pending drop of the two phase drop won't try to contest
    // with db/coll locks in the background.

    if (commandName === "mapReduce" || commandName === "mapreduce") {
        if (typeof massagedCmd.out === 'string') {
            massagedCmd.out = {replace: commandObjUnwrapped.out, writeConcern: majority};
        } else if (typeof massagedCmd.out === 'object') {
            let outOptions = massagedCmd.out;
            if (!outOptions.hasOwnProperty('inline')) {
                if (outOptions.hasOwnProperty('writeConcern')) {
                    if (outOptions.writeConcern.w !== 'majority') {
                        throw new Error(
                            'Running mapReduce with non majority write concern: ' +
                            tojson(commandObj) + '. Consider blacklisting the test ' +
                            'since the 2 phase drop can interfere with lock acquisitions.');
                    }
                } else {
                    outOptions.writeConcern = majority;
                }
            }
        }
    } else if (commandName === 'drop') {
        if (massagedCmd.hasOwnProperty('writeConcern')) {
            if (massagedCmd.writeConcern.w !== 'majority') {
                throw new Error('Running drop with non majority write concern: ' +
                                tojson(commandObj) + '. Consider blacklisting the test ' +
                                'since the 2 phase drop can interfere with lock acquisitions.');
            }
        } else {
            massagedCmd.writeConcern = majority;
        }
    }

    return func.apply(conn, makeFuncArgs(massagedCmd));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/txn_passthrough_cmd_massage.js");

OverrideHelpers.overrideRunCommand(runCommandInMultiStmtTxnPassthrough);
})();
