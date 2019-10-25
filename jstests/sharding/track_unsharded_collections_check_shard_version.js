/**
 * Tests the FCV 4.4 checkShardVersion protocol for each command, that is, when the shard does not
 * have a shardVersion cached for a namespace:
 * - if the namespace is a collection, the shard returns StaleShardVersion
 * - if the namespace is a view, the shard executes the request
 *   -- the request will either return a view definition or CommandNotSupportedOnView
 * - if the namespace does not exist, the shard executes the request
 *   -- the request will either return empty results or NamespaceNotFound
 */

// TODO (PM-1051): The UUID consistency hook should not check shards' caches once shards have an
// "UNKNOWN" state for collection filtering metadata.
TestData.skipCheckingCatalogCacheConsistencyWithShardingCatalog = true;

(function() {
'use strict';

load('jstests/sharding/libs/last_stable_mongos_commands.js');
load('jstests/sharding/libs/track_unsharded_collections_helpers.js');

const dbName = "test";
let collName, ns;

function validateTestCase(testCase) {
    // Check that only expected test case fields are present.
    for (let key of Object.keys(testCase)) {
        assert(
            [
                "skip",
                "whenNamespaceDoesNotExistFailsWith",
                "implicitlyCreatesCollection",
                "whenNamespaceIsViewFailsWith",
                "doesNotCheckShardVersion",
                "doesNotSendShardVersionIfTracked",
                "command",
                "conditional"
            ].includes(key),
            "Found unexpected field " + key + " in test case " + tojson(testCase));
    }

    assert(testCase.skip || testCase.command,
           "Must specify exactly one of 'skip' or 'command' for test case " + tojson(testCase));

    if (testCase.skip) {
        for (let key of Object.keys(testCase)) {
            assert(
                key === "skip" || key === "conditional",
                "if a test case specifies 'skip', it must not specify any other fields besides 'conditional': " +
                    key + ": " + tojson(testCase));
        }
        return;
    }
}

function expectShardsCachedShardVersionToBe(shardConn, ns, expectedShardVersion) {
    const shardVersion = assert.commandWorked(st.shard0.adminCommand({getShardVersion: ns})).global;
    assert.eq(expectedShardVersion, shardVersion);
}

let testCases = {
    _hashBSONElement: {skip: "executes locally on mongos (not sent to any remote node)"},
    _isSelf: {skip: "executes locally on mongos (not sent to any remote node)"},
    _mergeAuthzCollections: {skip: "always targets the config server"},
    abortTransaction: {skip: "unversioned and uses special targetting rules"},
    addShard: {skip: "not on a user database"},
    addShardToZone: {skip: "not on a user database"},
    aggregate: {
        command: collName => {
            return {aggregate: collName, pipeline: [{$match: {x: 1}}], cursor: {batchSize: 10}};
        },
    },
    authenticate: {skip: "does not forward command to primary shard"},
    availableQueryOptions: {skip: "executes locally on mongos (not sent to any remote node)"},
    balancerStart: {skip: "not on a user database"},
    balancerStatus: {skip: "not on a user database"},
    balancerStop: {skip: "not on a user database"},
    buildInfo: {skip: "executes locally on mongos (not sent to any remote node)"},
    clearJumboFlag: {skip: "does not forward command to primary shard"},
    clearLog: {skip: "executes locally on mongos (not sent to any remote node)"},
    collMod: {
        whenNamespaceDoesNotExistFailsWith: ErrorCodes.NamespaceNotFound,
        doesNotSendShardVersionIfTracked: true,
        command: collName => {
            return {collMod: collName};
        },
    },
    collStats: {
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {collStats: collName};
        },
    },
    commitTransaction: {skip: "unversioned and uses special targetting rules"},
    compact: {skip: "not allowed through mongos"},
    configureFailPoint: {skip: "executes locally on mongos (not sent to any remote node)"},
    connPoolStats: {skip: "executes locally on mongos (not sent to any remote node)"},
    connPoolSync: {skip: "executes locally on mongos (not sent to any remote node)"},
    connectionStatus: {skip: "executes locally on mongos (not sent to any remote node)"},
    convertToCapped: {skip: "will be removed in 4.4"},
    count: {
        command: collName => {
            return {count: collName, query: {x: 1}};
        },
    },
    create: {skip: "always targets the config server"},
    createIndexes: {
        implicitlyCreatesCollection: true,
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        doesNotCheckShardVersion: true,
        doesNotSendShardVersionIfTracked: true,
        command: collName => {
            return {createIndexes: collName, indexes: [{key: {a: 1}, name: "index"}]};
        },
    },
    createRole: {skip: "always targets the config server"},
    createUser: {skip: "always targets the config server"},
    currentOp: {skip: "not on a user database"},
    dataSize: {
        skip: "TODO (SERVER-42638): fails due to special checks on mongos if chunk manager exists",
    },
    dbStats: {skip: "always broadcast to all shards"},
    delete: {
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]};
        },
    },
    distinct: {
        command: collName => {
            return {distinct: collName, key: "x"};
        },
    },
    drop: {skip: "always targets the config server"},
    dropAllRolesFromDatabase: {skip: "always targets the config server"},
    dropAllUsersFromDatabase: {skip: "always targets the config server"},
    dropConnections: {skip: "not on a user database"},
    dropDatabase: {skip: "drops the database from the cluster, changing the UUID"},
    dropIndexes: {
        whenNamespaceDoesNotExistFailsWith: ErrorCodes.NamespaceNotFound,
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        doesNotSendShardVersionIfTracked: true,
        command: collName => {
            return {dropIndexes: collName, index: "*"};
        },
    },
    dropRole: {skip: "always targets the config server"},
    dropUser: {skip: "always targets the config server"},
    echo: {skip: "does not forward command to primary shard"},
    enableSharding: {skip: "does not forward command to primary shard"},
    endSessions: {skip: "goes through the cluster write path"},
    explain: {skip: "TODO SERVER-31226"},
    features: {skip: "executes locally on mongos (not sent to any remote node)"},
    filemd5: {
        doesNotCheckShardVersion: true,
        command: collName => {
            return {filemd5: ObjectId(), root: collName};
        },
    },
    find: {
        command: collName => {
            return {find: collName, filter: {x: 1}};
        },
    },
    findAndModify: {
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {findAndModify: collName, query: {_id: 0}, remove: true};
        },
    },
    flushRouterConfig: {skip: "executes locally on mongos (not sent to any remote node)"},
    fsync: {skip: "broadcast to all shards"},
    getCmdLineOpts: {skip: "executes locally on mongos (not sent to any remote node)"},
    getDefaultRWConcern: {skip: "executes locally on mongos (not sent to any remote node)"},
    getDiagnosticData: {skip: "executes locally on mongos (not sent to any remote node)"},
    getLastError: {skip: "does not forward command to primary shard"},
    getLog: {skip: "executes locally on mongos (not sent to any remote node)"},
    getMore: {skip: "requires a previously established cursor"},
    getParameter: {skip: "executes locally on mongos (not sent to any remote node)"},
    getShardMap: {skip: "executes locally on mongos (not sent to any remote node)"},
    getShardVersion: {skip: "executes locally on mongos (not sent to any remote node)"},
    getnonce: {skip: "not on a user database"},
    grantPrivilegesToRole: {skip: "always targets the config server"},
    grantRolesToRole: {skip: "always targets the config server"},
    grantRolesToUser: {skip: "always targets the config server"},
    hostInfo: {skip: "executes locally on mongos (not sent to any remote node)"},
    insert: {
        implicitlyCreatesCollection: true,
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {insert: collName, documents: [{_id: 1}]};
        },
    },
    invalidateUserCache: {skip: "executes locally on mongos (not sent to any remote node)"},
    isdbgrid: {skip: "executes locally on mongos (not sent to any remote node)"},
    isMaster: {skip: "executes locally on mongos (not sent to any remote node)"},
    killCursors: {skip: "requires a previously established cursor"},
    killAllSessions: {skip: "always broadcast to all hosts in the cluster"},
    killAllSessionsByPattern: {skip: "always broadcast to all hosts in the cluster"},
    killOp: {skip: "does not forward command to primary shard"},
    killSessions: {skip: "always broadcast to all hosts in the cluster"},
    listCollections: {skip: "not a command on a namespace"},
    listCommands: {skip: "executes locally on mongos (not sent to any remote node)"},
    listDatabases: {skip: "does not forward command to primary shard"},
    listIndexes: {
        whenNamespaceDoesNotExistFailsWith: ErrorCodes.NamespaceNotFound,
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        doesNotCheckShardVersion: true,
        command: collName => {
            return {listIndexes: collName};
        },
    },
    listShards: {skip: "does not forward command to primary shard"},
    logApplicationMessage: {skip: "not on a user database", conditional: true},
    logRotate: {skip: "executes locally on mongos (not sent to any remote node)"},
    logout: {skip: "not on a user database"},
    mapReduce: {
        // Uses connection versioning.
        whenNamespaceDoesNotExistFailsWith: ErrorCodes.NamespaceNotFound,
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {
                mapReduce: collName,
                map: function() {
                    emit(this.x, 1);
                },
                reduce: function(key, values) {
                    return Array.sum(values);
                },
                out: {inline: 1}
            };
        },
    },
    mergeChunks: {skip: "does not forward command to primary shard"},
    moveChunk: {skip: "does not forward command to primary shard"},
    movePrimary: {skip: "reads primary shard from sharding catalog with readConcern: local"},
    multicast: {skip: "does not forward command to primary shard"},
    netstat: {skip: "executes locally on mongos (not sent to any remote node)"},
    ping: {skip: "executes locally on mongos (not sent to any remote node)"},
    planCacheClear: {
        // Uses connection versioning.
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {planCacheClear: collName};
        },
    },
    planCacheClearFilters: {
        // Uses connection versioning.
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {planCacheClearFilters: collName};
        },
    },
    planCacheListFilters: {
        // Uses connection versioning.
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {planCacheListFilters: collName};
        },
    },
    planCacheListPlans: {
        // Uses connection versioning.
        whenNamespaceDoesNotExistFailsWith: ErrorCodes.BadValue,
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {planCacheListPlans: collName, query: {_id: "A"}};
        },
    },
    planCacheListQueryShapes: {
        // Uses connection versioning.
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {planCacheListQueryShapes: collName};
        },
    },
    planCacheSetFilter: {
        // Uses connection versioning.
        whenNamespaceDoesNotExistFailsWith: ErrorCodes.BadValue,
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {planCacheSetFilter: collName, query: {_id: "A"}, indexes: [{_id: 1}]};
        },
    },
    profile: {skip: "not supported in mongos"},
    reapLogicalSessionCacheNow: {skip: "is a no-op on mongos"},
    refineCollectionShardKey: {skip: "not on a user database"},
    refreshLogicalSessionCacheNow: {skip: "goes through the cluster write path"},
    refreshSessions: {skip: "executes locally on mongos (not sent to any remote node)"},
    refreshSessionsInternal:
        {skip: "executes locally on mongos (not sent to any remote node)", conditional: true},
    removeShard: {skip: "not on a user database"},
    removeShardFromZone: {skip: "not on a user database"},
    renameCollection: {skip: "always targets the config server"},
    replSetGetStatus: {skip: "not supported in mongos"},
    resetError: {skip: "not on a user database"},
    restartCatalog: {skip: "not on a user database"},
    revokePrivilegesFromRole: {skip: "always targets the config server"},
    revokeRolesFromRole: {skip: "always targets the config server"},
    revokeRolesFromUser: {skip: "always targets the config server"},
    rolesInfo: {skip: "always targets the config server"},
    saslContinue: {skip: "not on a user database"},
    saslStart: {skip: "not on a user database"},
    serverStatus: {skip: "executes locally on mongos (not sent to any remote node)"},
    setDefaultRWConcern: {skip: "always targets the config server"},
    setIndexCommitQuorum: {skip: "TODO what is this"},
    setFeatureCompatibilityVersion: {skip: "not on a user database"},
    setFreeMonitoring:
        {skip: "explicitly fails for mongos, primary mongod only", conditional: true},
    setParameter: {skip: "executes locally on mongos (not sent to any remote node)"},
    shardCollection: {skip: "does not forward command to primary shard"},
    shardConnPoolStats: {skip: "does not forward command to primary shard"},
    shutdown: {skip: "does not forward command to primary shard"},
    split: {skip: "does not forward command to primary shard"},
    splitVector: {skip: "does not forward command to primary shard"},
    startRecordingTraffic: {skip: "executes locally on mongos (not sent to any remote node)"},
    startSession: {skip: "executes locally on mongos (not sent to any remote node)"},
    stopRecordingTraffic: {skip: "executes locally on mongos (not sent to any remote node)"},
    update: {
        implicitlyCreatesCollection: true,
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        command: collName => {
            return {
                update: collName,
                updates: [{q: {_id: 2}, u: {_id: 2}, upsert: true, multi: false}]
            };
        },
    },
    updateRole: {skip: "always targets the config server"},
    updateUser: {skip: "always targets the config server"},
    updateZoneKeyRange: {skip: "not on a user database"},
    usersInfo: {skip: "always targets the config server"},
    validate: {
        whenNamespaceDoesNotExistFailsWith: ErrorCodes.NamespaceNotFound,
        whenNamespaceIsViewFailsWith: ErrorCodes.CommandNotSupportedOnView,
        doesNotCheckShardVersion: true,
        command: collName => {
            return {validate: collName};
        },
    },
    waitForFailPoint: {skip: "executes locally on mongos (not sent to any remote node)"},
    whatsmyuri: {skip: "executes locally on mongos (not sent to any remote node)"},
};

