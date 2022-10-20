"use strict";

load('jstests/libs/log.js');

function sessionAuthMetricsTest(conn, options) {
    // Unpack test options

    const {
        rootCredentials = {user: 'root', pwd: 'root'},
        guestCredentials = {user: 'guest', pwd: 'guest'},
        dbName = 'test',
        collectionName = 'test_coll',
        preAuthDelayMillis,
        postAuthDelayMillis
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

    function performTestConnection(host) {
        let conn = new Mongo(host);
        let db = conn.getDB(dbName);
        sleep(preAuthDelayMillis);
        db.auth(guestCredentials.user, guestCredentials.pwd);
        sleep(postAuthDelayMillis);
        db[collectionName].findOne();
    }

    function getTotalTimeToFirstNonAuthCommandMillis() {
        let status = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
        return status.metrics.network.totalTimeToFirstNonAuthCommandMillis;
    }

    function timingLogLineExists() {
        let serverLog = assert.commandWorked(conn.adminCommand({getLog: "global"})).log;
        for (const line of findMatchingLogLines(serverLog, {id: 6788700})) {
            let entry = JSON.parse(line);
            if (entry.attr.elapsedMillis >= postAuthDelayMillis) {
                return true;
            }
        }
        return false;
    }

    // Setup the test and return the function that will perform the test when called

    setupTest();

    return function() {
        assert.commandWorked(conn.adminCommand({clearLog: 'global'}));

        let metricBeforeTest = getTotalTimeToFirstNonAuthCommandMillis(conn);

        performTestConnection(conn.host, preAuthDelayMillis, postAuthDelayMillis);

        assert(timingLogLineExists(conn, postAuthDelayMillis));

        let metricAfterTest = getTotalTimeToFirstNonAuthCommandMillis(conn);
        assert.gte(metricAfterTest - metricBeforeTest, totalDelayMillis);
    };
}
