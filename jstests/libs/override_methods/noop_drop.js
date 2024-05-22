import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kIgnoredDDLCommands = new Set(["drop", "dropDatabase"]);

const mock_res = {
    ok: 1
};

function runCommandIgnoreDropOperations(
    conn, _dbName, _commandName, commandObj, func, makeFuncArgs) {
    if (kIgnoredDDLCommands.has(_commandName)) {
        return mock_res;
    }
    let res = func.apply(conn, makeFuncArgs(commandObj));
    jsTestLog(res);
    return res;
}

OverrideHelpers.prependOverrideInParallelShell("jstesats/libs/override_methods/noop_drop.js");

OverrideHelpers.overrideRunCommand(runCommandIgnoreDropOperations);
