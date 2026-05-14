/**
 * Tests all mongoS commands against a standby cluster in config-only mode.
 * Asserts that exactly the commands marked `standbyAllowed: true` succeed; any
 * drift in either direction (an allowed command failing, or a non-allowed one
 * succeeding) fails the test so the allowlist stays in sync with mongoS behavior
 * in standby mode.
 *
 * @tags: [
 *   requires_sharding,
 *   # The StandbyClusterTestFixture restarts nodes and expects persisted metadata to be preserved.
 *   requires_persistence,
 *   # Many commands executed via mongos experience timeouts on slow/resource-constrained variants.
 *   resource_intensive,
 * ]
 */

import {StandbyClusterTestFixture} from "jstests/noPassthrough/libs/sharded_cluster_topology/standby_cluster_test_fixture.js";

const dbName = "testDb";
const collName = "testColl";
const fullNs = dbName + "." + collName;

// Skip reasons.
const isAnInternalCommand = "internal command";
const requiresParallelShell = "requires parallel shell";
const requiresCursor = "requires a pre-existing cursor";

/**
 * Map of all mongoS commands. Each entry has:
 *   command        - the command object to send (omit if skip is set)
 *   isAdminCommand - run against the admin db instead of dbName
 *   skip           - string reason to skip without running
 *   standbyAllowed - true if this command must succeed against a config-only
 *                    mongoS pointed at a standby config server. Mutually
 *                    exclusive with skip.
 */
