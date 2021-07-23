// Test passing security token with op messages.
// @tags: [requires_replication, requires_sharding]

(function() {
'use strict';

const kLogLevelForToken = 4;
const kAcceptedSecurityTokenID = 5838100;
const kLogMessageID = 5060500;
const isMongoStoreEnabled = TestData.setParameters.featureFlagMongoStore;

if (!isMongoStoreEnabled) {
    assert.throws(() => MongoRunner.runMongod({
        setParameter: "acceptOpMsgSecurityToken=true",
    }));
    return;
}

function assertNoTokensProcessedYet(conn) {
    assert.eq(false,
              checkLog.checkContainsOnceJson(conn, kAcceptedSecurityTokenID, {}),
              'Unexpected security token has been processed');
}

function runTest(conn, enabled) {
    const admin = conn.getDB('admin');
    assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'admin', roles: ['root']}));
    assert(admin.auth('admin', 'admin'));

    // Dial up the logging to watch for tenant ID being processed.
    const originalLogLevel =
        assert.commandWorked(admin.setLogLevel(kLogLevelForToken)).was.verbosity;

    // Basic OP_MSG command.
    conn._setSecurityToken({});
    assert.commandWorked(admin.runCommand({ping: 1}));
    assertNoTokensProcessedYet(conn);

    // Passing a security token with unknown fields will always fail.
    conn._setSecurityToken({invalid: 1});
    assert.commandFailed(admin.runCommand({ping: 1}));
    conn._setSecurityToken({});  // clear so check-log can work
    assertNoTokensProcessedYet(conn);

    const tenantID = ObjectId();
    conn._setSecurityToken({tenant: tenantID});
    if (enabled) {
        // Basic use.
        assert.commandWorked(admin.runCommand({logMessage: 'This is a test'}));

        // Look for "Accepted Security Token" message with explicit tenant logging.
        // Log line will contain {"$oid": "12345..."} rather than ObjectId.
        const expect = {token: {tenant: {"$oid": tenantID.str}}};
        jsTest.log('Checking for: ' + tojson(expect));
        checkLog.containsJson(conn, kAcceptedSecurityTokenID, expect, 'Security Token not logged');

        // Now look for logMessage log line with implicit logging.
        const logMessages = checkLog.getGlobalLog(conn)
                                .map((l) => JSON.parse(l))
                                .filter((l) => l.id === kLogMessageID);
        jsTest.log(logMessages);
        assert.eq(logMessages.length, 1, 'Unexpected number of entries');
        assert.eq(logMessages[0].tenant, tenantID.str, 'Unable to find tenant ID');
    } else {
        // Attempting to pass a valid looking security token will fail if not enabled.
        assert.commandFailed(admin.runCommand({logMessage: 'This is a test'}));
    }

    // Restore logging and conn token before shutting down.
    conn._setSecurityToken({});
    assert.commandWorked(admin.setLogLevel(originalLogLevel));
}

function runShardTest(mongos, mongod, command) {
    const db1 = mongos.getDB('db1');
    const tenantID = ObjectId();

    const mongodOrig =
        assert.commandWorked(mongod.getDB('admin').setLogLevel(kLogLevelForToken)).was.verbosity;
    const mongosOrig =
        assert.commandWorked(mongos.getDB('admin').setLogLevel(kLogLevelForToken)).was.verbosity;
    mongos._setSecurityToken({tenant: tenantID});
    assert.commandWorked(db1.runCommand(command));
    mongos._setSecurityToken({});
    assert.commandWorked(mongos.getDB('admin').setLogLevel(mongosOrig));
    assert.commandWorked(mongod.getDB('admin').setLogLevel(mongodOrig));

    const expect = {token: {tenant: {"$oid": tenantID.str}}};
    checkLog.containsJson(
        mongos, kAcceptedSecurityTokenID, expect, 'Security Token not logged on mongos');
    checkLog.containsJson(
        mongod, kAcceptedSecurityTokenID, expect, 'Security Token not logged on mongod');
}

function runTests(enabled) {
    const opts = {
        setParameter: "acceptOpMsgSecurityToken=" + (enabled ? 'true' : 'false'),
    };
    {
        const standalone = MongoRunner.runMongod(opts);
        assert(standalone !== null, "MongoD failed to start");
        runTest(standalone, enabled);
        MongoRunner.stopMongod(standalone);
    }
    {
        const rst = new ReplSetTest({nodes: 2, nodeOptions: opts});
        rst.startSet();
        rst.initiate();
        runTest(rst.getPrimary(), enabled);
        rst.stopSet();
    }
    {
        const st = new ShardingTest({
            shards: 1,
            mongos: 1,
            config: 1,
            other: {shardOptions: opts, configOptions: opts, mongosOptions: opts}
        });
        runTest(st.s0, enabled);

        if (enabled) {
            // Check for passthroughs to config/data shards.
            runShardTest(st.s0, st.config0, {createUser: 'user1', pwd: 'user', roles: []});
            runShardTest(st.s0, st.shard0, {insert: 'coll1', documents: [{_id: ObjectId(), x: 1}]});
        }

        st.stop();
    }
}

runTests(true);
runTests(false);
})();
