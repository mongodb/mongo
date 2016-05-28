var dbConn = MongoRunner.runMongod();

function setAndCheckParameter(dbConn, parameterName, newValue, expectedResult) {
    jsTest.log("Test setting parameter: " + parameterName + " to value: " + newValue);
    var getParameterCommand = {getParameter: 1};
    getParameterCommand[parameterName] = 1;
    var ret = dbConn.adminCommand(getParameterCommand);
    assert.eq(ret.ok, 1, tojson(ret));
    oldValue = ret[parameterName];

    var setParameterCommand = {setParameter: 1};
    setParameterCommand[parameterName] = newValue;
    var ret = dbConn.adminCommand(setParameterCommand);
    assert.eq(ret.ok, 1, tojson(ret));
    assert.eq(ret.was, oldValue, tojson(ret));

    var ret = dbConn.adminCommand(getParameterCommand);
    assert.eq(ret.ok, 1, tojson(ret));
    // If we have explicitly set an "exptectedResult", use that, else use "newValue".  This is for
    // cases where the server does some type coersion that changes the value.
    if (typeof expectedResult === "undefined") {
        assert.eq(ret[parameterName], newValue, tojson(ret));
    } else {
        assert.eq(ret[parameterName], expectedResult, tojson(ret));
    }
    return newValue;
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
setAndCheckParameter(dbConn, "replMonitorMaxFailedChecks", 30);
setAndCheckParameter(dbConn, "replMonitorMaxFailedChecks", 30.5, 30);
setAndCheckParameter(dbConn, "replMonitorMaxFailedChecks", -30);

function ensureSetParameterFailure(dbConn, parameterName, newValue) {
    jsTest.log("Test setting parameter: " + parameterName + " to invalid value: " + newValue);
    var setParameterCommand = {setParameter: 1};
    setParameterCommand[parameterName] = newValue;
    var ret = dbConn.adminCommand(setParameterCommand);
    assert.eq(ret.ok, 0, tojson(ret));
    printjson(ret);
}

ensureSetParameterFailure(dbConn, "logLevel", "foo");
ensureSetParameterFailure(dbConn, "logLevel", "1.5");
ensureSetParameterFailure(dbConn, "logLevel", -1);
ensureSetParameterFailure(dbConn, "journalCommitInterval", "foo");
ensureSetParameterFailure(dbConn, "journalCommitInterval", "0.5");
ensureSetParameterFailure(dbConn, "journalCommitInterval", 0.5);
ensureSetParameterFailure(dbConn, "journalCommitInterval", 1000);
ensureSetParameterFailure(dbConn, "journalCommitInterval", 0);
ensureSetParameterFailure(dbConn, "replMonitorMaxFailedChecks", "foo");

MongoRunner.stopMongod(dbConn.port);

jsTest.log("noPassthrough_parameters_test succeeded!");
