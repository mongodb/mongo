/**
 * Contains helper functions for testing server parameters on start up and via get/setParameter.
 */

"use strict";

/**
 * Takes a server connection 'conn' and server parameter 'field' and calls getParameter on the
 * connection to retrieve the current setting of that server parameter.
 */
function getParameter(conn, field) {
    var q = {getParameter: 1};
    q[field] = 1;

    var ret = assert.commandWorked(conn.getDB("admin").runCommand(q));
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

/**
 * Helper for validation testing of server parameters with numeric values.
 *
 * Tests server parameter 'parameterName'. Will run startup tests if 'isStartupParameter' is true;
 * and setParameter tests if 'isRuntimeParameter' is true. Tests lower and upper bound of the server
 * parameter if 'hasLowerBound' and 'hasUpperBound' are true, respectively; otherwise
 * 'lowerOutOfBounds' and 'upperOutOfBounds' are ignored. 'defaultValue' is checked on startup.
 * 'nonDefaultValidValue' defines a safe setting to ensure a non-default setting is successful.
 *
 * 'lowerOutOfBounds' and 'upperOutOfBounds' should be the invalid values below and above the lowest
 * and highest valid values, respectively.
 */
function testNumericServerParameter(parameterName,
                                    isStartupParameter,
                                    isRuntimeParameter,
                                    defaultValue,
                                    nonDefaultValidValue,
                                    hasLowerBound,
                                    lowerOutOfBounds,
                                    hasUpperBound,
                                    upperOutOfBounds) {
    jsTest.log("Checking that '" + parameterName + "' defaults to '" + defaultValue +
               "' on startup");
    let conn1 = MongoRunner.runMongod({});
    assert(conn1);
    assert.eq(getParameter(conn1, parameterName), defaultValue);

    if (isRuntimeParameter) {
        jsTest.log("Checking that '" + parameterName + "' can be set at runtime to '" +
                   nonDefaultValidValue + "'");
        assert.commandWorked(setParameter(conn1, parameterName, nonDefaultValidValue));
        assert.eq(getParameter(conn1, parameterName), nonDefaultValidValue);

        if (hasLowerBound) {
            jsTest.log("Checking that '" + parameterName + "' cannot be set below bounds to '" +
                       lowerOutOfBounds + "'");
            assert.commandFailedWithCode(setParameter(conn1, parameterName, lowerOutOfBounds),
                                         ErrorCodes.BadValue);
            assert.eq(getParameter(conn1, parameterName), nonDefaultValidValue);
        }

        if (hasUpperBound) {
            jsTest.log("Checking that '" + parameterName + "' cannot be set above bounds to '" +
                       upperOutOfBounds + "'");
            assert.commandFailedWithCode(setParameter(conn1, parameterName, upperOutOfBounds),
                                         ErrorCodes.BadValue);
            assert.eq(getParameter(conn1, parameterName), nonDefaultValidValue);
        }
    }

    MongoRunner.stopMongod(conn1);

    if (isStartupParameter) {
        jsTest.log("Checking that '" + parameterName + "' can be set to '" + nonDefaultValidValue +
                   "' on startup");
        let conn2 =
            MongoRunner.runMongod({setParameter: parameterName + "=" + nonDefaultValidValue});
        assert(conn2);
        assert.eq(getParameter(conn2, parameterName), nonDefaultValidValue);
        MongoRunner.stopMongod(conn2);

        if (hasLowerBound) {
            jsTest.log("Checking that '" + parameterName + "' cannot be set below bounds to '" +
                       lowerOutOfBounds + "' on startup");
            let conn3 =
                MongoRunner.runMongod({setParameter: parameterName + "=" + lowerOutOfBounds});
            assert.eq(null,
                      conn3,
                      "expected mongod to fail to startup with an invalid '" + parameterName + "'" +
                          " server parameter setting '" + lowerOutOfBounds + "'.");
        }

        if (hasUpperBound) {
            jsTest.log("Checking that '" + parameterName + "' cannot be set above bounds to '" +
                       upperOutOfBounds + "' on startup");
            let conn4 =
                MongoRunner.runMongod({setParameter: parameterName + "=" + upperOutOfBounds});
            assert.eq(null,
                      conn4,
                      "expected mongod to fail to startup with an invalid '" + parameterName + "'" +
                          " server parameter setting '" + upperOutOfBounds + "'.");
        }
    }
}
