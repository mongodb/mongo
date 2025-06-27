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

    function getIngressHandshakeMetrics() {
        let status = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
        jsTest.log.info({'status.metrics.network': status.metrics.network});
        return Object.fromEntries([
            'totalTimeToFirstNonAuthCommandMillis',
            'averageTimeToCompletedTLSHandshakeMicros',
            'averageTimeToCompletedHelloMicros',
            'averageTimeToCompletedAuthMicros',
        ].map(name => [name, status.metrics.network[name]]));
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

    function performAuthMetricsTest() {
        const before = getIngressHandshakeMetrics();

        performAuthTestConnection();

        if (connectionHealthLoggingOn) {
            assert(timingLogLineExists, "No matching 'first non-auth command' log line");
        } else {
            assert.eq(
                timingLogLineExists(),
                false,
                "Found 'first non-auth command log line' despite disabling connection health logging");
        }

        const after = getIngressHandshakeMetrics();

        // Total time increased by at least as long as we slept.
        const diffMillis = after.totalTimeToFirstNonAuthCommandMillis -
            before.totalTimeToFirstNonAuthCommandMillis;
        assert.gte(diffMillis, totalDelayMillis);

        // Average time to hello will be no larger than average time to completed auth.
        assert.lte(after.averageTimeToCompletedHelloMicros, after.averageTimeToCompletedAuthMicros);

        // Since the average time metrics have values, they are present and non-negative.
        assert.gte(after.averageTimeToCompletedAuthMicros, 0);
        assert.gte(after.averageTimeToCompletedHelloMicros, 0);

        // "network.averageTimeToCompletedTLSHandshakeMicros" is not tested here.
        // See `jstests/ssl/ssl_ingress_conn_metrics.js`.
    }

    // Setup the test and return the function that will perform the test when called

    setupTest();

    return function() {
        assert.commandWorked(conn.adminCommand({clearLog: 'global'}));
        performAuthMetricsTest();
    };
}
