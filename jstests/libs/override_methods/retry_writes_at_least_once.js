/**
 * Overrides Mongo.prototype.runCommand to retry all retryable writes at least once, randomly more
 * than that, regardless of the outcome of the command. Returns the result of the latest attempt.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";

Random.setRandomSeed();

const kExtraRetryProbability = 0.2;

const mongoRunCommandOriginal = Mongo.prototype.runCommand;

Mongo.prototype.runCommand = function runCommand(dbName, cmdObj, options) {
    return runWithRetries(this, cmdObj, mongoRunCommandOriginal, arguments);
};

function runWithRetries(mongo, cmdObj, clientFunction, clientFunctionArguments) {
    let cmdName = Object.keys(cmdObj)[0];

    const isRetryableWriteCmd = cmdObj.hasOwnProperty("lsid") &&
        cmdObj.hasOwnProperty("txnNumber") && RetryableWritesUtil.isRetryableWriteCmdName(cmdName);
    const canRetryWrites = _ServerSession.canRetryWrites(cmdObj);

    let res = clientFunction.apply(mongo, clientFunctionArguments);

    if ((isRetryableWriteCmd && canRetryWrites)
        // Commit is always considered a retryable write.
        || cmdName === "commitTransaction") {
        jsTest.log.info("*** Initial response", {res});
        let retryAttempt = 1;
        do {
            jsTest.log.info("*** Retry attempt: " + retryAttempt + ", for command: " + cmdName,
                            {txnNumber: cmdObj.txnNumber, lsid: cmdObj.lsid});
            ++retryAttempt;
            res = clientFunction.apply(mongo, clientFunctionArguments);
            jsTest.log.info("*** Retry response", {res});
        } while (Random.rand() <= kExtraRetryProbability);
    }

    return res;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/retry_writes_at_least_once.js");