commandsRemovedFromMongosIn44.forEach(function(cmd) {
    testCases[cmd] = {skip: "must define test coverage for 4.0 backwards compatibility"};
});

const st = new ShardingTest({
    shards: 1,
    config: 1,
    other: {
        configOptions: {
            setParameter:
                {"failpoint.writeUnshardedCollectionsToShardingCatalog": "{mode: 'alwaysOn'}"},
        },
        shardOptions: {
            setParameter: {"failpoint.useFCV44CheckShardVersionProtocol": "{mode: 'alwaysOn'}"},
        },
    },
});

assert.commandWorked(st.s.getDB(dbName).runCommand({create: "underlying_collection_for_views"}));

let res = st.s.adminCommand({listCommands: 1});
assert.commandWorked(res);

for (let command of Object.keys(res.commands)) {
    let testCase = testCases[command];
    assert(testCase !== undefined, "coverage failure: must define a test case for " + command);
    if (!testCases[command].validated) {
        validateTestCase(testCase);
        testCases[command].validated = true;
    }

    if (testCase.skip) {
        print("skipping " + command + ": " + testCase.skip);
        continue;
    }

    //
    // Test command when namespace does not exist.
    //

    [collName, ns] = getNewNs(dbName);
    jsTest.log(`Testing ${command} when namespace does not exist; ns: ${ns}`);

    expectShardsCachedShardVersionToBe(st.shard0, ns, "UNKNOWN");

    if (testCase.whenNamespaceDoesNotExistFailsWith) {
        assert.commandFailedWithCode(st.s.getDB(dbName).runCommand(testCase.command(collName)),
                                     testCase.whenNamespaceDoesNotExistFailsWith);
    } else {
        assert.commandWorked(st.s.getDB(dbName).runCommand(testCase.command(collName)));
    }

    if (testCase.implicitlyCreatesCollection &&
        !(testCase.doesNotCheckShardVersion || testCase.doesNotSendShardVersionIfTracked)) {
        expectShardsCachedShardVersionToBe(st.shard0, ns, Timestamp(1, 0));
    } else {
        expectShardsCachedShardVersionToBe(st.shard0, ns, "UNKNOWN");
    }

    //
    // Test command when namespace is a view.
    //

    [collName, ns] = getNewNs(dbName);
    jsTest.log(`Testing ${command} when namespace is a view; ns: ${ns}`);

    assert.commandWorked(st.s.getDB(dbName).runCommand({
        create: collName,
        viewOn: "underlying_collection_for_views",
        pipeline: [{$project: {_id: 1}}]
    }));
    expectShardsCachedShardVersionToBe(st.shard0, ns, "UNKNOWN");

    if (testCase.whenNamespaceIsViewFailsWith) {
        assert.commandFailedWithCode(st.s.getDB(dbName).runCommand(testCase.command(collName)),
                                     testCase.whenNamespaceIsViewFailsWith);
    } else {
        assert.commandWorked(st.s.getDB(dbName).runCommand(testCase.command(collName)));
    }

    expectShardsCachedShardVersionToBe(st.shard0, ns, "UNKNOWN");

    //
    // Test command when namespace is an unsharded collection and mongos sends UNSHARDED version.
    //

    [collName, ns] = getNewNs(dbName);
    jsTest.log(`Testing ${command} when namespace is an unsharded collection and mongos sends
                UNSHARDED version; ns: ${ns}`);

    assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));
    expectShardsCachedShardVersionToBe(st.shard0, ns, "UNKNOWN");

    assert.commandWorked(st.s.getDB(dbName).runCommand(testCase.command(collName)));

    if (testCase.doesNotCheckShardVersion) {
        expectShardsCachedShardVersionToBe(st.shard0, ns, "UNKNOWN");
    } else {
        expectShardsCachedShardVersionToBe(st.shard0, ns, Timestamp(1, 0));
    }

    //
    // Test command when namespace is an unsharded collection and mongos sends a real version.
    //

    [collName, ns] = getNewNs(dbName);
    jsTest.log(`Testing ${command} when namespace is an unsharded collection and mongos sends a real
                version; ns: ${ns}`);

    assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));
    expectShardsCachedShardVersionToBe(st.shard0, ns, "UNKNOWN");

    // Flushing the router's config ensures the router will load the db and all collections, so the
    // router will send a real version on the first request.
    assert.commandWorked(st.s.adminCommand({flushRouterConfig: dbName}));
    assert.commandWorked(st.s.getDB(dbName).runCommand(testCase.command(collName)));

    if (testCase.doesNotCheckShardVersion || testCase.doesNotSendShardVersionIfTracked) {
        expectShardsCachedShardVersionToBe(st.shard0, ns, "UNKNOWN");
    } else {
        expectShardsCachedShardVersionToBe(st.shard0, ns, Timestamp(1, 0));
    }
}

// After iterating through all the existing commands, ensure there were no additional test cases
// that did not correspond to any mongos command.
for (let key of Object.keys(testCases)) {
    // We have defined real test cases for commands added in 4.2/4.4 so that the test cases are
    // exercised in the regular suites, but because these test cases can't run in the last stable
    // suite, we skip processing them here to avoid failing the below assertion. We have defined
    // "skip" test cases for commands removed in 4.2 so the test case is defined in last stable
    // suites (in which these commands still exist on the mongos), but these test cases won't be run
    // in regular suites, so we skip processing them below as well.
    if (commandsAddedToMongosIn44.includes(key) || commandsRemovedFromMongosIn44.includes(key)) {
        continue;
    }
    if (commandsAddedToMongosIn44.includes(key)) {
        continue;
    }
    assert(testCases[key].validated || testCases[key].conditional,
           "you defined a test case for a command '" + key +
               "' that does not exist on mongos: " + tojson(testCases[key]));
}

st.stop();
})();
