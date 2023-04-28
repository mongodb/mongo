// Verify setParameters paramaters which are an alias to a config parameter do not have the value
// passed with setParameter as a startup argument overwritten by the config default.

(function() {
'use strict';

const defaultsConn = MongoRunner.runMongod();
function getDefaultValue(parameterName) {
    const res =
        assert.commandWorked(defaultsConn.adminCommand({getParameter: 1, [parameterName]: 1}));
    return res[parameterName];
}

let paramsDict = {};
const parameters = ['journalCommitInterval', 'syncdelay'];
parameters.forEach(param => {
    const defaultValue = getDefaultValue(param);
    const setValue = defaultValue + 1;
    paramsDict[param] = setValue;
});
MongoRunner.stopMongod(defaultsConn);

function runTestOnConn(conn, setParams) {
    Object.keys(setParams).forEach(param => {
        const res = assert.commandWorked(conn.adminCommand({getParameter: 1, [param]: 1}));
        assert.eq(res[param], setParams[param]);
    });
}

// Run the test on a standalone mongod.
const standaloneConn = MongoRunner.runMongod({setParameter: paramsDict});
runTestOnConn(standaloneConn, paramsDict);
MongoRunner.stopMongod(standaloneConn);
}());
