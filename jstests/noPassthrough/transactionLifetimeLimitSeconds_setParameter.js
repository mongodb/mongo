// Test server validation of the 'transactionLifetimeLimitSeconds' server parameter setting on
// startup and via setParameter command.

(function() {
    'use strict';

    // transactionLifetimeLimitSeconds is set to be higher than its default value in test suites.
    delete TestData.transactionLifetimeLimitSeconds;

    /**
     * Takes a server connection 'conn' and server parameter 'field' and calls getParameter on the
     * connection to retrieve the current setting of that server parameter.
     */
    function getParameter(conn, field) {
        var q = {getParameter: 1};
        q[field] = 1;

        var ret = conn.getDB("admin").runCommand(q);
        return ret[field];
    }

    /**
     * Calls setParameter on 'conn' server connection, setting server parameter 'field' to 'value'.
     */
    function setParameter(conn, field, value) {
        var cmd = {setParameter: 1};
        cmd[field] = value;
        return conn.adminCommand(cmd);
    }

    // Check that 'transaictionLifetimeLimitSeconds' defaults to 60s on startup.
    let conn1 = MongoRunner.runMongod({});
    assert.eq(getParameter(conn1, "transactionLifetimeLimitSeconds"), 60);

    // Check that 'transactionLifetimeLimitSeconds' can be set via setParameter.
    assert.commandWorked(setParameter(conn1, "transactionLifetimeLimitSeconds", 30));
    assert.eq(getParameter(conn1, "transactionLifetimeLimitSeconds"), 30);

    // Check that setParameter on 'transactionLifetimeLimitSeconds' does validation checking:
    // setting 'transactionLifetimeLimitSeconds' below 1s should not be allowed.
    assert.commandFailedWithCode(setParameter(conn1, "transactionLifetimeLimitSeconds", -15),
                                 ErrorCodes.BadValue);
    assert.eq(getParameter(conn1, "transactionLifetimeLimitSeconds"), 30);

    MongoRunner.stopMongod(conn1);

    // Check that 'transactionLifetimeLimitSeconds' can be set on startup.
    let conn2 = MongoRunner.runMongod({setParameter: "transactionLifetimeLimitSeconds=50"});
    assert.eq(getParameter(conn2, "transactionLifetimeLimitSeconds"), 50);
    MongoRunner.stopMongod(conn2);

    // Check that 'transactionLifetimeLimitSeconds' cannot be set below 1s on startup.
    let conn3 = MongoRunner.runMongod({setParameter: "transactionLifetimeLimitSeconds=0"});
    assert.eq(
        null,
        conn3,
        "expected mongod to fail to startup with an invalid 'transactionLifetimeLimitSeconds'" +
            " server parameter setting of 0s.");

})();