const allCommands = {
    _clusterQueryWithoutShardKey: {skip: isAnInternalCommand},
    _clusterWriteWithoutShardKey: {skip: isAnInternalCommand},
    _dropConnectionsToMongot: {skip: isAnInternalCommand},
    _flushShardRegistry: {skip: isAnInternalCommand},
    _hashBSONElement: {skip: isAnInternalCommand},
    _isSelf: {skip: isAnInternalCommand},
    _killOperations: {skip: isAnInternalCommand},
    _mergeAuthzCollections: {skip: isAnInternalCommand},
    _mongotConnPoolStats: {skip: isAnInternalCommand},
    abortMoveCollection: {skip: requiresParallelShell},
    abortReshardCollection: {skip: requiresParallelShell},
    abortRewriteCollection: {skip: requiresParallelShell},
    abortTransaction: {skip: requiresParallelShell},
    abortUnshardCollection: {skip: requiresParallelShell},
    addShard: {command: {addShard: "rs0/localhost:27017"}, isAdminCommand: true},
    addShardToZone: {
        command: {addShardToZone: "shard0", zone: "zone1"},
        isAdminCommand: true,
    },
    aggregate: {command: {aggregate: collName, pipeline: [{$match: {}}], cursor: {}}},
    analyze: {command: {analyze: collName}},
    analyzeShardKey: {command: {analyzeShardKey: fullNs, key: {x: 1}}, isAdminCommand: true},
    appendOplogNote: {
        command: {appendOplogNote: 1, data: {a: 1}},
        isAdminCommand: true,
    },
    authenticate: {skip: "tested separately in auth tests"},
    autoSplitVector: {
        command: {
            autoSplitVector: collName,
            keyPattern: {x: 1},
            min: {x: MinKey},
            max: {x: MaxKey},
            maxChunkSizeBytes: 1024 * 1024,
        },
    },
    balancerCollectionStatus: {
        command: {balancerCollectionStatus: fullNs},
        isAdminCommand: true,
    },
    balancerStart: {command: {balancerStart: 1}, isAdminCommand: true},
    balancerStatus: {command: {balancerStatus: 1}, isAdminCommand: true},
    balancerStop: {command: {balancerStop: 1}, isAdminCommand: true},
    buildInfo: {command: {buildInfo: 1}, isAdminCommand: true, standbyAllowed: true},
    bulkWrite: {
        command: {
            bulkWrite: 1,
            ops: [{insert: 0, document: {skey: "MongoDB"}}],
            nsInfo: [{ns: fullNs}],
        },
        isAdminCommand: true,
    },
    checkMetadataConsistency: {command: {checkMetadataConsistency: 1}, isAdminCommand: true},
    cleanupStructuredEncryptionData: {skip: "requires encrypted collection setup"},
    clearJumboFlag: {
        command: {clearJumboFlag: fullNs, bounds: [{x: MinKey}, {x: MaxKey}]},
        isAdminCommand: true,
    },
    clearLog: {command: {clearLog: "global"}, isAdminCommand: true, standbyAllowed: true},
    collMod: {command: {collMod: collName, validator: {}}},
    collStats: {command: {aggregate: collName, pipeline: [{$collStats: {count: {}}}], cursor: {}}},
    commitReshardCollection: {skip: requiresParallelShell},
    commitShardRemoval: {command: {commitShardRemoval: "shard0"}, isAdminCommand: true},
    commitTransaction: {skip: requiresParallelShell},
    commitTransitionToDedicatedConfigServer: {
        command: {commitTransitionToDedicatedConfigServer: 1},
        isAdminCommand: true,
    },
    compact: {command: {compact: collName, force: true}},
    compactStructuredEncryptionData: {skip: "requires encrypted collection setup"},
    configureFailPoint: {skip: isAnInternalCommand},
    configureCollectionBalancing: {
        command: {configureCollectionBalancing: fullNs},
        isAdminCommand: true,
    },
    configureQueryAnalyzer: {
        command: {configureQueryAnalyzer: fullNs, mode: "off"},
        isAdminCommand: true,
    },
    connPoolStats: {command: {connPoolStats: 1}, isAdminCommand: true, standbyAllowed: true},
    connPoolSync: {command: {connPoolSync: 1}, isAdminCommand: true, standbyAllowed: true},
    connectionStatus: {command: {connectionStatus: 1}, isAdminCommand: true, standbyAllowed: true},
    convertToCapped: {
        command: {convertToCapped: collName, size: 10 * 1024 * 1024},
    },
    coordinateCommitTransaction: {skip: isAnInternalCommand},
    count: {command: {count: collName}},
    cpuload: {skip: isAnInternalCommand},
    create: {command: {create: "newCollection"}},
    createIndexes: {
        command: {createIndexes: collName, indexes: [{key: {x: 1}, name: "x_1"}]},
    },
    createRole: {command: {createRole: "testRole", privileges: [], roles: []}},
    createSearchIndexes: {skip: "requires mongot mock setup"},
    createUnsplittableCollection: {skip: isAnInternalCommand},
    createUser: {command: {createUser: "testUser", pwd: "testPwd", roles: []}},
    currentOp: {command: {currentOp: 1}, isAdminCommand: true},
    dataSize: {command: {dataSize: fullNs}},
    dbStats: {command: {dbStats: 1}},
    delete: {command: {delete: collName, deletes: [{q: {x: 999}, limit: 1}]}},
    distinct: {command: {distinct: collName, key: "x"}},
    drop: {command: {drop: "nonExistentCollForDrop"}},
    dropAllRolesFromDatabase: {command: {dropAllRolesFromDatabase: 1}},
    dropAllUsersFromDatabase: {command: {dropAllUsersFromDatabase: 1}},
    dropConnections: {skip: "requires additional reconfig setup"},
    dropDatabase: {command: {dropDatabase: 1}},
    dropIndexes: {command: {dropIndexes: collName, index: {x: 1}}},
    dropRole: {skip: "no role to drop without prior createRole succeeding"},
    dropSearchIndex: {skip: "requires mongot mock setup"},
    dropUser: {skip: "no user to drop without prior createUser succeeding"},
    echo: {command: {echo: 1}, isAdminCommand: true, standbyAllowed: true},
    enableSharding: {
        command: {enableSharding: "newShardedDb"},
        isAdminCommand: true,
    },
    endSessions: {
        command: {endSessions: []},
        isAdminCommand: true,
        standbyAllowed: true,
    },
    explain: {command: {explain: {count: collName}}},
    features: {command: {features: 1}, isAdminCommand: true, standbyAllowed: true},
    filemd5: {command: {filemd5: ObjectId(), root: "fs"}},
    find: {command: {find: collName, filter: {x: 1}}},
    findAndModify: {
        command: {findAndModify: collName, query: {x: 1}, update: {$set: {x: 99}}},
    },
    flushRouterConfig: {command: {flushRouterConfig: 1}, isAdminCommand: true, standbyAllowed: true},
    fsync: {command: {fsync: 1}, isAdminCommand: true},
    fsyncUnlock: {command: {fsyncUnlock: 1}, isAdminCommand: true},
    getAuditConfig: {command: {getAuditConfig: 1}, isAdminCommand: true},
    getClusterParameter: {
        command: {getClusterParameter: "changeStreamOptions"},
        isAdminCommand: true,
    },
    getCmdLineOpts: {command: {getCmdLineOpts: 1}, isAdminCommand: true, standbyAllowed: true},
    getDatabaseVersion: {command: {getDatabaseVersion: dbName}, isAdminCommand: true},
    getDefaultRWConcern: {command: {getDefaultRWConcern: 1}, isAdminCommand: true},
    getDiagnosticData: {command: {getDiagnosticData: 1}, isAdminCommand: true, standbyAllowed: true},
    getLog: {command: {getLog: "global"}, isAdminCommand: true, standbyAllowed: true},
    getMore: {skip: requiresCursor},
    getParameter: {command: {getParameter: 1, logLevel: 1}, isAdminCommand: true, standbyAllowed: true},
    getQueryableEncryptionCountInfo: {skip: isAnInternalCommand},
    getShardMap: {command: {getShardMap: 1}, isAdminCommand: true, standbyAllowed: true},
    getShardVersion: {command: {getShardVersion: fullNs}, isAdminCommand: true},
    getTrafficRecordingStatus: {command: {getTrafficRecordingStatus: 1}, isAdminCommand: true, standbyAllowed: true},
    getTransitionToDedicatedConfigServerStatus: {
        command: {getTransitionToDedicatedConfigServerStatus: 1},
        isAdminCommand: true,
    },
    grantPrivilegesToRole: {skip: "no role to grant to without prior createRole succeeding"},
    grantRolesToRole: {skip: "no role to modify without prior createRole succeeding"},
    grantRolesToUser: {skip: "no user to modify without prior createUser succeeding"},
    hello: {command: {hello: 1}, isAdminCommand: true, standbyAllowed: true},
    hostInfo: {command: {hostInfo: 1}, isAdminCommand: true, standbyAllowed: true},
    insert: {command: {insert: collName, documents: [{_id: ObjectId()}]}},
    invalidateUserCache: {command: {invalidateUserCache: 1}, isAdminCommand: true, standbyAllowed: true},
    isdbgrid: {command: {isdbgrid: 1}, isAdminCommand: true, standbyAllowed: true},
    isMaster: {command: {isMaster: 1}, isAdminCommand: true, standbyAllowed: true},
    killAllSessions: {command: {killAllSessions: []}, isAdminCommand: true},
    killAllSessionsByPattern: {command: {killAllSessionsByPattern: []}, isAdminCommand: true},
    killCursors: {skip: requiresCursor},
    killOp: {skip: requiresParallelShell},
    killSessions: {skip: requiresParallelShell},
    listCollections: {command: {listCollections: 1}},
    listCommands: {command: {listCommands: 1}, isAdminCommand: true, standbyAllowed: true},
    listDatabases: {command: {listDatabases: 1, nameOnly: true}, isAdminCommand: true, standbyAllowed: true},
    listIndexes: {command: {listIndexes: collName}},
    listSearchIndexes: {skip: "requires mongot mock setup"},
    listShards: {command: {listShards: 1}, isAdminCommand: true},
    lockInfo: {command: {lockInfo: 1}, isAdminCommand: true},
    logApplicationMessage: {
        command: {logApplicationMessage: "standby test message"},
        isAdminCommand: true,
        standbyAllowed: true,
    },
    logMessage: {skip: isAnInternalCommand},
    logRotate: {command: {logRotate: 1}, isAdminCommand: true, standbyAllowed: true},
    logout: {skip: "requires additional auth setup"},
    mapReduce: {
        command: {
            mapReduce: collName,
            map: function () {},
            reduce: function (key, vals) {},
            out: {inline: 1},
        },
    },
    mergeAllChunksOnShard: {
        command: {mergeAllChunksOnShard: fullNs, shard: "shard0"},
        isAdminCommand: true,
    },
    mergeChunks: {
        command: {mergeChunks: fullNs, bounds: [{x: MinKey}, {x: MaxKey}]},
        isAdminCommand: true,
    },
    moveChunk: {
        command: {
            moveChunk: fullNs,
            find: {x: 1},
            to: "shard0",
        },
        isAdminCommand: true,
    },
    moveCollection: {
        command: {moveCollection: fullNs, toShard: "shard0"},
        isAdminCommand: true,
    },
    movePrimary: {command: {movePrimary: dbName, to: "shard0"}, isAdminCommand: true},
    moveRange: {
        command: {moveRange: fullNs, min: {x: MinKey}, toShard: "shard0"},
        isAdminCommand: true,
    },
    multicast: {skip: isAnInternalCommand},
    netstat: {skip: isAnInternalCommand},
    oidcListKeys: {skip: "requires OIDC/OpenSSL setup"},
    oidcRefreshKeys: {skip: "requires OIDC/OpenSSL setup"},
    ping: {command: {ping: 1}, isAdminCommand: true, standbyAllowed: true},
    planCacheClear: {command: {planCacheClear: collName}},
    planCacheClearFilters: {command: {planCacheClearFilters: collName}},
    planCacheListFilters: {command: {planCacheListFilters: collName}},
    planCacheSetFilter: {
        command: {planCacheSetFilter: collName, query: {_id: "A"}, indexes: [{_id: 1}]},
    },
    profile: {command: {profile: 0}, isAdminCommand: true, standbyAllowed: true},
    reapLogicalSessionCacheNow: {
        command: {reapLogicalSessionCacheNow: 1},
        isAdminCommand: true,
        standbyAllowed: true,
    },
    recreateRangeDeletionTasks: {
        command: {recreateRangeDeletionTasks: fullNs},
        isAdminCommand: true,
    },
    refineCollectionShardKey: {
        command: {refineCollectionShardKey: fullNs, key: {x: 1, _id: 1}},
        isAdminCommand: true,
    },
    refreshLogicalSessionCacheNow: {
        command: {refreshLogicalSessionCacheNow: 1},
        isAdminCommand: true,
    },
    refreshSessions: {command: {refreshSessions: []}, isAdminCommand: true, standbyAllowed: true},
    releaseMemory: {skip: requiresCursor},
    removeQuerySettings: {skip: "requires specific query settings setup"},
    removeShard: {command: {removeShard: "shard0"}, isAdminCommand: true},
    removeShardFromZone: {
        command: {removeShardFromZone: "shard0", zone: "zone1"},
        isAdminCommand: true,
    },
    renameCollection: {
        command: {renameCollection: fullNs, to: dbName + ".renamedColl"},
        isAdminCommand: true,
    },
    replicateSearchIndexCommand: {skip: isAnInternalCommand},
    replSetGetStatus: {skip: "mongos does not run replset commands"},
    resetPlacementHistory: {command: {resetPlacementHistory: 1}, isAdminCommand: true},
    reshardCollection: {
        command: {reshardCollection: fullNs, key: {_id: 1}},
        isAdminCommand: true,
    },
    rewriteCollection: {command: {rewriteCollection: fullNs}, isAdminCommand: true},
    revokePrivilegesFromRole: {skip: "no role to revoke from without prior createRole succeeding"},
    revokeRolesFromRole: {skip: "no role to modify without prior createRole succeeding"},
    revokeRolesFromUser: {skip: "no user to modify without prior createUser succeeding"},
    rolesInfo: {command: {rolesInfo: 1}, standbyAllowed: true},
    rotateCertificates: {skip: "requires additional authentication setup"},
    rotateFTDC: {command: {rotateFTDC: 1}, isAdminCommand: true, standbyAllowed: true},
    saslContinue: {skip: "requires authentication setup"},
    saslStart: {skip: "requires authentication setup"},
    serverStatus: {command: {serverStatus: 1}, isAdminCommand: true, standbyAllowed: true},
    setAuditConfig: {skip: "requires additional audit setup"},
    setAllowMigrations: {
        command: {setAllowMigrations: fullNs, allowMigrations: true},
        isAdminCommand: true,
    },
    setDefaultRWConcern: {
        command: {
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: 1},
            writeConcern: {w: "majority"},
        },
        isAdminCommand: true,
    },
    setIndexCommitQuorum: {skip: requiresParallelShell},
    setFeatureCompatibilityVersion: {
        command: {setFeatureCompatibilityVersion: latestFCV, confirm: true},
        isAdminCommand: true,
    },
    setProfilingFilterGlobally: {skip: "requires special startup parameter"},
    setParameter: {command: {setParameter: 1, quiet: 1}, isAdminCommand: true, standbyAllowed: true},
    setClusterParameter: {skip: "requires specific cluster parameter setup"},
    setQuerySettings: {skip: "requires specific query settings setup"},
    setUserWriteBlockMode: {
        command: {setUserWriteBlockMode: 1, global: false},
        isAdminCommand: true,
    },
    shardCollection: {
        command: {shardCollection: dbName + ".anotherColl", key: {_id: 1}},
        isAdminCommand: true,
    },
    shardDrainingStatus: {command: {shardDrainingStatus: 1}, isAdminCommand: true},
    shutdown: {skip: "would terminate the server"},
    split: {
        command: {split: fullNs, find: {x: 1}},
        isAdminCommand: true,
    },
    splitVector: {skip: isAnInternalCommand},
    startShardDraining: {command: {startShardDraining: "shard0"}, isAdminCommand: true},
    startTrafficRecording: {skip: "requires a file path to record traffic to"},
    startTransitionToDedicatedConfigServer: {
        command: {startTransitionToDedicatedConfigServer: 1},
        isAdminCommand: true,
    },
    startSession: {command: {startSession: 1}, isAdminCommand: true, standbyAllowed: true},
    stopShardDraining: {command: {stopShardDraining: "shard0"}, isAdminCommand: true},
    stopTrafficRecording: {skip: "requires a file path to record traffic to"},
    stopTransitionToDedicatedConfigServer: {
        command: {stopTransitionToDedicatedConfigServer: 1},
        isAdminCommand: true,
    },
    testDeprecation: {skip: isAnInternalCommand},
    testDeprecationInVersion2: {skip: isAnInternalCommand},
    testInternalTransactions: {skip: isAnInternalCommand},
    testRemoval: {skip: isAnInternalCommand},
    testVersions1And2: {skip: isAnInternalCommand},
    testVersion2: {skip: isAnInternalCommand},
    upgradeDowngradeViewlessTimeseries: {skip: isAnInternalCommand},
    transitionFromDedicatedConfigServer: {
        command: {transitionFromDedicatedConfigServer: 1},
        isAdminCommand: true,
    },
    transitionToDedicatedConfigServer: {
        command: {transitionToDedicatedConfigServer: 1},
        isAdminCommand: true,
    },
    unshardCollection: {
        command: {unshardCollection: fullNs, toShard: "shard0"},
        isAdminCommand: true,
    },
    untrackUnshardedCollection: {
        command: {untrackUnshardedCollection: fullNs},
        isAdminCommand: true,
    },
    update: {command: {update: collName, updates: [{q: {x: 999}, u: {x: 1000}}]}},
    updateRole: {skip: "no role to update without prior createRole succeeding"},
    updateSearchIndex: {skip: "requires mongot mock setup"},
    updateUser: {skip: "no user to update without prior createUser succeeding"},
    updateZoneKeyRange: {
        command: {
            updateZoneKeyRange: fullNs,
            min: {x: MinKey},
            max: {x: MaxKey},
            zone: null,
        },
        isAdminCommand: true,
    },
    usersInfo: {command: {usersInfo: 1}, standbyAllowed: true},
    validate: {command: {validate: collName}},
    validateDBMetadata: {
        command: {validateDBMetadata: 1, apiParameters: {version: "1", strict: true}},
    },
    waitForFailPoint: {skip: isAnInternalCommand},
    whatsmyuri: {command: {whatsmyuri: 1}, isAdminCommand: true, standbyAllowed: true},
};

