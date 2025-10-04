/*
 * Utilities for checking that mongos commands forward their API version parameters to config
 * servers and shards.
 */

import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardTransitionUtil} from "jstests/libs/shard_transition_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {setLogVerbosity} from "jstests/replsets/rslib.js";
import {
    commandsAddedToMongosSinceLastLTS,
    commandsRemovedFromMongosSinceLastLTS,
} from "jstests/sharding/libs/last_lts_mongos_commands.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";
import {flushRoutersAndRefreshShardMetadata} from "jstests/sharding/libs/sharded_transactions_helpers.js";

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

export let MongosAPIParametersUtil = (function () {
    function validateTestCase(testCase) {
        assert(
            testCase.skip || testCase.run,
            "must specify exactly one of 'skip' or 'run' for test case " + tojson(testCase),
        );

        if (testCase.skip) {
            for (let key of Object.keys(testCase)) {
                assert(
                    key === "commandName" || key === "skip" || key === "conditional",
                    `if a test case specifies 'skip', it must not specify any other fields besides` +
                        ` 'commandName' and 'conditional': ${key}: ${tojson(testCase)}`,
                );
            }
            return;
        }

        validateCommandTestCase(testCase.run);

        if (testCase.explain) {
            validateCommandTestCase(testCase.explain);
        }
    }

    function validateCommandTestCase(testCase) {
        assert(testCase.command, "must specify 'command' for test case " + tojson(testCase));

        // Check that required fields are present.
        assert(
            testCase.hasOwnProperty("inAPIVersion1"),
            "must specify 'inAPIVersion1' for test case " + tojson(testCase),
        );

        // Check that all present fields are of the correct type.
        assert(typeof testCase.command === "function");
        assert(typeof testCase.inAPIVersion1 === "boolean");

        for (const [propertyName, defaultValue] of [
            ["runsAgainstAdminDb", false],
            ["permittedInTxn", true],
            ["permittedOnShardedCollection", true],
            ["requiresShardedCollection", false],
            ["requiresCommittedReads", false],
            ["requiresCatalogShardEnabled", false],
        ]) {
            if (testCase.hasOwnProperty(propertyName)) {
                assert(
                    typeof testCase[propertyName] === "boolean",
                    `${propertyName} must be a boolean: ${tojson(testCase)}`,
                );
            } else {
                testCase[propertyName] = defaultValue;
            }
        }

        assert(testCase.shardCommandName ? typeof testCase.shardCommandName === "string" : true);
        assert(testCase.shardPrimary ? typeof testCase.shardPrimary === "function" : true);
        assert(testCase.configServerCommandName ? typeof testCase.configServerCommandName === "string" : true);
        assert(
            testCase.shardCommandName || testCase.configServerCommandName,
            "must specify shardCommandName and/or configServerCommandName: " + tojson(testCase),
        );
        assert(
            testCase.setUp ? typeof testCase.setUp === "function" : true,
            "setUp must be a function: " + tojson(testCase),
        );
        assert(
            testCase.cleanUp ? typeof testCase.cleanUp === "function" : true,
            "cleanUp must be a function: " + tojson(testCase),
        );
    }

    function awaitRemoveShard(shardName) {
        assert.commandWorked(st.startBalancer());
        st.awaitBalancerRound();
        removeShard(st, shardName);
        assert.commandWorked(st.stopBalancer());
    }

    function awaitTransitionToDedicatedConfigServer() {
        assert.commandWorked(st.startBalancer());
        st.awaitBalancerRound();
        ShardTransitionUtil.transitionToDedicatedConfigServer(st);
        assert.commandWorked(st.stopBalancer());
    }

    // Each test case is potentially run with any combination of API parameters, in
    // sharded/unsharded collection, inside or outside of a multi-document transaction. The "db"
    // database is dropped and recreated between test cases, so most tests don't need custom setUp
    // or cleanUp. Test cases are not 1-1 with commands, e.g. "count" has two cases.
    let testCasesFirstHalf = [
        {commandName: "_clusterQueryWithoutShardKey", skip: "internal API"},
        {commandName: "_clusterWriteWithoutShardKey", skip: "internal API"},
        {commandName: "_dropConnectionsToMongot", skip: "internal API"},
        {
            commandName: "_hashBSONElement",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {commandName: "_isSelf", skip: "executes locally on mongos (not sent to any remote node)"},
        {
            commandName: "_killOperations",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {commandName: "_mergeAuthzCollections", skip: "internal API"},
        {commandName: "_mongotConnPoolStats", skip: "internal API"},
        {commandName: "abortMoveCollection", skip: "TODO(SERVER-108802)"},
        {commandName: "abortReshardCollection", skip: "TODO(SERVER-108802)"},
        {commandName: "abortUnshardCollection", skip: "TODO(SERVER-108802)"},
        {commandName: "analyze", skip: "TODO(SERVER-108802)"},
        {
            commandName: "analyzeShardKey",
            skip: "TODO(SERVER-108802) Unclear how this is supposed to be replicated",
            /* run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrCheckClusterMetadataConsistency",
                shardCommandName: "_shardsvrCheckMetadataConsistency",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand(
                        {enableSharding: "db", primaryShard: st.shard0.shardName}));
                    assert.commandWorked(
                        st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));
                    for (let i = 1000; i < 10000; i++) {
                        assert.commandWorked(st.s.getDB("db")["collection"].insertOne({_id: i}));
                    }
                },
                command: () => ({analyzeShardKey: "db.collection", key: {_id: 1}}),
                cleanUp: () => assert.commandWorked(st.s.getDB("db")["collection"].deleteMany({}))
            } */
        },
        {commandName: "appendOplogNote", skip: "TODO(SERVER-108802)"},
        {commandName: "autoSplitVector", skip: "TODO(SERVER-108802)"},
        {
            commandName: "bulkWrite",
            run: {
                inAPIVersion1: true,
                shardCommandName: "bulkWrite",

                runsAgainstAdminDb: true,

                command: () => ({
                    bulkWrite: 1,
                    ops: [{insert: 0, document: {_id: 1}}],
                    nsInfo: [{ns: "db.collection"}],
                }),
            },
        },
        {commandName: "commitShardRemoval", skip: "TODO(SERVER-108802)"},
        {commandName: "getAuditConfig", skip: "TODO(SERVER-108802)", conditional: true},
        {
            commandName: "releaseMemory",
            run: {
                inAPIVersion1: false,
                shardCommandName: "releaseMemory",
                permittedInTxn: true,
                setUp: (context) => {
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({
                            insert: "collection",
                            documents: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 11}, {_id: 12}, {_id: 13}],
                        }),
                    );
                    const findCmd = Object.assign({find: "collection", batchSize: 1}, context.apiParameters);
                    const res = assert.commandWorked(context.db.runCommand(findCmd));
                    context.cursorId = res.cursor.id;
                },
                command: (context) => {
                    return {releaseMemory: [context.cursorId]};
                },
            },
        },
        {commandName: "replicateSearchIndexCommand", skip: "TODO(SERVER-108802)"},
        {commandName: "shardDrainingStatus", skip: "TODO(SERVER-108802)"},
        {commandName: "startShardDraining", skip: "TODO(SERVER-108802)"},
        {commandName: "stopShardDraining", skip: "TODO(SERVER-108802)"},
        {commandName: "untrackUnshardedCollection", skip: "TODO(SERVER-108802)"},
        {
            commandName: "changePrimary",
            skip: "TODO: Cannot run changePrimary with featureFlagBalanceUnshardedCollections disabled",
        },
        {
            commandName: "checkMetadataConsistency",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrCheckClusterMetadataConsistency",
                shardCommandName: "_shardsvrCheckMetadataConsistency",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand({enableSharding: "db", primaryShard: st.shard0.shardName}));
                    assert.commandWorked(st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));
                },
                command: () => ({checkMetadataConsistency: 1}),
            },
        },
        {
            commandName: "cleanupReshardCollection",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrCleanupReshardCollection",
                // _shardsvrCleanupReshardCollection exists in the code but is not sent to the shard
                // server
                // TODO(SERVER-108802) shardCommandName: "_shardsvrCleanupReshardCollection",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand({enableSharding: "db", primaryShard: st.shard0.shardName}));
                    assert.commandWorked(st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));
                },
                command: () => ({cleanupReshardCollection: "db.collection"}),
            },
        },
        {commandName: "cleanupStructuredEncryptionData", skip: "TODO(SERVER-108802)"},
        {
            commandName: "commitReshardCollection",
            skip: "TODO requires failpoints and concurrency",
        },
        {commandName: "compactStructuredEncryptionData", skip: "TODO(SERVER-108802)"},
        {
            commandName: "configureCollectionBalancing",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrConfigureCollectionBalancing",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand({enableSharding: "db", primaryShard: st.shard0.shardName}));
                    assert.commandWorked(st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));
                },
                command: () => ({
                    configureCollectionBalancing: "db.collection",
                    chunkSize: 256,
                    defragmentCollection: true,
                    enableAutoMerger: true,
                }),
            },
        },
        {commandName: "configureQueryAnalyzer", skip: "TODO(SERVER-108802)"},
        {commandName: "coordinateCommitTransaction", skip: "TODO(SERVER-108802)"},
        {commandName: "cpuload", skip: "executes locally on mongos (not sent to any remote node)"},
        {commandName: "createSearchIndexes", skip: "executes locally on mongos"},
        {commandName: "createUnsplittableCollection", skip: "test only and temporary"},
        {commandName: "dropSearchIndex", skip: "executes locally on mongos"},
        {commandName: "fsyncUnlock", skip: "TODO(SERVER-108802)"},
        {commandName: "getClusterParameter", skip: "executes locally on mongos"},
        {
            commandName: "getDatabaseVersion",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "getQueryableEncryptionCountInfo",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "getQueryableEncryptionCountInfo",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                command: () => ({
                    getQueryableEncryptionCountInfo: "db.collection",
                    tokens: [
                        {
                            tokens: [{"s": BinData(0, "lUBO7Mov5Sb+c/D4cJ9whhhw/+PZFLCk/AQU2+BpumQ=")}],
                        },
                    ],
                    "queryType": "insert",
                }),
            },
        },
        {commandName: "listSearchIndexes", skip: "executes locally on mongos"},
        {commandName: "lockInfo", skip: "Internal command available on mongod instances only."},
        {
            commandName: "mergeAllChunksOnShard",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrCommitMergeAllChunksOnShard",
                shardCommandName: "_shardsvrMergeAllChunksOnShard",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand({enableSharding: "db", primaryShard: st.shard0.shardName}));
                    assert.commandWorked(st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));
                },
                command: () => ({mergeAllChunksOnShard: "db.collection", shard: st.shard0.shardName}),
            },
        },
        {
            commandName: "moveCollection",
            skip: 'TODO(SERVER-108802) Primary didn\'t log _configsvrReshardCollection with API parameters { "apiVersion" : "1" }',
            /* run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrReshardCollection",
                shardCommandName: "_shardsvrReshardCollection",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand(
                        {enableSharding: "db", primaryShard: st.shard0.shardName}));
                },
                command: () => ({moveCollection: "db.collection", toShard: st.shard0.shardName})
            }*/
        },
        {
            commandName: "moveRange",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrMoveRange",
                shardCommandName: "_shardsvrMoveRange",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand({enableSharding: "db", primaryShard: st.shard0.shardName}));
                    assert.commandWorked(st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));
                },
                command: () => ({moveRange: "db.collection", toShard: st.shard0.shardName, min: {_id: 1}}),
            },
        },
        {commandName: "oidcListKeys", skip: "TODO(SERVER-108802)", conditional: true},
        {commandName: "oidcRefreshKeys", skip: "TODO(SERVER-108802)", conditional: true},
        {commandName: "removeQuerySettings", skip: "TODO(SERVER-108802)"},
        {
            commandName: "repairShardedCollectionChunksHistory",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrRepairShardedCollectionChunksHistory",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand({enableSharding: "db", primaryShard: st.shard0.shardName}));
                    assert.commandWorked(st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));
                },
                command: () => ({repairShardedCollectionChunksHistory: "db.collection"}),
            },
        },
        {
            commandName: "resetPlacementHistory",
            // The command is expected to fail when the featureFlagChangeStreamPreciseShardTargeting is disabled.
            skip: "SERVER-73741 Re-enable execution once 9.0 becomes last-lts.",
            /* run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrResetPlacementHistory",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                command: () => ({resetPlacementHistory: 1}),
            },*/
        },
        {
            commandName: "setAuditConfig",
            skip: "TODO(SERVER-108802) Auditing is not enabled by default",
            conditional: true,
            /* run: {
                inAPIVersion1: true,
                configServerCommandName: "setAuditConfig",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                command: () => ({setAuditConfig: 1, filter: {}, auditAuthorizationSuccess: false})
            } */
        },
        {
            commandName: "setClusterParameter",
            skip: 'TODO(SERVER-108802): Primary didn\'t log _shardsvrSetClusterParameter with API parameters { "apiVersion" : "1" }',
            /* run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrSetClusterParameter",
                shardCommandName: "_shardsvrSetClusterParameter",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                command: () => ({setClusterParameter: {defaultMaxTimeMS: {readOperations: 1}}}),
                cleanUp: () => assert.commandWorked(st.s.adminCommand(
                    {setClusterParameter: {defaultMaxTimeMS: {readOperations: 0}}}))
            } */
        },
        {
            commandName: "setQuerySettings",
            skip: "executes locally on mongos",
        },
        {
            commandName: "setUserWriteBlockMode",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {commandName: "testInternalTransactions", skip: "Internal API."},
        {
            commandName: "unshardCollection",
            skip: "TODO(SERVER-108802) Unclear how this is supposed to be replicated",
            /*run: {
                inAPIVersion1: false,
                shardCommandName: "_shardsvrReshardCollection",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand(
                        {enableSharding: "db", primaryShard: st.shard0.shardName}));
                    assert.commandWorked(
                        st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));
                },
                command: () => ({unshardCollection: "db.collection", toShard: st.shard0.shardName})
            }*/
        },
        {commandName: "updateSearchIndex", skip: "executes locally on mongos"},
        {commandName: "validateDBMetadata", skip: "executes locally on mongos"},
        {
            commandName: "abortTransaction",
            run: {
                inAPIVersion1: true,
                runsAgainstAdminDb: true,
                shardCommandName: "abortTransaction",
                permittedInTxn: false, // We handle the transaction manually in this test.
                setUp: (context) => {
                    // Start a session and transaction.
                    const session = st.s0.startSession();
                    const txnOptions = {autocommit: false};

                    withRetryOnTransientTxnError(
                        () => {
                            session.startTransaction(txnOptions);

                            const cmd = {
                                insert: "collection",
                                // A doc on each shard in the 2-shard configuration.
                                documents: [{_id: 1}, {_id: 21}],
                            };
                            assert.commandWorked(
                                session.getDatabase("db").runCommand(Object.assign(cmd, context.apiParameters)),
                            );
                        },
                        () => {
                            session.abortTransaction();
                        },
                    );

                    context.session = session;
                },
                command: (context) => ({
                    abortTransaction: 1,
                    lsid: context.session.getSessionId(),
                    txnNumber: context.session.getTxnNumber_forTesting(),
                    autocommit: false,
                }),
            },
        },
        {
            commandName: "addShard",
            run: {
                inAPIVersion1: false,
                runsAgainstAdminDb: true,
                configServerCommandName: "_configsvrAddShard",
                shardCommandName: "_addShard",
                shardPrimary: () => {
                    return st.rs1.getPrimary();
                },
                permittedInTxn: false,
                setUp: () => {
                    // Remove shard0 so we can add it back.
                    assert.commandWorked(st.s0.getDB("db").dropDatabase());
                    awaitRemoveShard(st.shard1.shardName);
                },
                command: () => ({addShard: st.rs1.getURL()}),
            },
        },
        {
            commandName: "transitionFromDedicatedConfigServer",
            run: {
                inAPIVersion1: false,
                runsAgainstAdminDb: true,
                configServerCommandName: "_configsvrTransitionFromDedicatedConfigServer",
                permittedInTxn: false,
                requiresCatalogShardEnabled: true,
                setUp: () => {
                    // Remove shard0 so we can add it back.
                    assert.commandWorked(st.s0.getDB("db").dropDatabase());
                    awaitTransitionToDedicatedConfigServer();
                },
                command: () => ({transitionFromDedicatedConfigServer: 1}),
            },
        },
        {
            commandName: "addShardToZone",
            run: {
                inAPIVersion1: false,
                runsAgainstAdminDb: true,
                configServerCommandName: "_configsvrAddShardToZone",
                permittedInTxn: false,
                command: () => ({addShardToZone: st.shard0.shardName, zone: "foo"}),
                cleanUp: () =>
                    assert.commandWorked(
                        st.s0.getDB("admin").runCommand({removeShardFromZone: st.shard0.shardName, zone: "foo"}),
                    ),
            },
        },
        {
            commandName: "aggregate",
            run: {
                inAPIVersion1: true,
                shardCommandName: "aggregate",
                command: () => ({aggregate: "collection", pipeline: [], cursor: {}}),
            },
            explain: {
                inAPIVersion1: true,
                shardCommandName: "explain",
                permittedInTxn: false,
                command: () => ({explain: {aggregate: "collection", pipeline: [], cursor: {}}}),
            },
        },
        {
            commandName: "authenticate",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "balancerCollectionStatus",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "balancerStart",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "balancerStatus",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "balancerStop",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "buildInfo",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "clearJumboFlag",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {commandName: "clearLog", skip: "executes locally on mongos (not sent to any remote node)"},
        {
            commandName: "collMod",
            run: {
                inAPIVersion1: true,
                shardCommandName: "_shardsvrCollMod",
                permittedInTxn: false,
                command: () => ({collMod: "collection"}),
            },
        },
        {
            commandName: "collStats",
            run: {
                inAPIVersion1: false,
                shardCommandName: "collStats",
                permittedInTxn: false,
                command: () => ({collStats: "collection"}),
            },
        },
        {
            commandName: "commitTransaction",
            run: {
                inAPIVersion1: true,
                runsAgainstAdminDb: true,
                shardCommandName: "commitTransaction",
                permittedInTxn: false, // We handle the transaction manually in this test.
                setUp: (context) => {
                    // Start a session and transaction.
                    const session = st.s0.startSession();
                    const txnOptions = {autocommit: false};

                    withRetryOnTransientTxnError(() => {
                        session.startTransaction(txnOptions);

                        const cmd = {
                            insert: "collection",
                            // A doc on each shard in the 2-shard configuration.
                            documents: [{_id: 1}, {_id: 21}],
                        };

                        assert.commandWorked(
                            session.getDatabase("db").runCommand(Object.assign(cmd, context.apiParameters)),
                        );
                    });

                    context.session = session;
                },
                command: (context) => ({
                    commitTransaction: 1,
                    lsid: context.session.getSessionId(),
                    txnNumber: context.session.getTxnNumber_forTesting(),
                    autocommit: false,
                }),
            },
        },
        {commandName: "compact", skip: "not allowed through mongos"},
        {
            commandName: "configureFailPoint",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "connPoolStats",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "connPoolSync",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "connectionStatus",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "convertToCapped",
            run: {
                inAPIVersion1: false,
                shardCommandName: "_shardsvrConvertToCapped",
                permittedOnShardedCollection: false,
                permittedInTxn: false,
                command: () => ({convertToCapped: "collection", size: 8192}),
            },
        },
        // The count command behaves differently if it has a query or no query.
        {
            commandName: "count",
            run: {
                inAPIVersion1: true,
                shardCommandName: "count",
                permittedInTxn: false,
                command: () => ({count: "collection"}),
            },
            explain: {
                inAPIVersion1: true,
                shardCommandName: "explain",
                permittedInTxn: false,
                command: () => ({explain: {count: "collection"}}),
            },
        },
        {
            commandName: "count",
            run: {
                inAPIVersion1: true,
                shardCommandName: "count",
                permittedInTxn: false,
                command: () => ({count: "collection", query: {x: 1}}),
            },
            explain: {
                inAPIVersion1: true,
                shardCommandName: "explain",
                permittedInTxn: false,
                command: () => ({explain: {count: "collection", query: {x: 1}}}),
            },
        },
        {
            commandName: "create",
            run: {
                inAPIVersion1: true,
                shardCommandName: "_shardsvrCreateCollection",
                command: () => ({create: "collection2"}),
            },
        },
        {
            commandName: "createIndexes",
            run: {
                inAPIVersion1: true,
                shardCommandName: "createIndexes",
                permittedInTxn: false,
                command: () => ({createIndexes: "collection", indexes: [{key: {a: 1}, name: "index"}]}),
            },
        },
        {
            commandName: "createRole",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "createRole",
                permittedInTxn: false,
                command: () => ({createRole: "foo", privileges: [], roles: []}),
                cleanUp: () => assert.commandWorked(st.s0.getDB("db").runCommand({dropRole: "foo"})),
            },
        },
        {
            commandName: "createUser",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "createUser",
                permittedInTxn: false,
                command: () => ({createUser: "foo", pwd: "bar", roles: []}),
                cleanUp: () => assert.commandWorked(st.s0.getDB("db").runCommand({dropUser: "foo"})),
            },
        },
        {
            commandName: "currentOp",
            run: {
                inAPIVersion1: false,
                shardCommandName: "aggregate",
                permittedInTxn: false,
                runsAgainstAdminDb: true,
                command: () => ({currentOp: 1}),
            },
        },
        {
            commandName: "dataSize",
            run: {
                inAPIVersion1: false,
                shardCommandName: "dataSize",
                permittedInTxn: false,
                command: () => ({dataSize: "db.collection"}),
            },
        },
        {
            commandName: "dbStats",
            run: {
                inAPIVersion1: false,
                shardCommandName: "dbStats",
                permittedInTxn: false,
                command: () => ({dbStats: 1, scale: 1}),
            },
        },
        {
            commandName: "delete",
            run: {
                inAPIVersion1: true,
                shardCommandName: "delete",
                command: () => ({delete: "collection", deletes: [{q: {_id: 1}, limit: 1}]}),
            },
            explain: {
                inAPIVersion1: true,
                shardCommandName: "explain",
                permittedInTxn: false,
                command: () => ({explain: {delete: "collection", deletes: [{q: {_id: 1}, limit: 1}]}}),
            },
        },
        {
            commandName: "distinct",
            run: {
                inAPIVersion1: false,
                shardCommandName: "distinct",
                permittedInTxn: false,
                command: () => ({distinct: "collection", key: "x"}),
            },
            explain: {
                inAPIVersion1: false,
                shardCommandName: "explain",
                permittedInTxn: false,
                command: () => ({explain: {distinct: "collection", key: "x"}}),
            },
        },
        {
            commandName: "drop",
            run: {
                inAPIVersion1: true,
                shardCommandName: "_shardsvrDropCollection",
                permittedInTxn: false,
                command: () => ({drop: "collection"}),
            },
        },
        {
            commandName: "dropAllRolesFromDatabase",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "dropAllRolesFromDatabase",
                permittedInTxn: false,
                setUp: () =>
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}),
                    ),
                command: () => ({dropAllRolesFromDatabase: 1}),
            },
        },
        {
            commandName: "dropAllUsersFromDatabase",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "dropAllUsersFromDatabase",
                permittedInTxn: false,
                setUp: () =>
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({createUser: "foo", pwd: "bar", roles: [], writeConcern: {w: 1}}),
                    ),
                command: () => ({dropAllUsersFromDatabase: 1}),
            },
        },
        {
            commandName: "dropConnections",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "dropDatabase",
            run: {
                inAPIVersion1: true,
                shardCommandName: "_shardsvrDropDatabase",
                permittedInTxn: false,
                command: () => ({dropDatabase: 1}),
            },
        },
        {
            commandName: "dropIndexes",
            run: {
                inAPIVersion1: true,
                shardCommandName: "_shardsvrDropIndexes",
                permittedInTxn: false,
                command: () => ({dropIndexes: "collection", index: "*"}),
            },
        },
        {
            commandName: "dropRole",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "dropRole",
                permittedInTxn: false,
                setUp: () =>
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}),
                    ),
                command: () => ({dropRole: "foo"}),
            },
        },
        {
            commandName: "dropUser",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "dropUser",
                permittedInTxn: false,
                setUp: () =>
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({createUser: "foo", pwd: "bar", roles: [], writeConcern: {w: 1}}),
                    ),
                command: () => ({dropUser: "foo"}),
            },
        },
        {commandName: "echo", skip: "executes locally on mongos (not sent to any remote node)"},
        {
            commandName: "enableSharding",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "endSessions",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {commandName: "explain", skip: "tested by other means"},
        {commandName: "features", skip: "executes locally on mongos (not sent to any remote node)"},
        {
            commandName: "filemd5",
            run: {
                inAPIVersion1: false,
                shardCommandName: "filemd5",
                permittedInTxn: false,
                command: () => ({filemd5: ObjectId(), root: "collection"}),
            },
        },
        {
            commandName: "find",
            run: {
                inAPIVersion1: true,
                shardCommandName: "find",
                command: () => ({find: "collection", filter: {x: 1}}),
            },
            explain: {
                inAPIVersion1: true,
                shardCommandName: "explain",
                permittedInTxn: false,
                command: () => ({explain: {find: "collection", filter: {x: 1}}}),
            },
        },
        {
            commandName: "find",
            run: {
                inAPIVersion1: true,
                shardCommandName: "find",
                setUp: function () {
                    st.s.getDB("db")["view"].drop();
                    assert.commandWorked(
                        st.s.getDB("db").runCommand({create: "view", viewOn: "collection", pipeline: []}),
                    );
                },
                command: () => ({find: "view", filter: {x: 1}}),
            },
            explain: {
                inAPIVersion1: true,
                shardCommandName: "explain",
                permittedInTxn: false,
                setUp: function () {
                    st.s.getDB("db")["view"].drop();
                    assert.commandWorked(
                        st.s.getDB("db").runCommand({create: "view", viewOn: "collection", pipeline: []}),
                    );
                },
                command: () => ({explain: {find: "view", filter: {x: 1}}}),
            },
        },
        {
            commandName: "findAndModify",
            run: {
                inAPIVersion1: true,
                shardCommandName: "findAndModify",
                command: () => ({findAndModify: "collection", query: {_id: 0}, remove: true}),
            },
            explain: {
                inAPIVersion1: true,
                shardCommandName: "explain",
                permittedInTxn: false,
                command: () => ({explain: {findAndModify: "collection", query: {_id: 0}, remove: true}}),
            },
        },
        {
            commandName: "flushRouterConfig",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "fsync",
            run: {
                inAPIVersion1: false,
                shardCommandName: "fsync",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                command: () => ({fsync: 1}),
            },
        },
        {
            commandName: "getCmdLineOpts",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "getDefaultRWConcern",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "getDefaultRWConcern",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                command: () => ({getDefaultRWConcern: 1}),
            },
        },
        {
            commandName: "getDiagnosticData",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {commandName: "getLog", skip: "executes locally on mongos (not sent to any remote node)"},
        {
            commandName: "getMore",
            run: {
                inAPIVersion1: true,
                shardCommandName: "getMore",
                permittedInTxn: true,
                setUp: (context) => {
                    // Global setup puts one doc on each shard, we need several on each.
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({
                            insert: "collection",
                            documents: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 11}, {_id: 12}, {_id: 13}],
                        }),
                    );
                    const findCmd = Object.assign({find: "collection", batchSize: 1}, context.apiParameters);
                    const res = assert.commandWorked(context.db.runCommand(findCmd));
                    context.cursorId = res.cursor.id;
                },
                command: (context) => {
                    return {getMore: context.cursorId, collection: "collection"};
                },
            },
        },
        {
            commandName: "getParameter",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "getShardMap",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "getShardVersion",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "grantPrivilegesToRole",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "grantPrivilegesToRole",
                permittedInTxn: false,
                setUp: () =>
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}),
                    ),
                command: () => ({
                    grantPrivilegesToRole: "foo",
                    privileges: [{resource: {db: "db", collection: "collection"}, actions: ["find"]}],
                }),
                cleanUp: () => assert.commandWorked(st.s0.getDB("db").runCommand({dropRole: "foo"})),
            },
        },
        {
            commandName: "grantRolesToRole",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "grantRolesToRole",
                permittedInTxn: false,
                setUp: function () {
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}),
                    );
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({createRole: "bar", privileges: [], roles: [], writeConcern: {w: 1}}),
                    );
                },
                command: () => ({grantRolesToRole: "foo", roles: [{role: "bar", db: "db"}]}),
                cleanUp: () => {
                    assert.commandWorked(st.s0.getDB("db").runCommand({dropRole: "foo"}));
                    assert.commandWorked(st.s0.getDB("db").runCommand({dropRole: "bar"}));
                },
            },
        },
        {
            commandName: "grantRolesToUser",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "grantRolesToUser",
                permittedInTxn: false,
                setUp: () => {
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({createUser: "foo", pwd: "bar", roles: [], writeConcern: {w: 1}}),
                    );
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}),
                    );
                },
                command: () => ({grantRolesToUser: "foo", roles: [{role: "foo", db: "db"}]}),
                cleanUp: () => {
                    assert.commandWorked(st.s0.getDB("db").runCommand({dropUser: "foo"}));
                    assert.commandWorked(st.s0.getDB("db").runCommand({dropRole: "foo"}));
                },
            },
        },
        {commandName: "hello", skip: "executes locally on mongos (not sent to any remote node)"},
        {commandName: "hostInfo", skip: "executes locally on mongos (not sent to any remote node)"},
        {
            commandName: "insert",
            run: {
                inAPIVersion1: true,
                shardCommandName: "insert",
                command: () => ({insert: "collection", documents: [{_id: 1}]}),
            },
        },
        {
            commandName: "invalidateUserCache",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {commandName: "isdbgrid", skip: "executes locally on mongos (not sent to any remote node)"},
    ];

    let testCasesSecondHalf = [
        {commandName: "isMaster", skip: "executes locally on mongos (not sent to any remote node)"},
        {
            commandName: "killCursors",
            run: {
                inAPIVersion1: true,
                shardCommandName: "killCursors",
                permittedInTxn: false,
                // Global setup puts one doc on shard 0, we need several.
                setUp: () =>
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({insert: "collection", documents: [{_id: 1}, {_id: 2}, {_id: 3}]}),
                    ),
                command: () => {
                    // Some extra logging should this test case ever fail.
                    setLogVerbosity([st.s0, st.rs0.getPrimary(), st.rs1.getPrimary()], {"command": {"verbosity": 2}});
                    const res = assert.commandWorked(st.s0.getDB("db").runCommand({find: "collection", batchSize: 1}));
                    setLogVerbosity([st.s0, st.rs0.getPrimary(), st.rs1.getPrimary()], {"command": {"verbosity": 0}});
                    jsTestLog(`"find" reply: ${tojson(res)}`);
                    const cursorId = res.cursor.id;
                    return {killCursors: "collection", cursors: [cursorId]};
                },
            },
        },
        {
            commandName: "killAllSessions",
            run: {
                inAPIVersion1: false,
                shardCommandName: "killAllSessionsByPattern",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                command: () => ({killAllSessions: []}),
            },
        },
        {
            commandName: "killAllSessionsByPattern",
            run: {
                inAPIVersion1: false,
                shardCommandName: "killAllSessionsByPattern",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                command: () => ({killAllSessionsByPattern: []}),
            },
        },
        {
            commandName: "killOp",
            run: {
                inAPIVersion1: false,
                shardCommandName: "killOp",
                permittedInTxn: false,
                runsAgainstAdminDb: true,
                setUp: (context) => {
                    function threadRoutine(connStr, uuidStr) {
                        const client = new Mongo(connStr);
                        jsTestLog(`Calling find on "${connStr}" from thread,` + ` with comment ${uuidStr}`);
                        // Target shard 0 with an _id filter.
                        const res = client.getDB("db").runCommand({
                            find: "collection",
                            filter: {_id: {$lt: 9}, $where: "sleep(99999999); return true;"},
                            comment: uuidStr,
                        });
                        jsTestLog(`Called find command: ${tojson(res)}`);
                    }

                    // Some extra logging should this test case ever fail.
                    setLogVerbosity([st.s0, st.rs0.getPrimary(), st.rs1.getPrimary()], {"command": {"verbosity": 2}});

                    const uuidStr = UUID().toString();
                    context.thread = new Thread(threadRoutine, st.s0.host, uuidStr);
                    context.thread.start();
                    const adminDb = st.s0.getDB("admin");

                    jsTestLog(
                        `Waiting for "find" on "${st.shard0.shardName}" ` + `with comment ${uuidStr} in currentOp`,
                    );
                    assert.soon(() => {
                        const filter = {
                            "command.find": "collection",
                            "command.comment": uuidStr,
                            shard: st.shard0.shardName,
                        };
                        const inprog = adminDb.currentOp(filter).inprog;
                        if (inprog.length === 1) {
                            jsTestLog(`Found it! findOpId ${inprog[0].opid}`);
                            context.findOpId = inprog[0].opid;
                            return true;
                        }

                        assert.lt(inprog.length, 2, `More than one command found in currentOp: ${tojson(inprog)}`);
                    });
                    setLogVerbosity([st.s0, st.rs0.getPrimary(), st.rs1.getPrimary()], {"command": {"verbosity": 0}});
                },
                command: (context) => ({killOp: 1, op: context.findOpId}),
                cleanUp: (context) => {
                    context.thread.join();
                },
            },
        },
        {
            commandName: "killSessions",
            run: {
                inAPIVersion1: false,
                shardCommandName: "killAllSessionsByPattern",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                setUp: (context) => {
                    const session = st.s0.startSession();
                    context.lsid = session.getSessionId();
                },
                command: (context) => ({killSessions: [context.lsid]}),
            },
        },
        {
            commandName: "listCollections",
            run: {
                inAPIVersion1: true,
                shardCommandName: "listCollections",
                permittedInTxn: false,
                command: () => ({listCollections: 1}),
            },
        },
        {
            commandName: "listCommands",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "listDatabases",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "listIndexes",
            run: {
                inAPIVersion1: true,
                shardCommandName: "listIndexes",
                permittedInTxn: false,
                command: () => ({listIndexes: "collection"}),
            },
        },
        {
            commandName: "listShards",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "logApplicationMessage",
            skip: "executes locally on mongos (not sent to any remote node)",
            conditional: true,
        },
        {
            commandName: "logMessage",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "logRotate",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {commandName: "logout", skip: "executes locally on mongos (not sent to any remote node)"},
        {
            commandName: "mapReduce",
            run: {
                inAPIVersion1: false,
                permittedInTxn: false,
                shardCommandName: "aggregate",
                command: () => ({
                    mapReduce: "collection",
                    map: function mapFunc() {
                        emit(this.x, 1);
                    },
                    reduce: function reduceFunc(key, values) {
                        return Array.sum(values);
                    },
                    out: {inline: 1},
                }),
            },
            explain: {
                inAPIVersion1: false,
                shardCommandName: "explain",
                permittedInTxn: false,
                command: () => ({
                    explain: {
                        mapReduce: "collection",
                        map: function mapFunc() {
                            emit(this.x, 1);
                        },
                        reduce: function reduceFunc(key, values) {
                            return Array.sum(values);
                        },
                        out: {inline: 1},
                    },
                }),
            },
        },
        {
            commandName: "mergeChunks",
            run: {
                inAPIVersion1: false,
                shardCommandName: "_shardsvrMergeChunks",
                configServerCommandName: "_configsvrCommitChunksMerge",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                requiresShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand({split: "db.collection", middle: {_id: -5}}));
                    assert.commandWorked(st.s.adminCommand({split: "db.collection", middle: {_id: 10}}));
                    // Now the chunks are: [MinKey, -5], (-5, 10], (10, MaxKey].
                },
                command: () => ({mergeChunks: "db.collection", bounds: [{_id: MinKey}, {_id: 10}]}),
            },
        },
        {
            commandName: "moveChunk",
            run: {
                inAPIVersion1: false,
                shardCommandName: "_shardsvrMoveRange",
                configServerCommandName: "_configsvrMoveRange",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                requiresShardedCollection: true,
                command: () => ({
                    moveChunk: "db.collection",
                    find: {_id: 1},
                    to: st.shard1.shardName,
                    // Don't interfere with the next test case.
                    _waitForDelete: true,
                }),
            },
        },
        {
            commandName: "movePrimary",
            run: {
                inAPIVersion1: false,
                shardCommandName: "_shardsvrMovePrimary",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                command: () => ({movePrimary: "db", to: st.shard1.shardName}),
            },
        },
        {
            commandName: "multicast",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {commandName: "netstat", skip: "executes locally on mongos (not sent to any remote node)"},
        {commandName: "ping", skip: "executes locally on mongos (not sent to any remote node)"},
        {
            commandName: "planCacheClear",
            run: {
                inAPIVersion1: false,
                shardCommandName: "planCacheClear",
                permittedInTxn: false,
                command: () => ({planCacheClear: "collection"}),
            },
        },
        {
            commandName: "planCacheClearFilters",
            run: {
                inAPIVersion1: false,
                shardCommandName: "planCacheClearFilters",
                permittedInTxn: false,
                command: () => ({planCacheClearFilters: "collection"}),
            },
        },
        {
            commandName: "planCacheListFilters",
            run: {
                inAPIVersion1: false,
                shardCommandName: "planCacheListFilters",
                permittedInTxn: false,
                setUp: () =>
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({planCacheSetFilter: "collection", query: {_id: 1}, indexes: [{_id: 1}]}),
                    ),
                command: () => ({planCacheListFilters: "collection"}),
            },
        },
        {
            commandName: "planCacheSetFilter",
            run: {
                inAPIVersion1: false,
                shardCommandName: "planCacheSetFilter",
                permittedInTxn: false,
                command: () => ({planCacheSetFilter: "collection", query: {_id: 1}, indexes: [{_id: 1}]}),
            },
        },
        {commandName: "profile", skip: "not supported in mongos"},
        {commandName: "reapLogicalSessionCacheNow", skip: "is a no-op on mongos"},
        {
            commandName: "refineCollectionShardKey",
            run: {
                inAPIVersion1: false,
                shardCommandName: "_shardsvrRefineCollectionShardKey",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                requiresShardedCollection: true,
                setUp: () =>
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({
                            createIndexes: "collection",
                            indexes: [{key: {_id: 1, x: 1}, name: "_id-1-x-1"}],
                        }),
                    ),
                command: () => ({refineCollectionShardKey: "db.collection", key: {_id: 1, x: 1}}),
            },
        },
        {
            commandName: "refreshLogicalSessionCacheNow",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "refreshSessions",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "removeShard",
            run: {
                inAPIVersion1: false,
                runsAgainstAdminDb: true,
                configServerCommandName: "_configsvrRemoveShard",
                permittedInTxn: false,
                command: () => ({removeShard: st.shard1.shardName}),
                cleanUp: () => {
                    // Wait for the shard to be removed completely before re-adding it.
                    awaitRemoveShard(st.shard1.shardName);
                    assert.commandWorked(
                        st.s0.getDB("admin").runCommand({addShard: st.rs1.getURL(), name: st.shard1.shardName}),
                    );
                },
            },
        },
        {
            commandName: "transitionToDedicatedConfigServer",
            run: {
                inAPIVersion1: false,
                runsAgainstAdminDb: true,
                configServerCommandName: "_configsvrTransitionToDedicatedConfigServer",
                permittedInTxn: false,
                requiresCatalogShardEnabled: true,
                command: () => ({transitionToDedicatedConfigServer: 1}),
                cleanUp: () => {
                    // Wait for the shard to be removed completely before re-adding it.
                    awaitTransitionToDedicatedConfigServer(st.shard0.shardName);
                    assert.commandWorked(st.s0.getDB("admin").runCommand({transitionFromDedicatedConfigServer: 1}));
                },
            },
        },
        {
            commandName: "removeShardFromZone",
            run: {
                inAPIVersion1: false,
                runsAgainstAdminDb: true,
                configServerCommandName: "_configsvrRemoveShardFromZone",
                permittedInTxn: false,
                setup: () => assert.commandWorked({addShardToZone: st.shard0.shardName, zone: "foo"}),
                command: () => ({removeShardFromZone: st.shard0.shardName, zone: "foo"}),
            },
        },
        {
            commandName: "renameCollection",
            run: {
                inAPIVersion1: false,
                shardCommandName: "_shardsvrRenameCollection",
                permittedOnShardedCollection: false,
                permittedInTxn: false,
                runsAgainstAdminDb: true,
                command: () => ({renameCollection: "db.collection", to: "db.collection_renamed"}),
            },
        },
        {commandName: "replSetGetStatus", skip: "not supported in mongos"},
        {
            commandName: "reshardCollection",
            run: {
                inAPIVersion1: false,
                permittedInTxn: false,
                shardCommandName: "_shardsvrReshardCollection",
                requiresShardedCollection: true,
                // reshardCollection internally does atClusterTime reads.
                requiresCommittedReads: true,
                runsAgainstAdminDb: true,
                command: () => ({reshardCollection: "db.collection", key: {_id: 1}}),
            },
        },
        {
            commandName: "revokePrivilegesFromRole",
            run: {
                inAPIVersion1: false,
                permittedInTxn: false,
                configServerCommandName: "revokePrivilegesFromRole",
                setUp: () =>
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({
                            createRole: "foo",
                            privileges: [{resource: {db: "db", collection: "collection"}, actions: ["find"]}],
                            roles: [],
                            writeConcern: {w: 1},
                        }),
                    ),
                command: () => ({
                    revokePrivilegesFromRole: "foo",
                    privileges: [{resource: {db: "db", collection: "collection"}, actions: ["find"]}],
                }),
                cleanUp: () => {
                    assert.commandWorked(st.s0.getDB("db").runCommand({dropRole: "foo"}));
                },
            },
        },
        {
            commandName: "revokeRolesFromRole",
            run: {
                inAPIVersion1: false,
                permittedInTxn: false,
                configServerCommandName: "revokeRolesFromRole",
                setUp: () => {
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}),
                    );
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({createRole: "bar", privileges: [], roles: [], writeConcern: {w: 1}}),
                    );
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({grantRolesToRole: "foo", roles: [{role: "bar", db: "db"}]}),
                    );
                },
                command: () => ({revokeRolesFromRole: "foo", roles: [{role: "bar", db: "db"}]}),
                cleanUp: () => {
                    assert.commandWorked(st.s0.getDB("db").runCommand({dropRole: "foo"}));
                    assert.commandWorked(st.s0.getDB("db").runCommand({dropRole: "bar"}));
                },
            },
        },
        {
            commandName: "revokeRolesFromUser",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "revokeRolesFromUser",
                permittedInTxn: false,
                setUp: () => {
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({createUser: "foo", pwd: "bar", roles: [], writeConcern: {w: 1}}),
                    );
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}),
                    );
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({grantRolesToUser: "foo", roles: [{role: "foo", db: "db"}]}),
                    );
                },
                command: () => ({revokeRolesFromUser: "foo", roles: [{role: "foo", db: "db"}]}),
                cleanUp: () => {
                    assert.commandWorked(st.s0.getDB("db").runCommand({dropUser: "foo"}));
                    assert.commandWorked(st.s0.getDB("db").runCommand({dropRole: "foo"}));
                },
            },
        },
        {
            commandName: "rolesInfo",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "rolesInfo",
                permittedInTxn: false,
                command: () => ({rolesInfo: 1}),
            },
        },
        {
            commandName: "rotateCertificates",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "rotateFTDC",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "saslContinue",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "saslStart",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "serverStatus",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "setAllowMigrations",
            run: {
                inAPIVersion1: false,
                shardCommandName: "_shardsvrSetAllowMigrations",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                requiresShardedCollection: true,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand({enableSharding: "db", primaryShard: st.shard0.shardName}));
                    assert.commandWorked(st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));
                },
                command: () => ({setAllowMigrations: "db.collection", allowMigrations: true}),
            },
        },
        {
            commandName: "setDefaultRWConcern",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "setDefaultRWConcern",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                command: () => ({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}}),
            },
        },
        {
            commandName: "setIndexCommitQuorum",
            run: {
                inAPIVersion1: false,
                shardCommandName: "setIndexCommitQuorum",
                permittedInTxn: false,
                // The command should fail if there is no active index build on the collection.
                expectedFailureCode: ErrorCodes.IndexNotFound,
                command: () => ({
                    setIndexCommitQuorum: "collection",
                    indexNames: ["index"],
                    commitQuorum: "majority",
                }),
            },
        },
        {
            commandName: "setFeatureCompatibilityVersion",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "setFeatureCompatibilityVersion",
                permittedInTxn: false,
                runsAgainstAdminDb: true,
                command: () => ({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            },
        },
        {
            commandName: "setProfilingFilterGlobally",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "setParameter",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "shardCollection",
            run: {
                inAPIVersion1: false,
                shardCommandName: "_shardsvrCreateCollection",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                permittedOnShardedCollection: false,
                setUp: () => {
                    assert.commandWorked(st.s.adminCommand({enableSharding: "db", primaryShard: st.shard0.shardName}));
                },
                command: () => ({shardCollection: "db.collection", key: {_id: 1}}),
            },
        },
        {commandName: "shutdown", skip: "executes locally on mongos (not sent to any remote node)"},
        {
            commandName: "split",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrCommitChunkSplit",
                shardCommandName: "splitChunk",
                runsAgainstAdminDb: true,
                permittedInTxn: false,
                requiresShardedCollection: true,
                command: () => ({split: "db.collection", middle: {_id: 5}}),
            },
        },
        {
            commandName: "splitVector",
            run: {
                inAPIVersion1: false,
                shardCommandName: "splitVector",
                permittedInTxn: false,
                permittedOnShardedCollection: false,
                command: () => ({
                    splitVector: "db.collection",
                    keyPattern: {_id: 1},
                    min: {_id: 0},
                    max: {_id: MaxKey},
                    maxChunkSizeBytes: 1024 * 1024,
                }),
            },
        },
        {commandName: "getTrafficRecordingStatus", skip: "executes locally on targeted node"},
        {commandName: "startRecordingTraffic", skip: "Renamed to startTrafficRecording"},
        {commandName: "stopRecordingTraffic", skip: "Renamed to stopTrafficRecording"},
        {
            commandName: "startTrafficRecording",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "startSession",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "stopTrafficRecording",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "testDeprecation",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "testDeprecationInVersion2",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "testRemoval",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "testVersion2",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "testVersions1And2",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "update",
            run: {
                inAPIVersion1: true,
                shardCommandName: "update",
                command: () => ({
                    update: "collection",
                    updates: [{q: {_id: 2}, u: {_id: 2}, upsert: true, multi: false}],
                }),
            },
            explain: {
                inAPIVersion1: true,
                shardCommandName: "explain",
                permittedInTxn: false,
                command: () => ({
                    explain: {
                        update: "collection",
                        updates: [{q: {_id: 2}, u: {_id: 2}, upsert: true, multi: false}],
                    },
                }),
            },
        },
        {
            commandName: "updateRole",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "updateRole",
                permittedInTxn: false,
                setUp: () =>
                    assert.commandWorked(
                        st.s0
                            .getDB("db")
                            .runCommand({createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}),
                    ),
                command: () => ({updateRole: "foo", authenticationRestrictions: []}),
                cleanUp: () => assert.commandWorked(st.s0.getDB("db").runCommand({dropAllRolesFromDatabase: 1})),
            },
        },
        {
            commandName: "updateUser",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "updateUser",
                permittedInTxn: false,
                setUp: () =>
                    assert.commandWorked(
                        st.s0.getDB("db").runCommand({createUser: "foo", pwd: "bar", roles: [], writeConcern: {w: 1}}),
                    ),
                command: () => ({updateUser: "foo", authenticationRestrictions: []}),
                cleanUp: () => assert.commandWorked(st.s0.getDB("db").runCommand({dropAllUsersFromDatabase: 1})),
            },
        },
        {
            commandName: "updateZoneKeyRange",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "_configsvrUpdateZoneKeyRange",
                permittedInTxn: false,
                runsAgainstAdminDb: true,
                setUp: () =>
                    assert.commandWorked(
                        st.s0.getDB("admin").runCommand({addShardToZone: st.shard0.shardName, zone: "foo"}),
                    ),
                command: () => ({
                    updateZoneKeyRange: "db.collection",
                    min: {_id: 1},
                    max: {_id: 5},
                    zone: "foo",
                }),
                cleanUp: () => {
                    // Remove zone key range.
                    assert.commandWorked(
                        st.s0.getDB("admin").runCommand({
                            updateZoneKeyRange: "db.collection",
                            min: {_id: 1},
                            max: {_id: 5},
                            zone: null,
                        }),
                    );
                    assert.commandWorked(
                        st.s0.getDB("admin").runCommand({removeShardFromZone: st.shard0.shardName, zone: "foo"}),
                    );
                },
            },
        },
        {
            commandName: "usersInfo",
            run: {
                inAPIVersion1: false,
                configServerCommandName: "usersInfo",
                permittedInTxn: false,
                command: () => ({usersInfo: 1}),
            },
        },
        {
            commandName: "validate",
            run: {
                inAPIVersion1: false,
                shardCommandName: "validate",
                permittedInTxn: false,
                command: () => ({validate: "collection"}),
            },
        },
        {
            commandName: "waitForFailPoint",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
        {
            commandName: "whatsmyuri",
            skip: "executes locally on mongos (not sent to any remote node)",
        },
    ];

    commandsRemovedFromMongosSinceLastLTS.forEach(function (cmd) {
        // Since we will skip this test later, arbitrarily add it to the first half of
        // the test cases.
        testCasesFirstHalf.push({
            commandName: cmd,
            skip: "must define test coverage for latest version backwards compatibility",
        });
    });

    const st = new ShardingTest({mongos: 1, shards: 2, config: 1, rs: {nodes: 1}});
    const listCommandsRes = st.s0.adminCommand({listCommands: 1});
    assert.commandWorked(listCommandsRes);

    const supportsCommittedReads = assert.commandWorked(st.rs0.getPrimary().adminCommand({serverStatus: 1}))
        .storageEngine.supportsCommittedReads;

    const isConfigShardEnabled = ShardTransitionUtil.isConfigServerTransitionEnabledIgnoringFCV(st);

    (() => {
        // Validate test cases for all commands. Ensure there is at least one test case for every
        // mongos command, and that the test cases are well formed.
        for (const command of Object.keys(listCommandsRes.commands)) {
            const matchingCases = [
                ...testCasesFirstHalf.filter((elem) => elem.commandName === command),
                ...testCasesSecondHalf.filter((elem) => elem.commandName === command),
            ];
            assert(matchingCases.length > 0, "coverage failure: must define a test case for " + command);
            for (const testCase of matchingCases) {
                validateTestCase(testCase);
                testCase.validated = true;
            }
        }

        // After iterating through all the existing commands, ensure there were no additional test
        // cases that did not correspond to any mongos command.
        for (const testCase of [...testCasesFirstHalf, ...testCasesSecondHalf]) {
            // We have defined real test cases for commands added since the last LTS version so that
            // the test cases are exercised in the regular suites, but because these test cases
            // can't run in the last stable suite, we skip processing them here to avoid failing the
            // below assertion. We have defined "skip" test cases for commands removed since the
            // last LTS version so the test case is defined in last stable suites (in which these
            // commands still exist on the mongos), but these test cases won't be run in regular
            // suites, so we skip processing them below as well.
            if (
                commandsAddedToMongosSinceLastLTS.includes(testCase.commandName) ||
                commandsRemovedFromMongosSinceLastLTS.includes(testCase.commandName)
            )
                continue;
            assert(
                testCase.validated || testCase.conditional,
                "you defined a test case for a command '" +
                    testCase.commandName +
                    "' that does not exist on mongos: " +
                    tojson(testCase),
            );
        }
    })();

    function checkPrimaryLog(conn, commandName, apiParameters) {
        let msg;
        assert.soon(
            () => {
                const logs = checkLog.getGlobalLog(conn);
                let lastCommandInvocation;

                for (let logMsg of logs) {
                    const obj = JSON.parse(logMsg);
                    // Search for "About to run the command" logs.
                    if (obj.id !== 21965) continue;

                    const args = obj.attr.commandArgs;
                    if (commandName !== Object.keys(args)[0]) continue;

                    lastCommandInvocation = args;
                    if (
                        args.apiVersion !== apiParameters.apiVersion ||
                        args.apiStrict !== apiParameters.apiStrict ||
                        args.apiDeprecationErrors !== apiParameters.apiDeprecationErrors
                    )
                        continue;

                    // Found a match.
                    return true;
                }

                if (lastCommandInvocation === undefined) {
                    msg = `Primary didn't log ${commandName} with API parameters ` + `${tojson(apiParameters)}.`;
                    return false;
                }

                msg =
                    `Primary didn't log ${commandName} with API parameters ` +
                    `${tojson(apiParameters)}. Last invocation of ${commandName} was` +
                    ` ${tojson(lastCommandInvocation)}`;
                return false;
            },
            // assert.soon message function.
            () => {
                return msg;
            },
        );
    }

    // We split up the test cases into two halves in order to decrease the runtime of a single JS
    // test - each JS test file should either call runTestsFirstHalf or runTestsSecondHalf.
    function runTestsFirstHalf({inTransaction, shardedCollection}) {
        runTests({
            inTransaction: inTransaction,
            shardedCollection: shardedCollection,
            cases: testCasesFirstHalf,
        });
    }

    // We split up the test cases into two halves in order to decrease the runtime of a single JS
    // test - each JS test file should either call runTestsFirstHalf or runTestsSecondHalf.
    function runTestsSecondHalf({inTransaction, shardedCollection}) {
        runTests({
            inTransaction: inTransaction,
            shardedCollection: shardedCollection,
            cases: testCasesSecondHalf,
        });
    }

    function runTests({inTransaction, shardedCollection, cases}) {
        // For each combination of config parameters and test case, create a test instance. Do this
        // before executing the test instances so we can count the number of instances and log
        // progress.
        let testInstances = [];

        for (const apiParameters of [
            {},
            {apiVersion: "1"},
            {apiVersion: "1", apiDeprecationErrors: false},
            {apiVersion: "1", apiDeprecationErrors: true},
            {apiVersion: "1", apiStrict: false},
            {apiVersion: "1", apiStrict: false, apiDeprecationErrors: false},
            {apiVersion: "1", apiStrict: false, apiDeprecationErrors: true},
            {apiVersion: "1", apiStrict: true},
            {apiVersion: "1", apiStrict: true, apiDeprecationErrors: false},
            {apiVersion: "1", apiStrict: true, apiDeprecationErrors: true},
        ]) {
            for (const testCase of cases) {
                if (testCase.skip) continue;

                for (let runOrExplain of [testCase.run, testCase.explain]) {
                    if (runOrExplain === undefined) continue;

                    if (inTransaction && !runOrExplain.permittedInTxn) continue;

                    if (shardedCollection && !runOrExplain.permittedOnShardedCollection) continue;

                    if (!shardedCollection && runOrExplain.requiresShardedCollection) continue;

                    if (!supportsCommittedReads && runOrExplain.requiresCommittedReads) continue;

                    if (!isConfigShardEnabled && runOrExplain.requiresCatalogShardEnabled) continue;

                    if (apiParameters.apiStrict && !runOrExplain.inAPIVersion1) continue;

                    testInstances.push({
                        apiParameters: apiParameters,
                        commandName: testCase.commandName,
                        runOrExplain: runOrExplain,
                    });
                }
            }
        }

        for (let i = 0; i < testInstances.length; ++i) {
            const {apiParameters, commandName, runOrExplain} = testInstances[i];
            const context = {apiParameters: apiParameters};

            let shardPrimary, configPrimary;

            withRetryOnTransientTxnError(
                () => {
                    assert.commandWorked(st.s.adminCommand({enableSharding: "db", primaryShard: st.shard0.shardName}));

                    if (shardedCollection) {
                        assert.commandWorked(st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));
                    }

                    assert.commandWorked(
                        st.s.getDB("db")["collection"].insert({_id: 0}, {writeConcern: {w: "majority"}}),
                    );

                    configPrimary = st.configRS.getPrimary();
                    shardPrimary = runOrExplain.shardPrimary ? runOrExplain.shardPrimary() : st.rs0.getPrimary();

                    const commandDbName = runOrExplain.runsAgainstAdminDb ? "admin" : "db";
                    if (inTransaction) {
                        context.session = st.s0.startSession();
                        context.session.startTransaction();
                        context.db = context.session.getDatabase(commandDbName);
                    } else {
                        context.db = st.s0.getDB(commandDbName);
                    }

                    if (runOrExplain.setUp) {
                        jsTestLog(`setUp function for ${commandName}`);
                        runOrExplain.setUp(context);
                        jsTestLog(`setUp function for ${commandName} completed`);
                    }

                    // Make a copy of the test's command body, and set its API parameters.
                    const commandBody = runOrExplain.command(context);
                    const commandWithAPIParams = Object.assign(Object.assign({}, commandBody), apiParameters);

                    assert.commandWorked(configPrimary.adminCommand({clearLog: "global"}));
                    assert.commandWorked(shardPrimary.adminCommand({clearLog: "global"}));
                    const message =
                        `[${i + 1} of ${testInstances.length}]: command ${tojson(commandWithAPIParams)}` +
                        ` ${shardedCollection ? "sharded" : "unsharded"},` +
                        ` ${inTransaction ? "in" : "outside"} transaction` +
                        ` on "${commandDbName}" database`;

                    flushRoutersAndRefreshShardMetadata(st, {ns: "db.collection"});

                    jsTestLog(`Running ${message}`);
                    setLogVerbosity([configPrimary, st.rs0.getPrimary(), st.rs1.getPrimary()], {
                        "command": {"verbosity": 2},
                    });

                    const res = context.db.runCommand(commandWithAPIParams);
                    jsTestLog(`Command result: ${tojson(res)}`);
                    if (runOrExplain.expectedFailureCode) {
                        assert.commandFailedWithCode(res, runOrExplain.expectedFailureCode);
                    } else {
                        assert.commandWorked(res);
                    }

                    if (inTransaction) {
                        const commitCmd = {
                            commitTransaction: 1,
                            txnNumber: context.session.getTxnNumber_forTesting(),
                            autocommit: false,
                        };

                        assert.commandWorked(
                            context.session.getDatabase("admin").runCommand(Object.assign(commitCmd, apiParameters)),
                        );
                    }
                },
                () => {
                    if (inTransaction) {
                        jsTestLog(`handling transactional retry for ${commandName}`);
                        context.session.abortTransaction();

                        setLogVerbosity([configPrimary, st.rs0.getPrimary(), st.rs1.getPrimary()], {
                            "command": {"verbosity": 0},
                        });

                        st.s0.getDB("db").runCommand({dropDatabase: 1});
                        if (runOrExplain.cleanUp) {
                            jsTestLog(`cleanUp function for ${commandName}`);
                            runOrExplain.cleanUp(context);
                            jsTestLog(`cleanUp function for ${commandName} completed`);
                        }
                    }
                },
            );

            const configServerCommandName = runOrExplain.configServerCommandName;
            const shardCommandName = runOrExplain.shardCommandName;

            if (configServerCommandName) {
                jsTestLog(`Check for ${configServerCommandName} in config server's log`);
                checkPrimaryLog(configPrimary, configServerCommandName, apiParameters);
            }

            if (shardCommandName) {
                jsTestLog(`Check for ${shardCommandName} in shard server's log`);
                checkPrimaryLog(shardPrimary, shardCommandName, apiParameters);
            }

            setLogVerbosity([configPrimary, st.rs0.getPrimary(), st.rs1.getPrimary()], {"command": {"verbosity": 0}});

            st.s0.getDB("db").runCommand({dropDatabase: 1});
            if (runOrExplain.cleanUp) {
                jsTestLog(`cleanUp function for ${commandName}`);
                runOrExplain.cleanUp(context);
                jsTestLog(`cleanUp function for ${commandName} completed`);
            }
        }

        st.stop();
    }

    return {runTestsFirstHalf: runTestsFirstHalf, runTestsSecondHalf: runTestsSecondHalf};
})();
