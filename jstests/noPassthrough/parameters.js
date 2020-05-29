// Test runtime setting of various setParameters on a mongod.

(function() {
'use strict';

const dbConn = MongoRunner.runMongod();

function setAndCheckParameter(dbConn, parameterName, newValue, expectedResult) {
    jsTest.log("Test setting parameter: " + parameterName + " to value: " + newValue);
    const getParameterCommand = {getParameter: 1};
    getParameterCommand[parameterName] = 1;
    const getResult = assert.commandWorked(dbConn.adminCommand(getParameterCommand));
    const oldValue = getResult[parameterName];

    const setParameterCommand = {setParameter: 1};
    setParameterCommand[parameterName] = newValue;
    const setResult = assert.commandWorked(dbConn.adminCommand(setParameterCommand));
    assert.eq(setResult.was, oldValue, tojson(setResult));

    const finalResult = assert.commandWorked(dbConn.adminCommand(getParameterCommand));
    // If we have explicitly set an "exptectedResult", use that, else use "newValue".  This is for
    // cases where the server does some type coersion that changes the value.
    if (typeof expectedResult === "undefined") {
        expectedResult = newValue;
    }
    assert.eq(finalResult[parameterName], expectedResult, tojson(finalResult));
}

setAndCheckParameter(dbConn, "logLevel", 1);
setAndCheckParameter(dbConn, "logLevel", 1.5, 1);
setAndCheckParameter(dbConn, "journalCommitInterval", 100);
setAndCheckParameter(dbConn, "traceExceptions", true);
setAndCheckParameter(dbConn, "traceExceptions", false);
setAndCheckParameter(dbConn, "traceExceptions", 1, true);
setAndCheckParameter(dbConn, "traceExceptions", 0, false);
setAndCheckParameter(dbConn, "traceExceptions", "foo", true);
setAndCheckParameter(dbConn, "traceExceptions", "", true);
setAndCheckParameter(dbConn, "syncdelay", 0);
setAndCheckParameter(dbConn, "syncdelay", 3000);

function ensureSetParameterFailure(dbConn, parameterName, newValue, reason) {
    jsTest.log("Test setting parameter: " + parameterName + " to invalid value: " + newValue);
    const setParameterCommand = {setParameter: 1};
    setParameterCommand[parameterName] = newValue;
    const ret = assert.commandFailed(dbConn.adminCommand(setParameterCommand));
    printjson(ret);
    if (reason !== undefined) {
        assert(ret.errmsg.includes(reason));
    }
}

ensureSetParameterFailure(dbConn, "logLevel", "foo");
ensureSetParameterFailure(dbConn, "logLevel", "1.5");
ensureSetParameterFailure(dbConn, "logLevel", -1);
ensureSetParameterFailure(dbConn, "journalCommitInterval", "foo");
ensureSetParameterFailure(dbConn, "journalCommitInterval", "0.5");
ensureSetParameterFailure(dbConn, "journalCommitInterval", 0.5);
ensureSetParameterFailure(dbConn, "journalCommitInterval", 1000);
ensureSetParameterFailure(dbConn, "journalCommitInterval", 0);
ensureSetParameterFailure(dbConn, "syncdelay", 10 * 1000 * 1000);
ensureSetParameterFailure(dbConn, "syncdelay", -10 * 1000 * 1000);
ensureSetParameterFailure(
    dbConn, "scramSHA256IterationCount", 18446744073709551616, 'Out of bounds');
ensureSetParameterFailure(
    dbConn, "scramSHA256IterationCount", -18446744073709551616, 'Out of bounds');
ensureSetParameterFailure(dbConn, "scramSHA256IterationCount", NaN, 'Unable to coerce NaN/Inf');
ensureSetParameterFailure(
    dbConn, "scramSHA256IterationCount", Infinity, 'Unable to coerce NaN/Inf');

MongoRunner.stopMongod(dbConn);

jsTest.log("noPassthrough_parameters_test succeeded!");
})();
