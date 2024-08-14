import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {iterateMatchingLogLines} from "jstests/libs/log.js";

export function ingressHandshakeMetricsTest(conn, options) {
    // Unpack test options

    const {
        rootCredentials = {user: 'root', pwd: 'root'},
        guestCredentials = {user: 'guest', pwd: 'guest'},
        dbName = 'test',
        collectionName = 'test_coll',
        connectionHealthLoggingOn,
        preAuthDelayMillis,
        postAuthDelayMillis,
        helloProcessingDelayMillis,
        helloResponseDelayMillis,
        helloFailPointName,
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

        if (!connectionHealthLoggingOn) {
            assert.commandWorked(conn.adminCommand(
                {setParameter: 1, enableDetailedConnectionHealthMetricLogLines: false}));
        }
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
            configureFailPoint(conn, helloFailPointName, {delayMillis: helloProcessingDelayMillis});
        let delaySendMessageFailPoint = configureFailPoint(
            conn, 'sessionWorkflowDelayOrFailSendMessage', {millis: helloResponseDelayMillis});
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
        for (const line of iterateMatchingLogLines(serverLog, {id: id})) {
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

        if (connectionHealthLoggingOn) {
            assert(timingLogLineExists, "No matching 'first non-auth command' log line");
        } else {
            assert.eq(
                timingLogLineExists(),
                false,
                "Found 'first non-auth command log line' despite disabling connection health logging");
        }

        let metricAfterTest = getTotalTimeToFirstNonAuthCommandMillis();
        assert.gte(metricAfterTest - metricBeforeTest, totalDelayMillis);
    }

    function performHelloMetricsTest() {
        performHelloTestConnection();

        if (connectionHealthLoggingOn) {
            assert(helloCompletedLogLineExists, "No matching 'hello completed' log line");
        } else {
            assert.eq(
                helloCompletedLogLineExists(),
                false,
                "Found 'hello completed' log line despite disabling conection health logging");
        }
    }

    // Setup the test and return the function that will perform the test when called

    setupTest();

    return function() {
        assert.commandWorked(conn.adminCommand({clearLog: 'global'}));
        performAuthMetricsTest();
        performHelloMetricsTest();
    };
}
