"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/log.js');
load("jstests/libs/parallel_shell_helpers.js");

function ingressHandshakeMetricsTest(conn, options) {
    // Unpack test options

    const {
        rootCredentials = {user: 'root', pwd: 'root'},
        guestCredentials = {user: 'guest', pwd: 'guest'},
        dbName = 'test',
        collectionName = 'test_coll',
        preAuthDelayMillis,
        postAuthDelayMillis,
        helloProcessingDelayMillis,
        helloResponseDelayMillis,
    } = options;

    const totalDelayMillis = preAuthDelayMillis + postAuthDelayMillis;

    // Define helper functions

    function setupTest() {
        let admin = conn.getDB('admin');
        let db = conn.getDB(dbName);

        admin.createUser(Object.assign(rootCredentials, {roles: jsTest.adminUserRoles}));
        admin.auth(rootCredentials.user, rootCredentials.pwd);

        db.createUser(Object.assign(guestCredentials, {roles: jsTest.readOnlyUserRoles}));
        db[collectionName].insert({foo: 42});
    }

    function performAuthTestConnection() {
        let testConn = new Mongo(conn.host);
        let db = testConn.getDB(dbName);
        sleep(preAuthDelayMillis);
        db.auth(guestCredentials.user, guestCredentials.pwd);
        sleep(postAuthDelayMillis);
        db[collectionName].findOne();
    }

    function performHelloTestConnection() {
        let waitInHelloFailPoint =
            configureFailPoint(conn, 'waitInHello', {delayMillis: helloProcessingDelayMillis});
        let delaySendMessageFailPoint = configureFailPoint(
            conn, 'sessionWorkflowDelaySendMessage', {millis: helloResponseDelayMillis});
        let testConn = new Mongo(conn.host);
        delaySendMessageFailPoint.off();
        waitInHelloFailPoint.off();
    }

    function getTotalTimeToFirstNonAuthCommandMillis() {
        let status = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
        printjson(status);
        return status.metrics.network.totalTimeToFirstNonAuthCommandMillis;
    }

    function logLineExists(id, predicate) {
        let serverLog = assert.commandWorked(conn.adminCommand({getLog: "global"})).log;
        for (const line of findMatchingLogLines(serverLog, {id: id})) {
            if (predicate(JSON.parse(line))) {
                return true;
            }
        }
        return false;
    }

    function timingLogLineExists() {
        return logLineExists(6788700, entry => entry.attr.elapsedMillis >= postAuthDelayMillis);
    }

    function helloCompletedLogLineExists() {
        return logLineExists(
            6724100,
            entry => (entry.attr.processingDurationMillis >= helloProcessingDelayMillis) &&
                (entry.attr.sendingDurationMillis >= helloResponseDelayMillis) &&
                (entry.attr.okCode == 1));
    }

    function performAuthMetricsTest() {
        let metricBeforeTest = getTotalTimeToFirstNonAuthCommandMillis();

        performAuthTestConnection();

        assert(timingLogLineExists, "No matching 'first non-auth command' log line");

        let metricAfterTest = getTotalTimeToFirstNonAuthCommandMillis();
        assert.gte(metricAfterTest - metricBeforeTest, totalDelayMillis);
    }

    function performHelloMetricsTest() {
        performHelloTestConnection();
        assert(helloCompletedLogLineExists, "No matching 'hello completed' log line");
    }

    // Setup the test and return the function that will perform the test when called

    setupTest();

    return function() {
        assert.commandWorked(conn.adminCommand({clearLog: 'global'}));
        performAuthMetricsTest();
        performHelloMetricsTest();
    };
}