// ---------------------------------------------------------------------------
// Run the all-commands sweep against a standby cluster. Invoked once per
// topology (dedicated config server and embedded/configShard).
// ---------------------------------------------------------------------------

function runStandbyAllCommandsTest({configShard}) {
    jsTest.log.info(`Running standby all-commands test (configShard=${configShard})`);

    const fixtureOptions = {
        name: jsTestName(),
        shards: 1,
        configShard,
    };
    if (!configShard) {
        fixtureOptions.rs = {nodes: 1};
        fixtureOptions.config = 2;
    } else {
        fixtureOptions.rs = {nodes: 2};
    }
    const fixture = new StandbyClusterTestFixture(fixtureOptions);

    jsTest.log.info("Setting up initial data before standby transition");

    // Create testDb.testColl with some documents and an index.
    assert.commandWorked(
        fixture.st.s
            .getDB(dbName)
            .getCollection(collName)
            .insertMany([{x: 1}, {x: 2}, {x: 3}]),
    );
    assert.commandWorked(fixture.st.s.getDB(dbName).getCollection(collName).createIndex({x: 1}));

    // Create a sharded collection so that sharding metadata exists.
    assert.commandWorked(fixture.st.s.adminCommand({shardCollection: fullNs, key: {x: 1}}));

    // Create a second database.
    assert.commandWorked(fixture.st.s.getDB("otherDb").getCollection("otherColl").insertOne({y: 1}));

    jsTest.log.info("Transitioning to standby");
    fixture.transitionToStandby();

    // Start a mongoS against the standby config server in config-only mode.
    const mongos = MongoRunner.runMongos({
        configdb: fixture.standbyRS.getURL(),
        configOnly: "",
        setParameter: {defaultConfigCommandTimeoutMS: 2000},
    });
    assert.neq(null, mongos, "mongoS failed to start against standby config server");

    const listCommandsRes = assert.commandWorked(mongos.adminCommand({listCommands: 1}));
    const serverCommands = Object.keys(listCommandsRes.commands).sort();

    const unexpectedlySucceeded = [];
    const unexpectedlyFailed = [];
    const missing = [];

    for (const cmdName of serverCommands) {
        const test = allCommands[cmdName];

        if (!test) {
            missing.push(cmdName);
            continue;
        }

        if (test.skip !== undefined) {
            assert(!test.standbyAllowed, `'${cmdName}' has both skip and standbyAllowed set; pick one`);
            continue;
        }

        jsTest.log.info(`Testing ${cmdName}`);

        const db = test.isAdminCommand ? mongos.getDB("admin") : mongos.getDB(dbName);

        let ok;
        let detail;
        try {
            // maxTimeMS bounds commands that would otherwise wait the full server-selection
            // timeout looking for a primary; in standby clusters primary nodes are replaced
            // with injector nodes, so server selection never finds one.
            const res = db.runCommand({...test.command, maxTimeMS: 2000});
            ok = res.ok === 1;
            detail = ok ? "" : `[${res.code}] ${res.errmsg}`;
        } catch (e) {
            ok = false;
            detail = `[exception] ${e.message}`;
        }

        const expected = test.standbyAllowed === true;
        if (ok && !expected) {
            unexpectedlySucceeded.push(cmdName);
        } else if (!ok && expected) {
            unexpectedlyFailed.push(`${cmdName}: ${detail}`);
        }
    }

    const modeLabel = `[configShard=${configShard}]`;
    assert.eq(
        0,
        missing.length,
        `${modeLabel} Some commands returned by listCommands are missing from the allCommands map: ` +
            missing.join(", "),
    );
    assert.eq(
        0,
        unexpectedlyFailed.length,
        `${modeLabel} Commands marked standbyAllowed failed:\n  ` + unexpectedlyFailed.join("\n  "),
    );
    assert.eq(
        0,
        unexpectedlySucceeded.length,
        `${modeLabel} Commands succeeded but are not marked standbyAllowed (set the flag if intentional):\n  ` +
            unexpectedlySucceeded.join("\n  "),
    );

    MongoRunner.stopMongos(mongos);
    fixture.teardown();
}

runStandbyAllCommandsTest({configShard: false});
runStandbyAllCommandsTest({configShard: true});
