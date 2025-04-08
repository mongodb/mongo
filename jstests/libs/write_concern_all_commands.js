/**
 * Tests that commands that accept write concern correctly return write concern errors.
 *
 * Every command that accepts writeConcern should define a test case. Test cases should consist of a
 * no-op case, success case, and failure case where applicable. The no-op and failure scenarios
 * should mimic scenarios where it's necessary to wait for write concern because the outcome of the
 * request indicates to the user that the data is in some definitive state, e.g. the write is
 * identical to a previous write, or the user has set up schema validation rules that would cause
 * the current write to fail.
 *
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {AllCommandsTest} from "jstests/libs/all_commands_test.js";
import {getCommandName} from "jstests/libs/cmd_object_utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {assertWriteConcernError} from "jstests/libs/write_concern_util.js";

const dbName = 'testDB';
const collName = 'testColl';
const fullNs = dbName + "." + collName;

const timeValue = ISODate("2015-12-31T23:59:59.000Z");

// Set up txn state to be used in the transaction test cases
let _lsid = UUID();
let _txnNum = 0;
function getLSID() {
    return {id: _lsid};
}
function getTxnNumber() {
    return NumberLong(_txnNum);
}
function genNextTxnNumber() {
    _txnNum++;
}

function getShardNames(cluster) {
    return [cluster.shard0.shardName, cluster.shard1.shardName];
}

function getShardKeyMinRanges(coll) {
    let a = coll.getDB()
                .getSiblingDB("config")
                .chunks.find({uuid: coll.getUUID()}, {min: 1, _id: 0})
                .sort({min: 1});
    return a.toArray();
}

// TODO SERVER-97754 Remove these 2 functions, and do not stop the remaining secondaries once these
// commands no longer override user provided writeConcern
let stopAdditionalSecondariesIfSharded = function(clusterType, cluster, secondariesRunning) {
    if (clusterType == "sharded") {
        const shards = cluster.getAllShards();
        for (let i = 0; i < shards.length; i++) {
            shards[i].stop(secondariesRunning[i]);
        }
    }
};
let restartAdditionalSecondariesIfSharded = function(clusterType, cluster, secondariesRunning) {
    if (clusterType == "sharded") {
        const shards = cluster.getAllShards();
        for (let i = 0; i < shards.length; i++) {
            shards[i].restart(secondariesRunning[i]);
        }
    }
};

let getShardKey = (coll, fullNs) => {
    let entry = coll.getDB().getSiblingDB("config").collections.findOne({_id: fullNs}, {key: 1});

    // If there is no shard key, return an empty obj
    if (!entry) {
        return {};
    }

    return entry.key;
};

// All commands in the server.
const wcCommandsTests = {
    _addShard: {skip: "internal command"},
    _cloneCollectionOptionsFromPrimaryShard: {skip: "internal command"},
    _clusterQueryWithoutShardKey: {skip: "internal command"},
    _clusterWriteWithoutShardKey: {skip: "internal command"},
    _configsvrAbortReshardCollection: {skip: "internal command"},
    _configsvrAddShard: {skip: "internal command"},
    _configsvrAddShardCoordinator: {skip: "internal command"},
    _configsvrAddShardToZone: {skip: "internal command"},
    _configsvrBalancerCollectionStatus: {skip: "internal command"},
    _configsvrBalancerStart: {skip: "internal command"},
    _configsvrBalancerStatus: {skip: "internal command"},
    _configsvrBalancerStop: {skip: "internal command"},
    _configsvrCheckClusterMetadataConsistency: {skip: "internal command"},
    _configsvrCheckMetadataConsistency: {skip: "internal command"},
    _configsvrCleanupReshardCollection: {skip: "internal command"},
    _configsvrCollMod: {skip: "internal command"},
    _configsvrClearJumboFlag: {skip: "internal command"},
    _configsvrCommitChunksMerge: {skip: "internal command"},
    _configsvrCommitChunkMigration: {skip: "internal command"},
    _configsvrCommitChunkSplit: {skip: "internal command"},
    _configsvrCommitIndex: {skip: "internal command"},
    _configsvrCommitMergeAllChunksOnShard: {skip: "internal command"},
    _configsvrCommitMovePrimary: {skip: "internal command"},
    _configsvrCommitRefineCollectionShardKey: {skip: "internal command"},
    _configsvrCommitReshardCollection: {skip: "internal command"},
    _configsvrConfigureCollectionBalancing: {skip: "internal command"},
    _configsvrCreateDatabase: {skip: "internal command"},
    _configsvrDropIndexCatalogEntry: {skip: "internal command"},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: "internal command"},
    _configsvrGetHistoricalPlacement: {skip: "internal command"},
    _configsvrMoveRange: {skip: "internal command"},
    _configsvrRemoveChunks: {skip: "internal command"},
    _configsvrRemoveShard: {skip: "internal command"},
    _configsvrRemoveShardFromZone: {skip: "internal command"},
    _configsvrRemoveTags: {skip: "internal command"},
    _configsvrRenameCollection: {skip: "internal command"},
    _configsvrRepairShardedCollectionChunksHistory: {skip: "internal command"},
    _configsvrResetPlacementHistory: {skip: "internal command"},
    _configsvrReshardCollection: {skip: "internal command"},
    _configsvrRunRestore: {skip: "internal command"},
    _configsvrSetAllowMigrations: {skip: "internal command"},
    _configsvrSetClusterParameter: {skip: "internal command"},
    _configsvrSetUserWriteBlockMode: {skip: "internal command"},
    _configsvrTransitionFromDedicatedConfigServer: {skip: "internal command"},
    _configsvrTransitionToDedicatedConfigServer: {skip: "internal command"},
    _configsvrUpdateZoneKeyRange: {skip: "internal command"},
    _dropConnectionsToMongot: {skip: "internal command"},
    _flushDatabaseCacheUpdates: {skip: "internal command"},
    _flushDatabaseCacheUpdatesWithWriteConcern: {skip: "internal command"},
    _flushReshardingStateChange: {skip: "internal command"},
    _flushRoutingTableCacheUpdates: {skip: "internal command"},
    _flushRoutingTableCacheUpdatesWithWriteConcern: {skip: "internal command"},
    _getNextSessionMods: {skip: "internal command"},
    _getUserCacheGeneration: {skip: "internal command"},
    _hashBSONElement: {skip: "internal command"},
    _isSelf: {skip: "internal command"},
    _killOperations: {skip: "internal command"},
    _mergeAuthzCollections: {skip: "internal command"},
    _migrateClone: {skip: "internal command"},
    _mongotConnPoolStats: {skip: "internal command"},
    _recvChunkAbort: {skip: "internal command"},
    _recvChunkCommit: {skip: "internal command"},
    _recvChunkReleaseCritSec: {skip: "internal command"},
    _recvChunkStart: {skip: "internal command"},
    _recvChunkStatus: {skip: "internal command"},
    _refreshQueryAnalyzerConfiguration: {skip: "internal command"},
    _shardsvrAbortReshardCollection: {skip: "internal command"},
    _shardsvrBeginMigrationBlockingOperation: {skip: "internal command"},
    _shardsvrChangePrimary: {skip: "internal command"},
    _shardsvrCleanupReshardCollection: {skip: "internal command"},
    _shardsvrCloneAuthoritativeMetadata: {skip: "internal command"},
    _shardsvrCloneCatalogData: {skip: "internal command"},
    _shardsvrCommitCreateDatabaseMetadata: {skip: "internal command"},
    _shardsvrCommitDropDatabaseMetadata: {skip: "internal command"},
    _shardsvrRegisterIndex: {skip: "internal command"},
    _shardsvrCheckMetadataConsistency: {skip: "internal command"},
    _shardsvrCheckMetadataConsistencyParticipant: {skip: "internal command"},
    _shardsvrCleanupStructuredEncryptionData: {skip: "internal command"},
    _shardsvrCommitIndexParticipant: {skip: "internal command"},
    _shardsvrCommitReshardCollection: {skip: "internal command"},
    _shardsvrCompactStructuredEncryptionData: {skip: "internal command"},
    _shardsvrConvertToCapped: {skip: "internal command"},
    _shardsvrCoordinateMultiUpdate: {skip: "internal command"},
    _shardsvrCreateCollection: {skip: "internal command"},
    _shardsvrCreateCollectionParticipant: {skip: "internal command"},
    _shardsvrDropCollection: {skip: "internal command"},
    _shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern: {skip: "internal command"},
    _shardsvrDropCollectionParticipant: {skip: "internal command"},
    _shardsvrUnregisterIndex: {skip: "internal command"},
    _shardsvrDropIndexCatalogEntryParticipant: {skip: "internal command"},
    _shardsvrDropIndexes: {skip: "internal command"},
    _shardsvrDropDatabase: {skip: "internal command"},
    _shardsvrDropDatabaseParticipant: {skip: "internal command"},
    _shardsvrEndMigrationBlockingOperation: {skip: "internal command"},
    _shardsvrGetStatsForBalancing: {skip: "internal command"},
    _shardsvrJoinDDLCoordinators: {skip: "internal command"},
    _shardsvrJoinMigrations: {skip: "internal command"},
    _shardsvrMergeAllChunksOnShard: {skip: "internal command"},
    _shardsvrMovePrimary: {skip: "internal command"},
    _shardsvrMovePrimaryEnterCriticalSection: {skip: "internal command"},
    _shardsvrMovePrimaryExitCriticalSection: {skip: "internal command"},
    _shardsvrMoveRange: {skip: "internal command"},
    _shardsvrNotifyShardingEvent: {skip: "internal command"},
    _shardsvrRefineCollectionShardKey: {skip: "internal command"},
    _shardsvrRenameCollection: {skip: "internal command"},
    _shardsvrRenameCollectionParticipant: {skip: "internal command"},
    _shardsvrRenameCollectionParticipantUnblock: {skip: "internal command"},
    _shardsvrRenameIndexMetadata: {skip: "internal command"},
    _shardsvrReshardCollection: {skip: "internal command"},
    _shardsvrReshardingDonorFetchFinalCollectionStats: {skip: "internal command"},
    _shardsvrReshardingDonorStartChangeStreamsMonitor: {skip: "internal command"},
    _shardsvrReshardingOperationTime: {skip: "internal command"},
    _shardsvrReshardRecipientClone: {skip: "internal command"},
    _shardsvrResolveView: {skip: "internal command"},
    _shardsvrRunSearchIndexCommand: {skip: "internal command"},
    _shardsvrSetAllowMigrations: {skip: "internal command"},
    _shardsvrSetClusterParameter: {skip: "internal command"},
    _shardsvrSetUserWriteBlockMode: {skip: "internal command"},
    _shardsvrValidateShardKeyCandidate: {skip: "internal command"},
    _shardsvrCollMod: {skip: "internal command"},
    _shardsvrCollModParticipant: {skip: "internal command"},
    _shardsvrConvertToCappedParticipant: {skip: "internal command"},
    _shardsvrParticipantBlock: {skip: "internal command"},
    _shardsvrUntrackUnsplittableCollection: {skip: "internal command"},
    _shardsvrFetchCollMetadata: {skip: "internal command"},
    streams_startStreamProcessor: {skip: "internal command"},
    streams_startStreamSample: {skip: "internal command"},
    streams_stopStreamProcessor: {skip: "internal command"},
    streams_listStreamProcessors: {skip: "internal command"},
    streams_getMoreStreamSample: {skip: "internal command"},
    streams_getStats: {skip: "internal command"},
    streams_testOnlyInsert: {skip: "internal command"},
    streams_getMetrics: {skip: "internal command"},
    streams_updateFeatureFlags: {skip: "internal command"},
    streams_testOnlyGetFeatureFlags: {skip: "internal command"},
    streams_writeCheckpoint: {skip: "internal command"},
    streams_sendEvent: {skip: "internal command"},
    streams_updateConnection: {skip: "internal command"},
    _transferMods: {skip: "internal command"},
    abortMoveCollection: {skip: "does not accept write concern"},
    abortReshardCollection: {skip: "does not accept write concern"},
    abortTransaction: {
        success: {
            // Basic abort transaction
            req: () => ({
                abortTransaction: 1,
                txnNumber: getTxnNumber(),
                autocommit: false,
                lsid: getLSID()
            }),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 0}));

                if (clusterType == "sharded" && bsonWoCompare(getShardKey(coll, fullNs), {}) == 0) {
                    // Set the primary shard to shard0 so we can assume that it's okay to run
                    // prepareTransaction on it
                    assert.commandWorked(coll.getDB().adminCommand(
                        {moveCollection: fullNs, toShard: cluster.shard0.shardName}));
                }

                assert.commandWorked(coll.getDB().runCommand({
                    insert: collName,
                    documents: [{_id: 1}],
                    lsid: getLSID(),
                    stmtIds: [NumberInt(0)],
                    txnNumber: getTxnNumber(),
                    startTransaction: true,
                    autocommit: false
                }));

                assert.commandWorked(coll.getDB().runCommand({
                    update: collName,
                    updates: [{q: {}, u: {$set: {a: 1}}}],
                    lsid: getLSID(),
                    stmtIds: [NumberInt(1)],
                    txnNumber: getTxnNumber(),
                    autocommit: false
                }));

                let primary =
                    clusterType == "sharded" ? cluster.rs0.getPrimary() : cluster.getPrimary();
                assert.commandWorked(primary.getDB(dbName).adminCommand({
                    prepareTransaction: 1,
                    lsid: getLSID(),
                    txnNumber: getTxnNumber(),
                    autocommit: false
                }));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                genNextTxnNumber();
            },
            admin: true,
        },
        failure: {
            // The transaction has already been committed
            req: () => ({
                abortTransaction: 1,
                txnNumber: getTxnNumber(),
                autocommit: false,
                lsid: getLSID()
            }),
            setupFunc: (coll) => {
                assert.commandWorked(coll.getDB().runCommand({
                    insert: collName,
                    documents: [{_id: 1}],
                    lsid: getLSID(),
                    stmtIds: [NumberInt(0)],
                    txnNumber: getTxnNumber(),
                    startTransaction: true,
                    autocommit: false
                }));
                assert.commandWorked(coll.getDB().adminCommand({
                    commitTransaction: 1,
                    lsid: getLSID(),
                    txnNumber: getTxnNumber(),
                    autocommit: false
                }));
            },
            confirmFunc: (res, coll) => {
                assert.commandFailedWithCode(res, ErrorCodes.TransactionCommitted);
                assert.eq(coll.find().itcount(), 1);
                genNextTxnNumber();
            },
            admin: true,
        },
    },
    abortUnshardCollection: {skip: "does not accept write concern"},
    addShard: {skip: "unrelated"},
    addShardToZone: {skip: "does not accept write concern"},
    aggregate: {
        noop: {
            // The pipeline will not match any docs, so nothing should be written to "out"
            req: {aggregate: collName, pipeline: [{$match: {x: 1}}, {$out: "out"}], cursor: {}},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1, x: 1}));
                assert.commandWorked(coll.remove({_id: 1}));
                assert.eq(coll.count({_id: 1}), 0);
                assert.commandWorked(coll.getDB().createCollection("out"));
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                assert.eq(coll.getDB().out.find().itcount(), 0);
                coll.getDB().out.drop();
            },
        },
        success: {
            // The pipeline should write a single doc to "out"
            req: {aggregate: collName, pipeline: [{$match: {_id: 1}}, {$out: "out"}], cursor: {}},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(coll.getDB().createCollection("out"));
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                    assert.eq(coll.getDB().out.find().itcount(), 0);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(coll.getDB().out.find().itcount(), 1);
                }
                assert.eq(coll.count({_id: 1}), 1);
                coll.getDB().out.drop();
            },
        },
        failure: {
            // Attempt to update _id to the same value and get a duplicate key error
            req: {aggregate: collName, pipeline: [{$set: {_id: 0}}, {$out: "out"}], cursor: {}},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(coll.insert({_id: 2}));
                assert.commandWorked(coll.getDB().createCollection("out"));
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
                }
                assert.eq(coll.getDB().out.find().itcount(), 0);
                assert.eq(coll.count({_id: 1}), 1);
                coll.getDB().out.drop();
            },
        },
    },
    // TODO SPM-2513 Define test case for analyze once the command is user facing
    analyze: {skip: "internal only"},
    analyzeShardKey: {skip: "does not accept write concern"},
    appendOplogNote: {
        success: {
            // appendOplogNote basic
            req: {appendOplogNote: 1, data: {x: 1}},
            setupFunc: (coll) => {},
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
            },
            admin: true,
        },
    },
    applyOps: {
        noop: {
            // 'applyOps' where the update is a no-op
            req: {applyOps: [{op: "u", ns: fullNs, o: {_id: 1}, o2: {_id: 1}}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.applied, 1);
                assert.eq(res.results[0], true);
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.count({_id: 1}), 1);
            },
        },
        success: {
            // 'applyOps' basic insert
            req: {applyOps: [{op: "i", ns: fullNs, o: {_id: 2, x: 3}}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1, x: 3}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.applied, 1);
                assert.eq(res.results[0], true);
                assert.eq(coll.find().itcount(), 2);
                assert.eq(coll.count({x: 3}), 2);
            },
        },
        failure: {
            // 'applyOps' attempt to update immutable field
            req: {applyOps: [{op: "u", ns: fullNs, o: {_id: 2}, o2: {_id: 1}}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1, x: 1}));
            },
            confirmFunc: (res, coll) => {
                assert.eq(res.applied, 1);
                assert.eq(res.ok, 0);
                assert.eq(res.results[0], false);
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.count({_id: 1}), 1);
            },
        },
    },
    authenticate: {skip: "does not accept write concern"},
    autoCompact: {skip: "does not accept write concern"},
    autoSplitVector: {skip: "does not accept write concern"},
    balancerCollectionStatus: {skip: "does not accept write concern"},
    balancerStart: {skip: "does not accept write concern"},
    balancerStatus: {skip: "does not accept write concern"},
    balancerStop: {skip: "does not accept write concern"},
    buildInfo: {skip: "does not accept write concern"},
    bulkWrite: {
        noop: {
            // The doc to update doesn't exist
            req: {
                bulkWrite: 1,
                ops: [{update: 0, filter: {_id: 0}, updateMods: {$set: {x: 1}}}],
                nsInfo: [{ns: fullNs}]
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1, x: 1}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.nErrors, 0);
                assert.eq(res.nModified, 0);
            },
            admin: true,
        },
        success: {
            // Basic insert in a bulk write
            req: {bulkWrite: 1, ops: [{insert: 0, document: {_id: 2}}], nsInfo: [{ns: fullNs}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1, x: 3}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.nErrors, 0);
                assert.eq(res.nInserted, 1);
            },
            admin: true,
        },
        failure: {
            // Attempt to insert doc with duplicate _id
            req: {bulkWrite: 1, ops: [{insert: 0, document: {_id: 1}}], nsInfo: [{ns: fullNs}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1, x: 3}));
            },
            confirmFunc: (res, coll) => {
                assert.eq(res.nErrors, 1);
                assert(res.cursor && res.cursor.firstBatch && res.cursor.firstBatch.length == 1);
                assert.commandFailedWithCode(res.cursor.firstBatch[0], ErrorCodes.DuplicateKey);
            },
            admin: true,
        },
    },
    changePrimary: {
        noop: {
            // The destination shard is already the primary shard for the db
            req: (cluster) => ({changePrimary: dbName, to: getShardNames(cluster)[0]}),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(coll.getDB().adminCommand(
                    {changePrimary: dbName, to: getShardNames(cluster)[0]}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            admin: true,
        },
        success: {
            // Basic change primary
            req: (cluster) => ({changePrimary: dbName, to: getShardNames(cluster)[1]}),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(coll.getDB().adminCommand(
                    {changePrimary: dbName, to: getShardNames(cluster)[0]}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard1.shardName);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);

                // Change the primary back
                assert.commandWorked(coll.getDB().adminCommand(
                    {changePrimary: dbName, to: cluster.shard0.shardName}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);
            },
            admin: true,
        },
    },
    checkMetadataConsistency: {skip: "does not accept write concern"},
    checkShardingIndex: {skip: "does not accept write concern"},
    cleanupOrphaned: {skip: "only exist on direct shard connection"},
    cleanupReshardCollection: {skip: "does not accept write concern"},
    cleanupStructuredEncryptionData: {skip: "does not accept write concern"},
    clearJumboFlag: {skip: "does not accept write concern"},
    clearLog: {skip: "does not accept write concern"},
    cloneCollectionAsCapped: {
        success: {
            // Basic cloneColectionAsCapped
            req: {
                cloneCollectionAsCapped: collName,
                toCollection: collName + "2",
                size: 10 * 1024 * 1024
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.eq(coll.getDB().getCollection(collName + "2").find().itcount(), 0);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB().getCollection(collName + "2").find().itcount(), 1);
                coll.getDB().getCollection(collName + "2").drop();
            },
        },
        failure: {
            // The destination collection already exists
            req: {
                cloneCollectionAsCapped: collName,
                toCollection: collName + "2",
                size: 10 * 1024 * 1024
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(coll.getDB().getCollection(collName + "2").insert({_id: 1}));
            },
            confirmFunc: (res, coll) => {
                assert.commandFailedWithCode(res, ErrorCodes.NamespaceExists);
                assert.eq(coll.find().itcount(), 1);
            },
        },
    },
    clusterAbortTransaction: {skip: "already tested by 'abortTransaction' tests on mongos"},
    clusterAggregate: {skip: "already tested by 'aggregate' tests on mongos"},
    clusterBulkWrite: {skip: "already tested by 'bulkWrite' tests on mongos"},
    clusterCommitTransaction: {skip: "already tested by 'commitTransaction' tests on mongos"},
    clusterCount: {skip: "already tested by 'count' tests on mongos"},
    clusterDelete: {skip: "already tested by 'delete' tests on mongos"},
    clusterFind: {skip: "already tested by 'find' tests on mongos"},
    clusterGetMore: {skip: "already tested by 'getMore' tests on mongos"},
    clusterInsert: {skip: "already tested by 'insert' tests on mongos"},
    clusterUpdate: {skip: "already tested by 'update' tests on mongos"},
    collMod: {
        noop: {
            // Validator already non-existent
            req: {collMod: collName, validator: {}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(coll.getDB().runCommand({collMod: collName, validator: {}}));
                assert.eq(coll.getDB().getCollectionInfos({name: collName})[0].options.validator,
                          undefined);
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB().getCollectionInfos({name: collName})[0].options.validator,
                          undefined);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        success: {
            // Add validator
            req: {collMod: collName, validator: {x: {$exists: true}}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1, x: 1}));
                assert.eq(coll.getDB().getCollectionInfos({name: collName})[0].options.validator,
                          undefined);
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB().getCollectionInfos({name: collName})[0].options.validator,
                          {x: {$exists: true}});
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        failure: {
            // Index to be updated does not exist
            req: {collMod: collName, index: {name: "a_1", hidden: true}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{key: {a: 1}, name: "a_1"}],
                    commitQuorum: "majority"
                }));
                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand(
                    {dropIndexes: collName, index: "a_1"},
                    ));
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.IndexNotFound);
                assert.eq(coll.getDB().getCollectionInfos({name: collName})[0].options.validator,
                          undefined);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    collStats: {skip: "does not accept write concern"},
    commitReshardCollection: {skip: "does not accept write concern"},
    commitTransaction: {
        noop: {
            // The transaction is already committed
            req: () => ({
                commitTransaction: 1,
                txnNumber: getTxnNumber(),
                autocommit: false,
                lsid: getLSID()
            }),
            setupFunc: (coll) => {
                assert.commandWorked(coll.getDB().runCommand({
                    insert: collName,
                    documents: [{_id: 1}],
                    lsid: getLSID(),
                    stmtIds: [NumberInt(0)],
                    txnNumber: getTxnNumber(),
                    startTransaction: true,
                    autocommit: false
                }));
                assert.commandWorked(coll.getDB().adminCommand({
                    commitTransaction: 1,
                    txnNumber: getTxnNumber(),
                    autocommit: false,
                    lsid: getLSID()
                }));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                genNextTxnNumber();
            },
            admin: true,
        },
        success: {
            // Basic commit transaction
            req: () => ({
                commitTransaction: 1,
                txnNumber: getTxnNumber(),
                autocommit: false,
                lsid: getLSID()
            }),
            setupFunc: (coll) => {
                assert.commandWorked(coll.getDB().runCommand({
                    insert: collName,
                    documents: [{_id: 1}],
                    lsid: getLSID(),
                    stmtIds: [NumberInt(0)],
                    txnNumber: getTxnNumber(),
                    startTransaction: true,
                    autocommit: false
                }));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.find().itcount(), 1);
                genNextTxnNumber();
            },
            admin: true,
        },
    },
    compact: {skip: "does not accept write concern"},
    compactStructuredEncryptionData: {skip: "does not accept write concern"},
    configureCollectionBalancing: {skip: "does not accept write concern"},
    configureFailPoint: {skip: "internal command"},
    configureQueryAnalyzer: {skip: "does not accept write concern"},
    connPoolStats: {skip: "does not accept write concern"},
    connPoolSync: {skip: "internal command"},
    connectionStatus: {skip: "does not accept write concern"},
    convertToCapped: {
        noop: {
            // Collection is already capped
            req: {convertToCapped: collName, size: 10 * 1024 * 1024},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(
                    coll.runCommand({convertToCapped: collName, size: 10 * 1024 * 1024}));
                assert.eq(coll.stats().capped, true);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.stats().capped, true);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        success: {
            // Basic convertToCapped
            req: {convertToCapped: collName, size: 10 * 1024 * 1024},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.eq(coll.stats().capped, false);
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.stats().capped, true);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        failure: {
            // The collection doesn't exist
            req: {convertToCapped: collName, size: 10 * 1024 * 1024},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().runCommand({drop: collName}));
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.NamespaceNotFound);
                assert.eq(coll.find().itcount(), 0);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    coordinateCommitTransaction: {skip: "internal command"},
    count: {skip: "does not accept write concern"},
    cpuload: {skip: "does not accept write concern"},
    create: {
        noop: {
            // Coll already exists
            req: {create: collName},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().runCommand({create: collName}));
                assert.eq(coll.getDB().getCollectionInfos({name: collName}).length, 1);
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.getDB().getCollectionInfos({name: collName}).length, 1);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        success: {
            // Basic create coll
            req: {create: collName},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                // Ensure DB exists, but create a different collection
                assert.commandWorked(coll.getDB().coll2.insert({_id: 1}));

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.getDB().getCollectionInfos({name: collName}).length, 1);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        failure: {
            // Attempt to create a view and output to a nonexistent collection
            req: {create: "viewWithOut", viewOn: collName, pipeline: [{$out: "nonexistentColl"}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(coll.getDB().runCommand({drop: "nonexistentColl"}));
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.OptionNotSupportedOnView);
                }
                assert.eq(coll.find().itcount(), 1);
                assert(!coll.getDB().getCollectionNames().includes("nonexistentColl"));

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    createIndexes: {
        // All voting data bearing nodes are not up for this test. So 'createIndexes' command
        // can't succeed with the default index commitQuorum value "votingMembers". So, we run
        // createIndexes using commit quorum "majority".
        noop: {
            // Index already exists
            req: {
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
                commitQuorum: "majority"
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{key: {a: 1}, name: "a_1"}],
                    commitQuorum: "majority"
                }));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                let details = res;
                if ("raw" in details) {
                    const raw = details.raw;
                    details = raw[Object.keys(raw)[0]];
                }
                assert.eq(details.numIndexesBefore, details.numIndexesAfter);
                assert.eq(details.note, 'all indexes already exist');
            },
        },
        success: {
            // Basic create index
            req: {
                createIndexes: collName,
                indexes: [{key: {b: 1}, name: "b_1"}],
                commitQuorum: "majority"
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({b: 1}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                let details = res;
                if ("raw" in details) {
                    const raw = details.raw;
                    details = raw[Object.keys(raw)[0]];
                }
                assert.eq(details.numIndexesBefore, details.numIndexesAfter - 1);
            },
        },
        failure: {
            // Attempt to create two indexes with the same name and different keys
            req: {
                createIndexes: collName,
                indexes: [{key: {b: 1}, name: "b_1"}],
                commitQuorum: "majority"
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({b: 1}));
                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{key: {b: 1}, name: "b_1"}],
                    commitQuorum: "majority"
                }));
            },
            confirmFunc: (res, coll) => {
                assert.commandFailedWithCode(res, ErrorCodes.IndexKeySpecsConflict);
            },
        },
    },
    createRole: {
        targetConfigServer: true,
        success: {
            // Basic create role
            req: {createRole: "foo", privileges: [], roles: []},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs, st) => {
                assert.eq(coll.getDB().getRole("foo"), null);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc:
                (res, coll, cluster, clusterType, secondariesRunning, optionalArgs, st) => {
                    if (clusterType == "sharded") {
                        assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                        cluster.configRS.restart(secondariesRunning[0]);
                    } else {
                        assert.commandWorkedIgnoringWriteConcernErrors(res);
                        assert.neq(coll.getDB().getRole("foo"), null);
                    }
                    coll.getDB().runCommand({dropRole: "foo"});
                },
        },
        failure: {
            // Role already exists
            req: {createRole: "foo", privileges: [], roles: []},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs, st) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.neq(coll.getDB().getRole("foo"), null);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createUser: "fakeusr",
                        pwd: "bar",
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc:
                (res, coll, cluster, clusterType, secondariesRunning, optionalArgs, st) => {
                    if (clusterType == "sharded") {
                        assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                        cluster.configRS.restart(secondariesRunning[0]);
                        coll.getDB().runCommand({dropUser: "fakeusr"});
                    } else {
                        assert.commandFailedWithCode(res, 51002);
                    }
                    assert.neq(coll.getDB().getRole("foo"), null);
                    coll.getDB().runCommand({dropRole: "foo"});
                },
        },
    },
    createSearchIndexes: {skip: "does not accept write concern"},
    createUnsplittableCollection: {skip: "internal command"},
    createUser: {
        targetConfigServer: true,
        success: {
            // Basic create user
            req: {createUser: "foo", pwd: "bar", roles: []},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs, st) => {
                assert.eq(coll.getDB().getUser("foo"), null);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc:
                (res, coll, cluster, clusterType, secondariesRunning, optionalArgs, st) => {
                    if (clusterType == "sharded") {
                        assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                        cluster.configRS.restart(secondariesRunning[0]);
                    } else {
                        assert.commandWorkedIgnoringWriteConcernErrors(res);
                        assert.neq(coll.getDB().getUser("foo"), null);
                    }

                    coll.getDB().runCommand({dropUser: "foo"});
                },
        },
        failure: {
            // User already exists
            req: {createUser: "foo", pwd: "bar", roles: []},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs, st) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "bar", roles: []}));
                assert.neq(coll.getDB().getUser("foo"), null);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createRole: "bar",
                        privileges: [],
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc:
                (res, coll, cluster, clusterType, secondariesRunning, optionalArgs, st) => {
                    if (clusterType == "sharded") {
                        assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                        cluster.configRS.restart(secondariesRunning[0]);
                        coll.getDB().runCommand({dropRole: "bar"});
                    } else {
                        assert.commandFailedWithCode(res, 51003);
                    }
                    assert.neq(coll.getDB().getUser("foo"), null);
                    coll.getDB().runCommand({dropUser: "foo"});
                },
        },
    },
    currentOp: {skip: "does not accept write concern"},
    dataSize: {skip: "does not accept write concern"},
    dbCheck: {skip: "does not accept write concern"},
    dbHash: {skip: "does not accept write concern"},
    dbStats: {skip: "does not accept write concern"},
    delete: {
        noop: {
            // The query will not match any doc
            req: {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(coll.remove({_id: 1}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 0);
                assert.eq(coll.count({a: 1}), 0);
            },
        },
        success: {
            req: {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 1);
                assert.eq(coll.find().itcount(), 0);
            },
        },
    },
    distinct: {skip: "does not accept write concern"},
    drop: {
        noop: {
            // The collection has already been dropped
            req: {drop: collName},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(coll.getDB().runCommand({drop: collName}));
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        success: {
            // Basic drop collection
            req: {drop: collName},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({a: 1}));

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.find().itcount(), 0);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    dropAllRolesFromDatabase: {
        targetConfigServer: true,
        noop: {
            // No roles exist
            req: {dropAllRolesFromDatabase: 1},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.eq(coll.getDB().getRoles({rolesInfo: 1}).length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                assert.eq(coll.getDB().getRoles({rolesInfo: 1}).length, 0);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                }
            },
        },
        success: {
            // Basic dropAllRolesFromDatabase
            req: {dropAllRolesFromDatabase: 1},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(coll.getDB().getRoles({rolesInfo: 1}).length, 0);
                }
            },
        },
    },
    dropAllUsersFromDatabase: {
        targetConfigServer: true,
        noop: {
            // No users exist
            req: {dropAllUsersFromDatabase: 1},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.eq(coll.getDB().getUsers().length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createRole: "bar",
                        privileges: [],
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                assert.eq(coll.getDB().getUsers().length, 0);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().runCommand({dropRole: "bar"});
                }
            },
        },
        success: {
            // Basic dropAllUsersFromDatabase
            req: {dropAllUsersFromDatabase: 1},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "bar", roles: []}));

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(coll.getDB().getUsers().length, 0);
                }
            },
        },
    },
    dropConnections: {skip: "does not accept write concern"},
    dropDatabase: {
        noop: {
            // Database has already been dropped
            req: {dropDatabase: 1},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorkedIgnoringWriteConcernErrors(
                    coll.getDB().runCommand({dropDatabase: 1}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
            },
        },
        success: {
            // Basic drop database
            req: {dropDatabase: 1},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({a: 1}));
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    dropIndexes: {
        noop: {
            // Passing "*" will drop all indexes except the _id index. The only index on this
            // collection is the _id index, so the command will be a no-op.
            req: {
                dropIndexes: collName,
                index: "*",
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({a: "a"}));
                assert.eq(coll.getIndexes().length, 1);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                let details = res;
                if ("raw" in details) {
                    const raw = details.raw;
                    details = raw[Object.keys(raw)[0]];
                }
                assert.eq(coll.getIndexes().length, 1);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        success: {
            // Basic drop index
            req: {
                dropIndexes: collName,
                index: "b_1",
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({b: "b"}));

                const numIndexesBefore = coll.getIndexes().length;
                optionalArgs.numIndexesBefore = numIndexesBefore;

                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{key: {b: 1}, name: "b_1"}],
                    commitQuorum: "majority"
                }));

                assert.eq(coll.getIndexes().length, numIndexesBefore + 1);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                let details = res;
                if ("raw" in details) {
                    const raw = details.raw;
                    details = raw[Object.keys(raw)[0]];
                }
                assert.eq(coll.getIndexes().length, optionalArgs.numIndexesBefore);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    dropRole: {
        targetConfigServer: true,
        success: {
            // Basic dropRole
            req: {dropRole: "foo"},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.eq(coll.getDB().getRoles().length, 1);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(coll.getDB().getRoles().length, 0);
                }
            },
        },
        failure: {
            // Role does not exist
            req: {dropRole: "foo"},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.eq(coll.getDB().getRoles().length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createRole: "bar",
                        privileges: [],
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.RoleNotFound);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().runCommand({dropRole: "bar"});
                }
                assert.eq(coll.getDB().getRoles().length, 0);
            },
        },
    },
    dropSearchIndex: {skip: "does not accept write concern"},
    dropUser: {
        targetConfigServer: true,
        success: {
            // Basic dropUser
            req: {dropUser: "foo"},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "bar", roles: []}));
                assert.eq(coll.getDB().getUsers().length, 1);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(coll.getDB().getUsers().length, 0);
                }
            },
        },
        failure: {
            // User doesn't exist
            req: {dropUser: "foo"},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.eq(coll.getDB().getUsers({filter: {user: "foo"}}).length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createRole: "bar",
                        privileges: [],
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.UserNotFound);
                assert.eq(coll.getDB().getUsers({filter: {user: "foo"}}).length, 0);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().runCommand({dropRole: "bar"});
                }
            },
        },
    },
    echo: {skip: "does not accept write concern"},
    enableSharding: {
        targetConfigServer: true,
        noop: {
            // Database creation only acknowledged by the config primary node
            req: {enableSharding: dbName},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().runCommand({dropDatabase: 1}));

                // TODO SERVER-97754 Do not stop the remaining secondary once enableSharding no
                // longer override user provided writeConcern
                cluster.configRS.stop(secondariesRunning[0]);

                // `enableSharding` throws the WCE as top-level error when majority WC is not
                // available
                assert.commandFailedWithCode(coll.getDB().adminCommand({enableSharding: dbName}),
                                             ErrorCodes.WriteConcernTimeout);

                // The database exists on the config primary node
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .databases
                              .find({_id: dbName},
                                    {},  // project
                                    {readConcern: {level: 1}, $readPreference: {mode: 'primary'}})
                              .itcount(),
                          1);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                cluster.configRS.restart(secondariesRunning[0]);
            },
            admin: true,
        },
        success: {
            // Basic enable sharding
            req: {enableSharding: dbName},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.eq(
                    coll.getDB().getSiblingDB("config").databases.find({_id: dbName}).itcount(), 1);
                assert.commandWorked(coll.getDB().runCommand({dropDatabase: 1}));
                assert.eq(
                    coll.getDB().getSiblingDB("config").databases.find({_id: dbName}).itcount(), 0);

                // TODO SERVER-97754 Do not stop the remaining secondary once enableSharding no
                // longer override user provided writeConcern
                cluster.configRS.stop(secondariesRunning[0]);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                cluster.configRS.restart(secondariesRunning[0]);
            },
            admin: true,
        },
    },
    endSessions: {skip: "does not accept write concern"},
    explain: {skip: "does not accept write concern"},
    features: {skip: "does not accept write concern"},
    filemd5: {skip: "does not accept write concern"},
    find: {skip: "does not accept write concern"},
    findAndModify: {
        noop: {
            // The doc to update does not exist
            req: {findAndModify: collName, query: {_id: 1}, update: {b: 2}},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(coll.remove({_id: 1}));
            },
            confirmFunc: (res, coll) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.find().itcount(), 0);
                assert.eq(coll.count({b: 2}), 0);
            },
        },
        success: {
            // Basic findOneAndReplace
            req: {findAndModify: collName, query: {_id: 1}, update: {_id: 1, b: 2}},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1}));
            },
            confirmFunc: (res, coll) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.find().itcount(), 1);
            },
        },
        failure: {
            // Attempt to update and cause document validation error
            req: {findAndModify: collName, query: {_id: 1}, update: {_id: 1}},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1, x: 1}));
                assert.commandWorked(
                    coll.getDB().runCommand({collMod: collName, validator: {x: {$exists: true}}}));
            },
            confirmFunc: (res, coll) => {
                assert.commandFailedWithCode(res, ErrorCodes.DocumentValidationFailure);
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.count({_id: 1}), 1);
            },
        }
    },
    flushRouterConfig: {skip: "does not accept write concern"},
    forceerror: {skip: "test command"},
    fsync: {skip: "does not accept write concern"},
    fsyncUnlock: {skip: "does not accept write concern"},
    getAuditConfig: {skip: "does not accept write concern"},
    getChangeStreamState: {skip: "does not accept write concern"},
    getClusterParameter: {skip: "does not accept write concern"},
    getCmdLineOpts: {skip: "does not accept write concern"},
    getDatabaseVersion: {skip: "internal command"},
    getDefaultRWConcern: {skip: "does not accept write concern"},
    getDiagnosticData: {skip: "does not accept write concern"},
    getLog: {skip: "does not accept write concern"},
    getMore: {skip: "does not accept write concern"},
    getParameter: {skip: "does not accept write concern"},
    getQueryableEncryptionCountInfo: {skip: "does not accept write concern"},
    getShardMap: {skip: "internal command"},
    getShardVersion: {skip: "internal command"},
    godinsert: {skip: "for testing only"},
    grantPrivilegesToRole: {
        targetConfigServer: true,
        noop: {
            // Role already has privilege
            req: {
                grantPrivilegesToRole: "foo",
                privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}]
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(coll.getDB().runCommand({
                    grantPrivilegesToRole: "foo",
                    privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}]
                }));

                let role = coll.getDB().getRoles({rolesInfo: 1, showPrivileges: true});
                assert.eq(role.length, 1);
                assert.eq(role[0].privileges.length, 1);
                assert.eq(role[0].privileges[0].actions, ["find"]);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createUser: "fakeusr",
                        pwd: "bar",
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                let role = coll.getDB().getRoles({rolesInfo: 1, showPrivileges: true});
                assert.eq(role.length, 1);
                assert.eq(role[0].privileges.length, 1);
                assert.eq(role[0].privileges[0].actions, ["find"]);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropUser("fakeusr");
                }

                coll.getDB().dropRole("foo");
            },
        },
        success: {
            // Basic grantPrivilegesToRole
            req: {
                grantPrivilegesToRole: "foo",
                privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}]
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));

                let role = coll.getDB().getRoles({rolesInfo: 1, showPrivileges: true});
                assert.eq(role.length, 1);
                assert.eq(role.length, 1);
                assert.eq(role[0].privileges.length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);

                    let role = coll.getDB().getRoles({rolesInfo: 1, showPrivileges: true});
                    assert.eq(role.length, 1);
                    assert.eq(role[0].privileges.length, 1);
                    assert.eq(role[0].privileges[0].actions, ["find"]);
                }

                coll.getDB().dropRole("foo");
            },
        },
    },
    grantRolesToRole: {
        targetConfigServer: true,
        noop: {
            // Foo already has role bar
            req: {grantRolesToRole: "foo", roles: [{role: "bar", db: dbName}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(coll.getDB().runCommand({
                    createRole: "bar",
                    privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}],
                    roles: []
                }));
                assert.commandWorked(coll.getDB().runCommand(
                    {grantRolesToRole: "foo", roles: [{role: "bar", db: dbName}]}));

                let role = coll.getDB().getRole("foo");
                assert.eq(role.inheritedRoles.length, 1);
                assert.eq(role.inheritedRoles[0].role, "bar");

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createUser: "fakeusr",
                        pwd: "bar",
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                let role = coll.getDB().getRole("foo");
                assert.eq(role.inheritedRoles.length, 1);
                assert.eq(role.inheritedRoles[0].role, "bar");

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropUser("fakeusr");
                }

                coll.getDB().dropRole("foo");
                coll.getDB().dropRole("bar");
            },
        },
        success: {
            // Basic grantRolesToRole
            req: {grantRolesToRole: "foo", roles: [{role: "bar", db: dbName}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(coll.getDB().runCommand({
                    createRole: "bar",
                    privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}],
                    roles: []
                }));

                let role = coll.getDB().getRole("foo");
                assert.eq(role.inheritedRoles.length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    let role = coll.getDB().getRole("foo");
                    assert.eq(role.inheritedRoles.length, 1);
                    assert.eq(role.inheritedRoles[0].role, "bar");
                }

                coll.getDB().dropRole("foo");
                coll.getDB().dropRole("bar");
            },
        },
    },
    grantRolesToUser: {
        targetConfigServer: true,
        noop: {
            // User already has role
            req: {grantRolesToUser: "foo", roles: [{role: "foo", db: dbName}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "bar", roles: []}));
                assert.commandWorked(coll.getDB().runCommand(
                    {grantRolesToUser: "foo", roles: [{role: "foo", db: dbName}]}));

                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 1);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createRole: "bar",
                        privileges: [],
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 1);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropRole("bar");
                }
                coll.getDB().dropRole("foo");
                coll.getDB().runCommand({dropUser: "foo"});
            },
        },
        success: {
            // Basic grantRolesToUser
            req: {grantRolesToUser: "foo", roles: [{role: "foo", db: dbName}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "bar", roles: []}));

                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    let user = coll.getDB().getUser("foo");
                    assert.eq(user.roles.length, 1);
                }

                coll.getDB().dropRole("foo");
                coll.getDB().runCommand({dropUser: "foo"});
            },
        },
        failure: {
            // Role does not exist
            req: {grantRolesToUser: "foo", roles: ["fakeRole"]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "bar", roles: []}));

                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createRole: "bar",
                        privileges: [],
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.RoleNotFound);
                }
                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 0);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropRole("bar");
                }
                coll.getDB().dropRole("foo");
                coll.getDB().runCommand({dropUser: "foo"});
            },
        },
    },
    handshake: {skip: "does not accept write concern"},
    hello: {skip: "does not accept write concern"},
    hostInfo: {skip: "does not accept write concern"},
    httpClientRequest: {skip: "does not accept write concern"},
    exportCollection: {skip: "internal command"},
    importCollection: {skip: "internal command"},
    insert: {
        // A no-op insert that returns success is not possible
        success: {
            // Basic insert
            req: {insert: collName, documents: [{_id: 11}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 10}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 1);
                assert.eq(coll.find().itcount(), 2);
            },
        },
        failure: {
            // Insert causes DuplicateKeyError
            req: {insert: collName, documents: [{_id: 10}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 10}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.writeErrors && res.writeErrors.length == 1);
                assert.eq(res.writeErrors[0].code, ErrorCodes.DuplicateKey);
                assert.eq(res.n, 0);
                assert.eq(coll.count({_id: 10}), 1);
            },
        }
    },
    internalRenameIfOptionsAndIndexesMatch: {skip: "internal command"},
    invalidateUserCache: {skip: "does not accept write concern"},
    isdbgrid: {skip: "does not accept write concern"},
    isMaster: {skip: "does not accept write concern"},
    killAllSessions: {skip: "does not accept write concern"},
    killAllSessionsByPattern: {skip: "does not accept write concern"},
    killCursors: {skip: "does not accept write concern"},
    killOp: {skip: "does not accept write concern"},
    killSessions: {skip: "does not accept write concern"},
    listCollections: {skip: "does not accept write concern"},
    listCommands: {skip: "does not accept write concern"},
    listDatabases: {skip: "does not accept write concern"},
    listDatabasesForAllTenants: {skip: "does not accept write concern"},
    listIndexes: {skip: "does not accept write concern"},
    listSearchIndexes: {skip: "does not accept write concern"},
    listShards: {skip: "does not accept write concern"},
    lockInfo: {skip: "does not accept write concern"},
    logApplicationMessage: {skip: "does not accept write concern"},
    logMessage: {skip: "does not accept write concern"},
    logRotate: {skip: "does not accept write concern"},
    logout: {skip: "does not accept write concern"},
    makeSnapshot: {skip: "does not accept write concern"},
    mapReduce: {skip: "deprecated"},
    mergeAllChunksOnShard: {skip: "does not accept write concern"},
    mergeChunks: {skip: "does not accept write concern"},
    moveChunk: {
        targetConfigServer: true,
        success: {
            // Basic moveChunk
            req: (cluster, coll) => ({
                moveChunk: fullNs,
                find: getShardKeyMinRanges(coll)[0]["min"],
                to: getShardNames(cluster)[0],
                secondaryThrottle: true
            }),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                let keyToMove = getShardKeyMinRanges(coll)[0]["min"];
                optionalArgs.originalShard =
                    coll.getDB()
                        .getSiblingDB("config")
                        .chunks.findOne({uuid: coll.getUUID(), min: keyToMove})
                        .shard;
                let destShard = getShardNames(cluster)[1];

                assert.commandWorked(
                    coll.getDB().adminCommand({moveChunk: fullNs, find: keyToMove, to: destShard}));
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .chunks.findOne({uuid: coll.getUUID(), min: keyToMove})
                              .shard,
                          destShard);

                // TODO SERVER-97754 Do not stop the remaining secondary once moveChunk no
                // longer override user provided writeConcern
                cluster.configRS.stop(secondariesRunning[0]);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                let keyMoved = getShardKeyMinRanges(coll)[0]["min"];
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .chunks.findOne({uuid: coll.getUUID(), min: keyMoved})
                              .shard,
                          getShardNames(cluster)[1]);

                cluster.configRS.restart(secondariesRunning[0]);

                assert.commandWorked(coll.getDB().adminCommand(
                    {moveChunk: fullNs, find: keyMoved, to: optionalArgs.originalShard}));
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .chunks.findOne({uuid: coll.getUUID(), min: keyMoved})
                              .shard,
                          optionalArgs.originalShard);
            },
            admin: true,
        },
    },
    moveCollection: {skip: "does not accept write concern"},
    movePrimary: {
        noop: {
            // The destination shard is already the primary shard for the db
            req: (cluster) => ({movePrimary: dbName, to: getShardNames(cluster)[0]}),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(coll.getDB().adminCommand(
                    {movePrimary: dbName, to: getShardNames(cluster)[0]}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            admin: true,
        },
        success: {
            // Basic movePrimary
            req: (cluster) => ({movePrimary: dbName, to: getShardNames(cluster)[1]}),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.commandWorked(coll.getDB().adminCommand(
                    {movePrimary: dbName, to: getShardNames(cluster)[0]}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);

                // Change the primary back
                assert.commandWorked(
                    coll.getDB().adminCommand({movePrimary: dbName, to: cluster.shard0.shardName}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);
            },
            admin: true,
        },
    },
    moveRange: {
        targetConfigServer: true,
        noop: {
            // Chunk already lives on this shard
            req: (cluster, coll) => ({
                moveRange: fullNs,
                min: getShardKeyMinRanges(coll)[0]["min"],
                toShard: getShardNames(cluster)[0]
            }),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                let keyToMove = getShardKeyMinRanges(coll)[0]["min"];
                optionalArgs.originalShard =
                    coll.getDB()
                        .getSiblingDB("config")
                        .chunks.findOne({uuid: coll.getUUID(), min: keyToMove})
                        .shard;
                let destShard = getShardNames(cluster)[0];

                assert.commandWorked(coll.getDB().adminCommand(
                    {moveRange: fullNs, min: keyToMove, toShard: destShard}));
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .chunks.findOne({uuid: coll.getUUID(), min: keyToMove})
                              .shard,
                          destShard);

                // TODO SERVER-97754 Do not stop the remaining secondary once moveChunk no
                // longer override user provided writeConcern
                cluster.configRS.stop(secondariesRunning[0]);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                let keyMoved = getShardKeyMinRanges(coll)[0]["min"];
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .chunks.findOne({uuid: coll.getUUID(), min: keyMoved})
                              .shard,
                          getShardNames(cluster)[0]);

                cluster.configRS.restart(secondariesRunning[0]);

                assert.commandWorked(coll.getDB().adminCommand(
                    {moveRange: fullNs, min: keyMoved, toShard: optionalArgs.originalShard}));
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .chunks.findOne({uuid: coll.getUUID(), min: keyMoved})
                              .shard,
                          optionalArgs.originalShard);
            },
            admin: true,
        },
        success: {
            // Basic moveRange
            req: (cluster, coll) => ({
                moveRange: fullNs,
                min: getShardKeyMinRanges(coll)[0]["min"],
                toShard: getShardNames(cluster)[0],
                secondaryThrottle: true
            }),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                let keyToMove = getShardKeyMinRanges(coll)[0]["min"];
                optionalArgs.originalShard =
                    coll.getDB()
                        .getSiblingDB("config")
                        .chunks.findOne({uuid: coll.getUUID(), min: keyToMove})
                        .shard;
                let destShard = getShardNames(cluster)[1];

                assert.commandWorked(coll.getDB().adminCommand(
                    {moveRange: fullNs, min: keyToMove, toShard: destShard}));
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .chunks.findOne({uuid: coll.getUUID(), min: keyToMove})
                              .shard,
                          destShard);

                // TODO SERVER-97754 Do not stop the remaining secondary once moveChunk no
                // longer override user provided writeConcern
                cluster.configRS.stop(secondariesRunning[0]);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                let keyMoved = getShardKeyMinRanges(coll)[0]["min"];
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .chunks.findOne({uuid: coll.getUUID(), min: keyMoved})
                              .shard,
                          getShardNames(cluster)[1]);

                cluster.configRS.restart(secondariesRunning[0]);

                assert.commandWorked(coll.getDB().adminCommand(
                    {moveRange: fullNs, min: keyMoved, toShard: optionalArgs.originalShard}));
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .chunks.findOne({uuid: coll.getUUID(), min: keyMoved})
                              .shard,
                          optionalArgs.originalShard);
            },
            admin: true,
        },
    },
    multicast: {skip: "does not accept write concern"},
    netstat: {skip: "internal command"},
    oidcListKeys: {skip: "does not accept write concern"},
    oidcRefreshKeys: {skip: "does not accept write concern"},
    pinHistoryReplicated: {skip: "internal command"},
    ping: {skip: "does not accept write concern"},
    planCacheClear: {skip: "does not accept write concern"},
    planCacheClearFilters: {skip: "does not accept write concern"},
    planCacheListFilters: {skip: "does not accept write concern"},
    planCacheSetFilter: {skip: "does not accept write concern"},
    prepareTransaction: {skip: "internal command"},
    profile: {skip: "does not accept write concern"},
    reIndex: {skip: "does not accept write concern"},
    reapLogicalSessionCacheNow: {skip: "does not accept write concern"},
    refineCollectionShardKey: {
        noop: {
            // Refine to same shard key
            req: (cluster, coll) =>
                ({refineCollectionShardKey: fullNs, key: getShardKey(coll, fullNs)}),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({x: 1}));

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            admin: true,
        },
        success: {
            // Add additional field to shard key
            req: (cluster, coll) => ({
                refineCollectionShardKey: fullNs,
                key: Object.assign({}, getShardKey(coll, fullNs), {newSkField: 1})
            }),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                let sk = getShardKey(coll, fullNs);
                optionalArgs.origSk = sk;
                assert.eq(bsonWoCompare(getShardKey(coll, fullNs), sk), 0);

                assert.commandWorked(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{
                        key: Object.assign({}, getShardKey(coll, fullNs), {newSkField: 1}),
                        name: 'sk_1'
                    }],
                    commitQuorum: "majority"
                }));

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(
                    bsonWoCompare(getShardKey(coll, fullNs),
                                  Object.assign({}, getShardKey(coll, fullNs), {newSkField: 1})),
                    0);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);

                assert.commandWorked(coll.getDB().adminCommand(
                    {refineCollectionShardKey: fullNs, key: optionalArgs.sk}));
                assert.eq(bsonWoCompare(getShardKey(coll, fullNs), optionalArgs.sk), 0);
            },
            admin: true,
        },
    },
    refreshLogicalSessionCacheNow: {skip: "does not accept write concern"},
    refreshSessions: {skip: "does not accept write concern"},
    releaseMemory: {skip: "does not accept write concern"},
    removeShard: {skip: "unrelated"},
    removeShardFromZone: {skip: "does not accept write concern"},
    renameCollection: {
        success: {
            // Basic rename
            req: {
                renameCollection: fullNs,
                to: dbName + "." +
                    "coll2"
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.eq(coll.find().itcount(), 1);
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.find().itcount(), 0);
                assert.eq(coll.getDB().coll2.find().itcount(), 1);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
                coll.getDB().coll2.drop();
            },
            admin: true,
        },
        failure: {
            // target collection exists, and dropTarget not set to true
            req: {
                renameCollection: fullNs,
                to: dbName + "." +
                    "coll2"
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(coll.getDB().coll2.insert({a: 1}));
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.getDB().coll2.find().itcount(), 1);
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.NamespaceExists);
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.getDB().coll2.find().itcount(), 1);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            admin: true,
        },
    },
    repairShardedCollectionChunksHistory: {skip: "does not accept write concern"},
    replicateSearchIndexCommand: {skip: "internal command for testing only"},
    replSetAbortPrimaryCatchUp: {skip: "does not accept write concern"},
    replSetFreeze: {skip: "does not accept write concern"},
    replSetGetConfig: {skip: "does not accept write concern"},
    replSetGetRBID: {skip: "does not accept write concern"},
    replSetGetStatus: {skip: "does not accept write concern"},
    replSetHeartbeat: {skip: "does not accept write concern"},
    replSetInitiate: {skip: "does not accept write concern"},
    replSetMaintenance: {skip: "does not accept write concern"},
    replSetReconfig: {skip: "does not accept write concern"},
    replSetRequestVotes: {skip: "does not accept write concern"},
    replSetResizeOplog: {skip: "does not accept write concern"},
    replSetStepDown: {skip: "does not accept write concern"},
    replSetStepUp: {skip: "does not accept write concern"},
    replSetSyncFrom: {skip: "does not accept write concern"},
    replSetTest: {skip: "does not accept write concern"},
    replSetTestEgress: {skip: "does not accept write concern"},
    replSetUpdatePosition: {skip: "does not accept write concern"},
    resetPlacementHistory: {skip: "internal command"},
    reshardCollection: {skip: "does not accept write concern"},
    revokePrivilegesFromRole: {
        targetConfigServer: true,
        noop: {
            // Role does not have privilege
            req: {
                revokePrivilegesFromRole: "foo",
                privileges: [{resource: {db: dbName, collection: collName}, actions: ["insert"]}]
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().runCommand({
                    createRole: "foo",
                    privileges:
                        [{resource: {db: dbName, collection: collName}, actions: ["insert"]}],
                    roles: []
                }));
                assert.commandWorked(coll.getDB().runCommand({
                    revokePrivilegesFromRole: "foo",
                    privileges:
                        [{resource: {db: dbName, collection: collName}, actions: ["insert"]}]
                }));
                let role = coll.getDB().getRoles({rolesInfo: 1, showPrivileges: true});
                assert.eq(role.length, 1);
                assert.eq(role[0].privileges.length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createUser: "fakeusr",
                        pwd: "bar",
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                let role = coll.getDB().getRoles({rolesInfo: 1, showPrivileges: true});
                assert.eq(role.length, 1);
                assert.eq(role[0].privileges.length, 0);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropUser("fakeusr");
                }

                coll.getDB().dropRole("foo");
            },
        },
        success: {
            // Basic revokePrivilegesFromRole
            req: {
                revokePrivilegesFromRole: "foo",
                privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}]
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(coll.getDB().runCommand({
                    grantPrivilegesToRole: "foo",
                    privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}]
                }));
                let role = coll.getDB().getRoles({rolesInfo: 1, showPrivileges: true});
                assert.eq(role.length, 1);
                assert.eq(role[0].privileges.length, 1);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    let role = coll.getDB().getRoles({rolesInfo: 1, showPrivileges: true});
                    assert.eq(role.length, 1);
                    assert.eq(role[0].privileges.length, 0);
                }

                coll.getDB().dropRole("foo");
            },
        },
    },
    revokeRolesFromRole: {
        targetConfigServer: true,
        noop: {
            // Role foo does not have role bar
            req: {revokeRolesFromRole: "foo", roles: [{role: "bar", db: dbName}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(coll.getDB().runCommand({
                    createRole: "bar",
                    privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}],
                    roles: []
                }));
                let role = coll.getDB().getRole("foo");
                assert.eq(role.inheritedRoles.length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createUser: "fakeusr",
                        pwd: "bar",
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                let role = coll.getDB().getRole("foo");
                assert.eq(role.inheritedRoles.length, 0);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropUser("fakeusr");
                }

                coll.getDB().dropRole("foo");
                coll.getDB().dropRole("bar");
            },
        },
        success: {
            // Basic revokeRolesFromRole
            req: {revokeRolesFromRole: "foo", roles: [{role: "bar", db: dbName}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(coll.getDB().runCommand({
                    createRole: "bar",
                    privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}],
                    roles: []
                }));
                assert.commandWorked(coll.getDB().runCommand(
                    {grantRolesToRole: "foo", roles: [{role: "bar", db: dbName}]}));

                let role = coll.getDB().getRole("foo");
                assert.eq(role.inheritedRoles.length, 1);
                assert.eq(role.inheritedRoles[0].role, "bar");

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    let role = coll.getDB().getRole("foo");
                    assert.eq(role.inheritedRoles.length, 0);
                }

                coll.getDB().dropRole("foo");
                coll.getDB().dropRole("bar");
            },
        },
    },
    revokeRolesFromUser: {
        targetConfigServer: true,
        noop: {
            // User does not have role to revoke
            req: {revokeRolesFromUser: "foo", roles: [{role: "foo", db: dbName}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "bar", roles: []}));

                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createRole: "bar",
                        privileges: [],
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 0);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropRole("bar");
                }
                coll.getDB().dropRole("foo");
                coll.getDB().runCommand({dropUser: "foo"});
            },
        },
        success: {
            // Basic revokeRolesFromUser
            req: {revokeRolesFromUser: "foo", roles: [{role: "foo", db: dbName}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "bar", roles: []}));
                assert.commandWorked(coll.getDB().runCommand(
                    {grantRolesToUser: "foo", roles: [{role: "foo", db: dbName}]}));

                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 1);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);

                    let user = coll.getDB().getUser("foo");
                    assert.eq(user.roles.length, 0);
                }

                coll.getDB().dropRole("foo");
                coll.getDB().runCommand({dropUser: "foo"});
            },
        },
    },
    rolesInfo: {skip: "does not accept write concern"},
    rotateCertificates: {skip: "does not accept write concern"},
    rotateFTDC: {skip: "does not accept write concern"},
    saslContinue: {skip: "does not accept write concern"},
    saslStart: {skip: "does not accept write concern"},
    sbe: {skip: "internal command"},
    serverStatus: {skip: "does not accept write concern"},
    setAllowMigrations: {
        targetConfigServer: true,
        noop: {
            // Migrations already not allowed
            req: {setAllowMigrations: fullNs, allowMigrations: false},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().adminCommand(
                    {setAllowMigrations: fullNs, allowMigrations: false}));
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .collections.findOne({_id: fullNs})
                              .permitMigrations,
                          false);

                // TODO SERVER-97754 Do not stop the remaining secondary once setAllowMigrations
                // no longer override user provided writeConcern
                cluster.configRS.stop(secondariesRunning[0]);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .collections.findOne({_id: fullNs})
                              .permitMigrations,
                          false);

                cluster.configRS.restart(secondariesRunning[0]);
            },
            admin: true,
        },
        success: {
            // Basic setAllowMigrations
            req: {setAllowMigrations: fullNs, allowMigrations: false},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().adminCommand({setAllowMigrations: fullNs, allowMigrations: true}));
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .collections.find({_id: fullNs, permitMigrations: {$exists: false}})
                              .itcount(),
                          1);

                // TODO SERVER-97754 Do not stop the remaining secondary once setAllowMigrations
                // no longer override user provided writeConcern
                cluster.configRS.stop(secondariesRunning[0]);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB()
                              .getSiblingDB("config")
                              .collections.find({_id: fullNs, permitMigrations: {$exists: false}})
                              .itcount(),
                          0);

                cluster.configRS.restart(secondariesRunning[0]);
            },
            admin: true,
        },
    },
    setAuditConfig: {skip: "does not accept write concern"},
    setCommittedSnapshot: {skip: "internal command"},
    setDefaultRWConcern: {
        targetConfigServer: true,
        noop: {
            // Default write concern already set to w:1
            req: {setDefaultRWConcern: 1, defaultWriteConcern: {"w": 1, "wtimeout": 0}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().adminCommand(
                    {setDefaultRWConcern: 1, defaultWriteConcern: {"w": 1, "wtimeout": 0}}));
                assert.eq(coll.getDB().adminCommand({getDefaultRWConcern: 1}).defaultWriteConcern,
                          {"w": 1, "wtimeout": 0});

                // TODO SERVER-97754 Do not stop the remaining secondary once setAllowMigrations
                // no longer override user provided writeConcern
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.getDB().adminCommand({getDefaultRWConcern: 1}).defaultWriteConcern,
                          {"w": 1, "wtimeout": 0});

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                }

                // Reset to default of majority
                assert.commandWorked(coll.getDB().adminCommand({
                    setDefaultRWConcern: 1,
                    defaultWriteConcern: {"w": "majority", "wtimeout": 0}
                }));
            },
            admin: true,
        },
        success: {
            // Default RWConcern has wtimeout of 1234
            req: {setDefaultRWConcern: 1, defaultWriteConcern: {"w": 1, "wtimeout": 0}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().adminCommand(
                    {setDefaultRWConcern: 1, defaultWriteConcern: {"w": 1, "wtimeout": 1234}}));
                assert.eq(coll.getDB().adminCommand({getDefaultRWConcern: 1}).defaultWriteConcern,
                          {"w": 1, "wtimeout": 1234});

                // TODO SERVER-97754 Do not stop the remaining secondary once setAllowMigrations
                // no longer override user provided writeConcern
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                }
                // Reset to default of majority
                assert.commandWorked(coll.getDB().adminCommand({
                    setDefaultRWConcern: 1,
                    defaultWriteConcern: {"w": "majority", "wtimeout": 0}
                }));
            },
            admin: true,
        },
        failure: {
            // Default RWConcern is unset
            req: {setDefaultRWConcern: 1, defaultWriteConcern: {}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().adminCommand(
                    {setDefaultRWConcern: 1, defaultWriteConcern: {"w": 1, "wtimeout": 1234}}));

                // TODO SERVER-97754 Do not stop the remaining secondary once setAllowMigrations
                // no longer override user provided writeConcern
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
                assert.eq(coll.getDB().adminCommand({getDefaultRWConcern: 1}).defaultWriteConcern,
                          {"w": 3, "wtimeout": 0});

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                }
                // Reset to default of majority
                assert.commandWorked(coll.getDB().adminCommand({
                    setDefaultRWConcern: 1,
                    defaultWriteConcern: {"w": "majority", "wtimeout": 0}
                }));
            },
            admin: true,
        },
    },
    setFeatureCompatibilityVersion: {
        targetConfigServer: true,
        noop: {
            // FCV already 'latest'
            req: {setFeatureCompatibilityVersion: latestFCV, confirm: true},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                let fcv = coll.getDB()
                              .getSiblingDB("admin")
                              .system.version.findOne({"_id": "featureCompatibilityVersion"})
                              .version;
                optionalArgs.fcv = fcv;

                assert.commandWorked(coll.getDB().adminCommand(
                    {setFeatureCompatibilityVersion: latestFCV, confirm: true}));
                assert.eq(coll.getDB()
                              .getSiblingDB("admin")
                              .system.version.findOne({"_id": "featureCompatibilityVersion"})
                              .version,
                          latestFCV);

                // TODO SERVER-97754 Do not stop the remaining secondary once setAllowMigrations
                // no longer override user provided writeConcern
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB()
                              .getSiblingDB("admin")
                              .system.version.findOne({"_id": "featureCompatibilityVersion"})
                              .version,
                          latestFCV);

                assert.commandWorked(coll.getDB().adminCommand(
                    {setFeatureCompatibilityVersion: optionalArgs.fcv, confirm: true}));
                assert.eq(coll.getDB()
                              .getSiblingDB("admin")
                              .system.version.findOne({"_id": "featureCompatibilityVersion"})
                              .version,
                          optionalArgs.fcv);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                }
            },
            admin: true,
        },
        success: {
            // Change FCV from lastLTS to latest
            req: {setFeatureCompatibilityVersion: latestFCV, confirm: true},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                let fcv = coll.getDB()
                              .getSiblingDB("admin")
                              .system.version.findOne({"_id": "featureCompatibilityVersion"})
                              .version;
                optionalArgs.fcv = fcv;

                assert.commandWorked(coll.getDB().adminCommand(
                    {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
                assert.eq(coll.getDB()
                              .getSiblingDB("admin")
                              .system.version.findOne({"_id": "featureCompatibilityVersion"})
                              .version,
                          lastLTSFCV);

                // TODO SERVER-97754 Do not stop the remaining secondary once setAllowMigrations
                // no longer override user provided writeConcern
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    assert.eq(coll.getDB()
                                  .getSiblingDB("admin")
                                  .system.version.findOne({"_id": "featureCompatibilityVersion"})
                                  .version,
                              lastLTSFCV);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(coll.getDB()
                                  .getSiblingDB("admin")
                                  .system.version.findOne({"_id": "featureCompatibilityVersion"})
                                  .version,
                              latestFCV);
                }

                // Reset FCV
                assert.commandWorked(coll.getDB().adminCommand(
                    {setFeatureCompatibilityVersion: optionalArgs.fcv, confirm: true}));
                assert.eq(coll.getDB()
                              .getSiblingDB("admin")
                              .system.version.findOne({"_id": "featureCompatibilityVersion"})
                              .version,
                          optionalArgs.fcv);
            },
            admin: true,
        },
    },
    setProfilingFilterGlobally: {skip: "does not accept write concern"},
    setIndexCommitQuorum: {
        noop: {
            // commitQuorum already majority
            req: {setIndexCommitQuorum: collName, indexNames: ['b_1'], commitQuorum: "majority"},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1, b: 1}));

                let fpConn = coll.getDB();
                if (clusterType == "sharded") {
                    // Make sure we set the fp on a shard that will be sent the createIndex request
                    let owningShard =
                        cluster.getShard(coll, {_id: 1, b: 1}, false /* includeEmpty */);
                    fpConn = owningShard.getDB(dbName);
                }

                optionalArgs.failpoint =
                    configureFailPoint(fpConn, "hangAfterIndexBuildFirstDrain");
                optionalArgs.thread = new Thread((host, dbName, collName) => {
                    const conn = new Mongo(host);
                    assert.commandWorked(conn.getDB(dbName).runCommand({
                        createIndexes: collName,
                        indexes: [{key: {b: 1}, name: 'b_1'}],
                        commitQuorum: "majority"
                    }));
                }, coll.getDB().getMongo().host, dbName, collName);

                optionalArgs.thread.start();
                optionalArgs.failpoint["wait"]();
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (res.raw) {
                    Object.keys(res.raw).forEach((key) => {
                        assert.commandWorkedIgnoringWriteConcernErrorsOrFailedWithCode(
                            res.raw[key], ErrorCodes.IndexNotFound);
                    });
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }

                optionalArgs.failpoint["off"]();
                optionalArgs.thread.join();
            },
        },
        success: {
            // Basic setIndexCommitQuorum
            req: {setIndexCommitQuorum: collName, indexNames: ['b_1'], commitQuorum: 2},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({_id: 1}));

                let fpConn = coll.getDB();
                if (clusterType == "sharded") {
                    // Make sure we set the fp on a shard that will be sent the createIndex request
                    let owningShard = cluster.getShard(coll, {_id: 1}, /* includeEmpty */);
                    fpConn = owningShard.getDB(dbName);
                }
                optionalArgs.failpoint =
                    configureFailPoint(fpConn, "hangAfterIndexBuildFirstDrain");
                optionalArgs.thread = new Thread((host, dbName, collName) => {
                    // Use the index builds coordinator for a two-phase index build.
                    const conn = new Mongo(host);
                    assert.commandWorked(conn.getDB(dbName).runCommand({
                        createIndexes: collName,
                        indexes: [{key: {b: 1}, name: 'b_1'}],
                        commitQuorum: "majority"
                    }));
                }, coll.getDB().getMongo().host, dbName, collName);
                optionalArgs.thread.start();

                optionalArgs.failpoint["wait"]();
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (res.raw) {
                    Object.keys(res.raw).forEach((key) => {
                        assert.commandWorkedIgnoringWriteConcernErrorsOrFailedWithCode(
                            res.raw[key], ErrorCodes.IndexNotFound);
                    });
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                optionalArgs.failpoint["off"]();
                optionalArgs.thread.join();
            },
        },
    },
    setParameter: {skip: "does not accept write concern"},
    setShardVersion: {skip: "internal command"},
    setChangeStreamState: {skip: "does not accept write concern"},
    setClusterParameter: {skip: "does not accept write concern"},
    setQuerySettings: {skip: "does not accept write concern"},
    removeQuerySettings: {skip: "does not accept write concern"},
    setUserWriteBlockMode: {skip: "does not accept write concern"},
    shardCollection: {
        noop: {
            // Coll already sharded
            req: {shardCollection: fullNs, key: {x: 1}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().adminCommand({shardCollection: fullNs, key: {x: 1}}));
                assert.eq(bsonWoCompare(getShardKey(coll, fullNs), {x: 1}), 0);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(getShardKey(coll, fullNs), {x: 1});
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            admin: true,
        },
        success: {
            // Basic shard coll
            req: {shardCollection: fullNs, key: {x: 1}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                // Ensure DB exists, but create a different collection
                assert.commandWorked(coll.getDB().coll2.insert({_id: 1}));

                assert.eq(bsonWoCompare(getShardKey(coll, fullNs), {}), 0);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(bsonWoCompare(getShardKey(coll, fullNs), {x: 1}), 0);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            admin: true,
        },
    },
    shardingState: {skip: "does not accept write concern"},
    shutdown: {skip: "does not accept write concern"},
    sleep: {skip: "does not accept write concern"},
    split: {skip: "does not accept write concern"},
    splitChunk: {skip: "does not accept write concern"},
    splitVector: {skip: "internal command"},
    stageDebug: {skip: "does not accept write concern"},
    startRecordingTraffic: {skip: "does not accept write concern"},
    startSession: {skip: "does not accept write concern"},
    stopRecordingTraffic: {skip: "does not accept write concern"},
    sysprofile: {skip: "internal command"},
    testCommandFeatureFlaggedOnLatestFCV: {skip: "internal command"},
    testDeprecation: {skip: "test command"},
    testDeprecationInVersion2: {skip: "test command"},
    testInternalTransactions: {skip: "internal command"},
    testRemoval: {skip: "test command"},
    testReshardCloneCollection: {skip: "internal command"},
    testVersions1And2: {skip: "test command"},
    testVersion2: {skip: "test command"},
    timeseriesCatalogBucketParamsChanged: {skip: "internal command"},
    top: {skip: "does not accept write concern"},
    transitionFromDedicatedConfigServer: {skip: "unrelated"},
    transitionToDedicatedConfigServer: {skip: "unrelated"},
    transitionToShardedCluster: {skip: "internal command"},
    unshardCollection: {skip: "does not accept write concern"},
    untrackUnshardedCollection: {skip: "does not accept write concern"},
    update: {
        noop: {
            // The query will not match any doc
            req: {update: collName, updates: [{q: {_id: 1}, u: {b: 2}}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 0, b: 2}));
                assert.commandWorked(coll.remove({_id: 0}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 0);
                assert.eq(res.nModified, 0);
                assert.eq(coll.find().itcount(), 0);
            },
        },
        success: {
            // Basic update
            req: {update: collName, updates: [{q: {_id: 1}, u: {$set: {b: 1}}}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert([{_id: 1}]));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.find({b: 1}).toArray().length, 1);
            },
        },
        failure: {
            // Attempt to update a doc that would fail the validator
            req: {update: collName, updates: [{q: {_id: 1}, u: {_id: 1, c: 2}}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1, x: 1}));
                assert.commandWorked(
                    coll.getDB().runCommand({collMod: collName, validator: {x: {$exists: true}}}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.writeErrors && res.writeErrors.length == 1);
                assert.eq(res.writeErrors[0]["code"], ErrorCodes.DocumentValidationFailure);
                assert.eq(res.n, 0);
                assert.eq(res.nModified, 0);
                assert.eq(coll.count({_id: 1}), 1);
            },
        },
    },
    updateRole: {
        targetConfigServer: true,
        noop: {
            // updateRole to have privileges it already has
            req: {
                updateRole: "foo",
                privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}]
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().runCommand({
                    createRole: "foo",
                    privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}],
                    roles: []
                }));
                assert.eq(coll.getDB().getRoles().length, 1);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createUser: "fakeusr",
                        pwd: "bar",
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                assert.eq(coll.getDB().getRoles().length, 1);
                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropUser("fakeusr");
                }
                coll.getDB().dropRole("foo");
            },
        },
        success: {
            // Basic updateRole to add inherited role
            req: {updateRole: "foo", roles: ["bar"]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "bar", privileges: [], roles: []}));
                assert.eq(coll.getDB().getRoles().length, 2);

                let role = coll.getDB().getRole("foo");
                assert.eq(role.inheritedRoles.length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    let role = coll.getDB().getRole("foo");
                    assert.eq(role.inheritedRoles.length, 1);
                    assert.eq(role.inheritedRoles[0]["role"], ["bar"]);
                }

                coll.getDB().dropRole("foo");
                coll.getDB().dropRole("bar");
            },
        },
        failure: {
            // Creating cycle
            req: {updateRole: "foo", roles: ["foo"]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.eq(coll.getDB().getRoles().length, 1);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createUser: "fakeusr",
                        pwd: "bar",
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.InvalidRoleModification);
                assert.eq(coll.getDB().getRoles().length, 1);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropUser("fakeusr");
                }
                coll.getDB().dropRole("foo");
            },
        },
    },
    updateSearchIndex: {skip: "does not accept write concern"},
    updateUser: {
        targetConfigServer: true,
        noop: {
            // user already has role
            req: {updateUser: "foo", roles: [{role: "foo", db: dbName}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "pwd", roles: ["foo"]}));

                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 1);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createRole: "bar",
                        privileges: [],
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 1);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropRole("bar");
                }

                coll.getDB().dropUser("foo");
                coll.getDB().dropRole("foo");
            },
        },
        success: {
            // Basic updateUser to cadd role
            req: {updateUser: "foo", roles: [{role: "foo", db: dbName}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createRole: "foo", privileges: [], roles: []}));
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "pwd", roles: []}));

                let user = coll.getDB().getUser("foo");
                assert.eq(user.roles.length, 0);

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                    cluster.configRS.restart(secondariesRunning[0]);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);

                    let user = coll.getDB().getUser("foo");
                    assert.eq(user.roles.length, 1);
                }

                coll.getDB().dropUser("foo");
                coll.getDB().dropRole("foo");
            },
        },
        failure: {
            // Role does not exist
            req: {updateUser: "foo", roles: ["fakeRole"]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(
                    coll.getDB().runCommand({createUser: "foo", pwd: "pwd", roles: []}));

                // UMCs enforce wc: majority, so shut down the other node
                if (clusterType == "sharded") {
                    cluster.configRS.stop(secondariesRunning[0]);
                    // Run this to advance the system optime on the config server, so that the
                    // subsequent failing request will also encounter a WriteConcernTimeout.
                    assert.commandFailedWithCode(coll.getDB().runCommand({
                        createRole: "bar",
                        privileges: [],
                        roles: [],
                        writeConcern: {w: "majority", wtimeout: 100}
                    }),
                                                 ErrorCodes.WriteConcernTimeout);
                }
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.RoleNotFound);
                assert.eq(coll.getDB().getUser("foo").roles.length, 0);

                if (clusterType == "sharded") {
                    cluster.configRS.restart(secondariesRunning[0]);
                    coll.getDB().dropRole("bar");
                }
                coll.getDB().dropUser("foo");
            },
        },
    },
    updateZoneKeyRange: {skip: "does not accept write concern"},
    usersInfo: {skip: "does not accept write concern"},
    validate: {skip: "does not accept write concern"},
    validateDBMetadata: {skip: "does not accept write concern"},
    voteAbortIndexBuild: {skip: "internal command"},
    voteCommitImportCollection: {skip: "internal command"},
    voteCommitIndexBuild: {skip: "internal command"},
    waitForFailPoint: {skip: "test command"},
    getShardingReady: {skip: "internal command"},
    whatsmysni: {skip: "does not accept write concern"},
    whatsmyuri: {skip: "internal command"},
};

// All commands applicable on timeseries views in the server.

const wcTimeseriesViewsCommandsTests = {
    _addShard: {skip: "internal command"},
    _cloneCollectionOptionsFromPrimaryShard: {skip: "internal command"},
    _clusterQueryWithoutShardKey: {skip: "internal command"},
    _clusterWriteWithoutShardKey: {skip: "internal command"},
    _configsvrAbortReshardCollection: {skip: "internal command"},
    _configsvrAddShard: {skip: "internal command"},
    _configsvrAddShardCoordinator: {skip: "internal command"},
    _configsvrAddShardToZone: {skip: "internal command"},
    _configsvrBalancerCollectionStatus: {skip: "internal command"},
    _configsvrBalancerStart: {skip: "internal command"},
    _configsvrBalancerStatus: {skip: "internal command"},
    _configsvrBalancerStop: {skip: "internal command"},
    _configsvrCheckClusterMetadataConsistency: {skip: "internal command"},
    _configsvrCheckMetadataConsistency: {skip: "internal command"},
    _configsvrCleanupReshardCollection: {skip: "internal command"},
    _configsvrCollMod: {skip: "internal command"},
    _configsvrClearJumboFlag: {skip: "internal command"},
    _configsvrCommitChunksMerge: {skip: "internal command"},
    _configsvrCommitChunkMigration: {skip: "internal command"},
    _configsvrCommitChunkSplit: {skip: "internal command"},
    _configsvrCommitIndex: {skip: "internal command"},
    _configsvrCommitMergeAllChunksOnShard: {skip: "internal command"},
    _configsvrCommitMovePrimary: {skip: "internal command"},
    _configsvrCommitRefineCollectionShardKey: {skip: "internal command"},
    _configsvrCommitReshardCollection: {skip: "internal command"},
    _configsvrConfigureCollectionBalancing: {skip: "internal command"},
    _configsvrCreateDatabase: {skip: "internal command"},
    _configsvrDropIndexCatalogEntry: {skip: "internal command"},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: "internal command"},
    _configsvrGetHistoricalPlacement: {skip: "internal command"},
    _configsvrMoveRange: {skip: "internal command"},
    _configsvrRemoveChunks: {skip: "internal command"},
    _configsvrRemoveShard: {skip: "internal command"},
    _configsvrRemoveShardCommit: {skip: "internal command"},
    _configsvrRemoveShardFromZone: {skip: "internal command"},
    _configsvrRemoveTags: {skip: "internal command"},
    _configsvrRenameCollection: {skip: "internal command"},
    _configsvrRepairShardedCollectionChunksHistory: {skip: "internal command"},
    _configsvrResetPlacementHistory: {skip: "internal command"},
    _configsvrReshardCollection: {skip: "internal command"},
    _configsvrRunRestore: {skip: "internal command"},
    _configsvrSetAllowMigrations: {skip: "internal command"},
    _configsvrSetClusterParameter: {skip: "internal command"},
    _configsvrSetUserWriteBlockMode: {skip: "internal command"},
    _configsvrTransitionFromDedicatedConfigServer: {skip: "internal command"},
    _configsvrTransitionToDedicatedConfigServer: {skip: "internal command"},
    _configsvrUpdateZoneKeyRange: {skip: "internal command"},
    _dropConnectionsToMongot: {skip: "internal command"},
    _flushDatabaseCacheUpdates: {skip: "internal command"},
    _flushDatabaseCacheUpdatesWithWriteConcern: {skip: "internal command"},
    _flushReshardingStateChange: {skip: "internal command"},
    _flushRoutingTableCacheUpdates: {skip: "internal command"},
    _flushRoutingTableCacheUpdatesWithWriteConcern: {skip: "internal command"},
    _getNextSessionMods: {skip: "internal command"},
    _getUserCacheGeneration: {skip: "internal command"},
    _hashBSONElement: {skip: "internal command"},
    _isSelf: {skip: "internal command"},
    _killOperations: {skip: "internal command"},
    _mergeAuthzCollections: {skip: "internal command"},
    _migrateClone: {skip: "internal command"},
    _mongotConnPoolStats: {skip: "internal command"},
    _recvChunkAbort: {skip: "internal command"},
    _recvChunkCommit: {skip: "internal command"},
    _recvChunkReleaseCritSec: {skip: "internal command"},
    _recvChunkStart: {skip: "internal command"},
    _recvChunkStatus: {skip: "internal command"},
    _refreshQueryAnalyzerConfiguration: {skip: "internal command"},
    _shardsvrAbortReshardCollection: {skip: "internal command"},
    _shardsvrBeginMigrationBlockingOperation: {skip: "internal command"},
    _shardsvrChangePrimary: {skip: "internal command"},
    _shardsvrCleanupReshardCollection: {skip: "internal command"},
    _shardsvrCloneCatalogData: {skip: "internal command"},
    _shardsvrCommitToShardLocalCatalog: {skip: "internal command"},
    _shardsvrRegisterIndex: {skip: "internal command"},
    _shardsvrCheckMetadataConsistency: {skip: "internal command"},
    _shardsvrCheckMetadataConsistencyParticipant: {skip: "internal command"},
    _shardsvrCleanupStructuredEncryptionData: {skip: "internal command"},
    _shardsvrCloneAuthoritativeMetadata: {skip: "internal command"},
    _shardsvrCommitCreateDatabaseMetadata: {skip: "internal command"},
    _shardsvrCommitDropDatabaseMetadata: {skip: "internal command"},
    _shardsvrCommitIndexParticipant: {skip: "internal command"},
    _shardsvrCommitReshardCollection: {skip: "internal command"},
    _shardsvrCompactStructuredEncryptionData: {skip: "internal command"},
    _shardsvrConvertToCapped: {skip: "internal command"},
    _shardsvrCoordinateMultiUpdate: {skip: "internal command"},
    _shardsvrCreateCollection: {skip: "internal command"},
    _shardsvrCreateCollectionParticipant: {skip: "internal command"},
    _shardsvrDropCollection: {skip: "internal command"},
    _shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern: {skip: "internal command"},
    _shardsvrDropCollectionParticipant: {skip: "internal command"},
    _shardsvrUnregisterIndex: {skip: "internal command"},
    _shardsvrDropIndexCatalogEntryParticipant: {skip: "internal command"},
    _shardsvrDropIndexes: {skip: "internal command"},
    _shardsvrDropDatabase: {skip: "internal command"},
    _shardsvrDropDatabaseParticipant: {skip: "internal command"},
    _shardsvrEndMigrationBlockingOperation: {skip: "internal command"},
    _shardsvrFetchCollMetadata: {skip: "internal command"},
    _shardsvrGetStatsForBalancing: {skip: "internal command"},
    _shardsvrJoinDDLCoordinators: {skip: "internal command"},
    _shardsvrJoinMigrations: {skip: "internal command"},
    _shardsvrMergeAllChunksOnShard: {skip: "internal command"},
    _shardsvrMovePrimary: {skip: "internal command"},
    _shardsvrMovePrimaryEnterCriticalSection: {skip: "internal command"},
    _shardsvrMovePrimaryExitCriticalSection: {skip: "internal command"},
    _shardsvrMoveRange: {skip: "internal command"},
    _shardsvrNotifyShardingEvent: {skip: "internal command"},
    _shardsvrRefineCollectionShardKey: {skip: "internal command"},
    _shardsvrRenameCollection: {skip: "internal command"},
    _shardsvrRenameCollectionParticipant: {skip: "internal command"},
    _shardsvrRenameCollectionParticipantUnblock: {skip: "internal command"},
    _shardsvrRenameIndexMetadata: {skip: "internal command"},
    _shardsvrReshardCollection: {skip: "internal command"},
    _shardsvrReshardingDonorFetchFinalCollectionStats: {skip: "internal command"},
    _shardsvrReshardingDonorStartChangeStreamsMonitor: {skip: "internal command"},
    _shardsvrReshardingOperationTime: {skip: "internal command"},
    _shardsvrReshardRecipientClone: {skip: "internal command"},
    _shardsvrResolveView: {skip: "internal command"},
    _shardsvrRunSearchIndexCommand: {skip: "internal command"},
    _shardsvrSetAllowMigrations: {skip: "internal command"},
    _shardsvrSetClusterParameter: {skip: "internal command"},
    _shardsvrSetUserWriteBlockMode: {skip: "internal command"},
    _shardsvrValidateShardKeyCandidate: {skip: "internal command"},
    _shardsvrCollMod: {skip: "internal command"},
    _shardsvrCollModParticipant: {skip: "internal command"},
    _shardsvrConvertToCappedParticipant: {skip: "internal command"},
    _shardsvrParticipantBlock: {skip: "internal command"},
    _shardsvrUntrackUnsplittableCollection: {skip: "internal command"},
    streams_startStreamProcessor: {skip: "internal command"},
    streams_startStreamSample: {skip: "internal command"},
    streams_stopStreamProcessor: {skip: "internal command"},
    streams_listStreamProcessors: {skip: "internal command"},
    streams_getMoreStreamSample: {skip: "internal command"},
    streams_getStats: {skip: "internal command"},
    streams_testOnlyInsert: {skip: "internal command"},
    streams_getMetrics: {skip: "internal command"},
    streams_updateFeatureFlags: {skip: "internal command"},
    streams_testOnlyGetFeatureFlags: {skip: "internal command"},
    streams_writeCheckpoint: {skip: "internal command"},
    streams_sendEvent: {skip: "internal command"},
    streams_updateConnection: {skip: "internal command"},
    _transferMods: {skip: "internal command"},
    abortMoveCollection: {skip: "does not accept write concern"},
    abortReshardCollection: {skip: "does not accept write concern"},
    abortTransaction: {skip: "not supported on timeseries views"},
    abortUnshardCollection: {skip: "does not accept write concern"},
    addShard: {skip: "unrelated"},
    addShardToZone: {skip: "does not accept write concern"},
    aggregate: {
        noop: {
            // The pipeline will not match any docs, so nothing should be written to "out"
            req: {
                aggregate: collName,
                pipeline: [{$match: {"meta.x": 1}}, {$out: "out"}],
                cursor: {}
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1, y: 1}, time: timeValue}));
                assert.commandWorked(coll.deleteMany({meta: {x: 1, y: 1}}));
                assert.eq(coll.count({meta: {x: 1, y: 1}}), 0);
                assert.commandWorked(coll.getDB().createCollection("out"));
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                }
                assert.eq(coll.getDB().out.find().itcount(), 0);
                coll.getDB().out.drop();
            },
        },
        success: {
            // The pipeline should write a single doc to "out"
            req: {
                aggregate: collName,
                pipeline: [{$match: {"meta.x": 1}}, {$out: "out"}],
                cursor: {}
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1, y: 1}, time: timeValue}));
                assert.commandWorked(coll.getDB().createCollection("out"));
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                    assert.eq(coll.getDB().out.find().itcount(), 0);
                } else {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(coll.getDB().out.find().itcount(), 1);
                }
                assert.eq(coll.count({meta: {x: 1, y: 1}}), 1);
                coll.getDB().out.drop();
            },
        },
        failure: {
            // Attempt to update non meta field
            req: {
                aggregate: collName,
                pipeline: [{$match: {"meta.x": 1}}, {$set: {time: "deadbeef"}}, {$out: "out"}],
                cursor: {},
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.commandWorked(coll.insert({meta: 2, time: timeValue}));
                assert.commandWorked(coll.getDB().createCollection(
                    "out", {timeseries: {timeField: "time", metaField: "meta"}}));
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.getDB().out.find().itcount(), 0);
                coll.getDB().out.drop();
            },
        },
    },
    // TODO SPM-2513 Define test case for analyze once the command is user facing
    analyze: {skip: "internal only"},
    analyzeShardKey: {skip: "does not accept write concern"},
    appendOplogNote: {
        success: {
            // appendOplogNote basic
            req: {appendOplogNote: 1, data: {meta: 1}},
            setupFunc: (coll) => {},
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
            },
            admin: true,
        },
    },
    applyOps: {
        noop: {
            // 'applyOps' where the update is a no-op
            req: {applyOps: [{op: "u", ns: fullNs, o: {meta: 1, _id: 0}, o2: {meta: 1}}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                if (clusterType == "sharded") {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(res.results[0], true);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupportedOnView);
                }
                assert.eq(res.applied, 1);
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.count({meta: 1}), 1);
            },
        },
        success: {
            // 'applyOps' basic insert
            req: {applyOps: [{op: "i", ns: fullNs, o: {meta: 2, time: timeValue}}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                if (clusterType == "sharded") {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(res.results[0], true);
                    assert.eq(coll.find().itcount(), 2);
                    assert.eq(coll.count({time: timeValue}), 2);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupportedOnView);
                    assert.eq(coll.find().itcount(), 1);
                    assert.eq(coll.count({time: timeValue}), 1);
                }
                assert.eq(res.applied, 1);
            },
        },
        failure: {
            // 'applyOps' attempt to update to bad value
            req: {
                applyOps:
                    [{op: "u", ns: fullNs, o: {time: timeValue, _id: 0}, o2: {time: "deadbeef"}}]
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue, _id: 0}));
            },
            confirmFunc: (res, coll) => {
                assert.eq(res.applied, 1);
                assert.eq(res.ok, 0);
                assert.eq(res.results[0], false);
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.count({meta: 1}), 1);
            },
        },
    },
    authenticate: {skip: "does not accept write concern"},
    autoCompact: {skip: "does not accept write concern"},
    autoSplitVector: {skip: "does not accept write concern"},
    balancerCollectionStatus: {skip: "does not accept write concern"},
    balancerStart: {skip: "does not accept write concern"},
    balancerStatus: {skip: "does not accept write concern"},
    balancerStop: {skip: "does not accept write concern"},
    buildInfo: {skip: "does not accept write concern"},
    bulkWrite: {
        noop: {
            // The doc to update doesn't exist
            req: {
                bulkWrite: 1,
                ops: [{
                    update: 0,
                    multi: true,
                    filter: {"meta.x": 0},
                    updateMods: {$set: {"meta.y": 1}}
                }],
                nsInfo: [{ns: fullNs}]
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1, y: 1}, time: timeValue}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.nErrors, 0);
                assert.eq(res.nModified, 0);
            },
            admin: true,
        },
        success: {
            // Basic insert in a bulk write
            req: {
                bulkWrite: 1,
                ops: [{insert: 0, document: {meta: 2, time: timeValue}}],
                nsInfo: [{ns: fullNs}]
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 1, time: ISODate()}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.nErrors, 0);
                assert.eq(res.nInserted, 1);
            },
            admin: true,
        },
        failure: {
            // Attempt to update doc with bad time value
            req: {
                bulkWrite: 1,
                ops: [{
                    update: 0,
                    filter: {"meta.x": 1},
                    multi: true,
                    updateMods: {$set: {time: "deadbeef"}}
                }],
                nsInfo: [{ns: fullNs}]
            },
            setupFunc: (coll, cluster) => {
                assert.commandWorked(coll.insert({meta: {x: 1, y: 1}, time: ISODate()}));
            },
            confirmFunc: (res, coll, cluster) => {
                assert.eq(res.nErrors, 1);
                assert(res.cursor && res.cursor.firstBatch && res.cursor.firstBatch.length == 1);
                assert.includes([ErrorCodes.BadValue, ErrorCodes.InvalidOptions],
                                res.cursor.firstBatch[0].code);
            },
            admin: true,
        },
    },
    changePrimary: {
        noop: {
            // The destination shard is already the primary shard for the db
            req: (cluster) => ({changePrimary: dbName, to: getShardNames(cluster)[0]}),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.commandWorked(coll.getDB().adminCommand(
                    {changePrimary: dbName, to: getShardNames(cluster)[0]}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            admin: true,
        },
        success: {
            // Basic change primary
            req: (cluster) => ({changePrimary: dbName, to: getShardNames(cluster)[1]}),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.commandWorked(coll.getDB().adminCommand(
                    {changePrimary: dbName, to: getShardNames(cluster)[0]}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard1.shardName);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);

                // Change the primary back
                assert.commandWorked(coll.getDB().adminCommand(
                    {changePrimary: dbName, to: cluster.shard0.shardName}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);
            },
            admin: true,
        },
    },
    checkMetadataConsistency: {skip: "does not accept write concern"},
    checkShardingIndex: {skip: "does not accept write concern"},
    cleanupOrphaned: {skip: "only exist on direct shard connection"},
    cleanupReshardCollection: {skip: "does not accept write concern"},
    cleanupStructuredEncryptionData: {skip: "does not accept write concern"},
    clearJumboFlag: {skip: "does not accept write concern"},
    clearLog: {skip: "does not accept write concern"},
    cloneCollectionAsCapped: {skip: "not supported on timeseries views"},
    clusterAbortTransaction: {skip: "already tested by 'abortTransaction' tests on mongos"},
    clusterAggregate: {skip: "already tested by 'aggregate' tests on mongos"},
    clusterBulkWrite: {skip: "already tested by 'bulkWrite' tests on mongos"},
    clusterCommitTransaction: {skip: "already tested by 'commitTransaction' tests on mongos"},
    clusterCount: {skip: "already tested by 'count' tests on mongos"},
    clusterDelete: {skip: "already tested by 'delete' tests on mongos"},
    clusterFind: {skip: "already tested by 'find' tests on mongos"},
    clusterGetMore: {skip: "already tested by 'getMore' tests on mongos"},
    clusterInsert: {skip: "already tested by 'insert' tests on mongos"},
    clusterUpdate: {skip: "already tested by 'update' tests on mongos"},
    collMod: {
        noop: {
            // Set expireAfterSeconds off for already non expiring
            req: {collMod: collName, expireAfterSeconds: "off"},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        success: {
            // Add validator
            req: {collMod: collName, index: {keyPattern: {a: 1}, hidden: true}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.commandWorked(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{key: {a: 1}, name: "a_1"}],
                    commitQuorum: "majority"
                }));
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getIndexes().length, 2);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        failure: {
            // Index to be updated does not exist
            req: {collMod: collName, index: {name: "a_1", hidden: true}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{key: {a: 1}, name: "a_1"}],
                    commitQuorum: "majority"
                }));
                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand(
                    {dropIndexes: collName, index: "a_1"},
                    ));
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.IndexNotFound);
                assert.eq(coll.getDB().getCollectionInfos({name: collName})[0].options.validator,
                          undefined);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    collStats: {skip: "does not accept write concern"},
    commitReshardCollection: {skip: "does not accept write concern"},
    commitTransaction: {skip: "not supported on timeseries views"},
    compact: {skip: "does not accept write concern"},
    compactStructuredEncryptionData: {skip: "does not accept write concern"},
    configureCollectionBalancing: {skip: "does not accept write concern"},
    configureFailPoint: {skip: "internal command"},
    configureQueryAnalyzer: {skip: "does not accept write concern"},
    connPoolStats: {skip: "does not accept write concern"},
    connPoolSync: {skip: "internal command"},
    connectionStatus: {skip: "does not accept write concern"},
    convertToCapped: {skip: "not supported on timeseries views"},
    coordinateCommitTransaction: {skip: "internal command"},
    count: {skip: "does not accept write concern"},
    cpuload: {skip: "does not accept write concern"},
    create: {
        noop: {
            // Coll already exists
            req: {create: collName, timeseries: {timeField: "time", metaField: "meta"}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                coll.insert({meta: 1, time: ISODate()});
                assert.eq(coll.getDB().getCollectionInfos({name: collName}).length, 1);
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB().getCollectionInfos({name: collName}).length, 1);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        success: {
            // Basic create coll
            req: {create: collName, timeseries: {timeField: "time", metaField: "meta"}},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert(coll.drop());
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.getDB().getCollectionInfos({name: collName}).length, 1);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        failure: {
            // Attempt to create a view and output to a nonexistent collection
            req: {create: "viewWithOut", viewOn: collName, pipeline: [{$out: "nonexistentColl"}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.eq(coll.find().itcount(), 1);
                assert.commandWorked(coll.getDB().runCommand({drop: "nonexistentColl"}));
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                if (clusterType == "sharded") {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.OptionNotSupportedOnView);
                }
                assert.eq(coll.find().itcount(), 1);
                assert(!coll.getDB().getCollectionNames().includes("nonexistentColl"));

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    createIndexes: {
        noop: {
            // Index already exists
            req: {
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
                commitQuorum: "majority"
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{key: {a: 1}, name: "a_1"}],
                    commitQuorum: "majority"
                }));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                let details = res;
                if ("raw" in details) {
                    const raw = details.raw;
                    details = raw[Object.keys(raw)[0]];
                }
                assert.eq(details.numIndexesBefore, details.numIndexesAfter);
                assert.eq(details.note, 'all indexes already exist');
            },
        },
        success: {
            // Basic create index
            req: {
                createIndexes: collName,
                indexes: [{key: {b: 1}, name: "b_1"}],
                commitQuorum: "majority"
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                let details = res;
                if ("raw" in details) {
                    const raw = details.raw;
                    details = raw[Object.keys(raw)[0]];
                }
                assert.eq(details.numIndexesBefore, details.numIndexesAfter - 1);
            },
        },
        failure: {
            // Attempt to create two indexes with the same name and different keys
            req: {
                createIndexes: collName,
                indexes: [{key: {b: 1}, name: "b_1"}],
                commitQuorum: "majority"
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{key: {b: 1}, name: "b_1"}],
                    commitQuorum: "majority"
                }));
            },
            confirmFunc: (res, coll) => {
                assert.commandFailedWithCode(res, ErrorCodes.IndexKeySpecsConflict);
            },
        },
    },
    createRole: wcCommandsTests["createRole"],
    createSearchIndexes: {skip: "does not accept write concern"},
    createUnsplittableCollection: {skip: "internal command"},
    createUser: wcCommandsTests["createUser"],
    currentOp: {skip: "does not accept write concern"},
    dataSize: {skip: "does not accept write concern"},
    dbCheck: {skip: "does not accept write concern"},
    dbHash: {skip: "does not accept write concern"},
    dbStats: {skip: "does not accept write concern"},
    delete: {
        noop: {
            // The query will not match any doc
            req: {delete: collName, deletes: [{q: {"meta.x": 1}, limit: 0}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1, y: 1}, time: timeValue}));
                assert.commandWorked(coll.remove({meta: {x: 1, y: 1}, time: timeValue}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 0);
                assert.eq(coll.count({meta: {x: 1, y: 1}}), 0);
            },
        },
        success: {
            req: {delete: collName, deletes: [{q: {"meta.x": 1}, limit: 0}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1, y: 1}, time: timeValue}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 1);
                assert.eq(coll.find().itcount(), 0);
            },
        },
    },
    distinct: {skip: "does not accept write concern"},
    drop: {
        noop: {
            // The collection has already been dropped
            req: {drop: collName},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.commandWorked(coll.getDB().runCommand({drop: collName}));
                assert.eq(coll.find().itcount(), 0);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        success: {
            // Basic drop collection
            req: {drop: collName},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.find().itcount(), 0);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    dropAllRolesFromDatabase: wcCommandsTests["dropAllRolesFromDatabase"],
    dropAllUsersFromDatabase: wcCommandsTests["dropAllUsersFromDatabase"],
    dropConnections: {skip: "does not accept write concern"},
    dropDatabase: {
        noop: {
            // Database has already been dropped
            req: {dropDatabase: 1},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.commandWorkedIgnoringWriteConcernErrors(
                    coll.getDB().runCommand({dropDatabase: 1}));
                assert.eq(coll.find().itcount(), 0);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
            },
        },
        success: {
            // Basic drop database
            req: {dropDatabase: 1},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    dropIndexes: {
        noop: {
            // Passing "*" will drop all indexes except the index created for shard key in pre
            // setup.
            // The only index on this collection is that one so the command will be a no-op in
            // sharded cluster. In replica sets, there is a default index created on the timeseries
            // fields - this will be dropped in setup.
            req: {
                dropIndexes: collName,
                index: "*",
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: "a", time: timeValue}));
                assert.commandWorked(coll.getDB().runCommand({dropIndexes: collName, index: "*"}));
                if (clusterType == "sharded") {
                    assert.eq(coll.getIndexes().length, 1);
                } else {
                    assert.eq(coll.getIndexes().length, 0);
                }
                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                let details = res;
                if ("raw" in details) {
                    const raw = details.raw;
                    details = raw[Object.keys(raw)[0]];
                }
                if (clusterType == "sharded") {
                    assert.eq(coll.getIndexes().length, 1);
                } else {
                    assert.eq(coll.getIndexes().length, 0);
                }
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
        success: {
            // Basic drop index
            req: {
                dropIndexes: collName,
                index: "b_1",
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: "b", time: timeValue}));

                const numIndexesBefore = coll.getIndexes().length;
                optionalArgs.numIndexesBefore = numIndexesBefore;

                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{key: {b: 1}, name: "b_1"}],
                    commitQuorum: "majority"
                }));

                assert.eq(coll.getIndexes().length, numIndexesBefore + 1);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                let details = res;
                if ("raw" in details) {
                    const raw = details.raw;
                    details = raw[Object.keys(raw)[0]];
                }
                assert.eq(coll.getIndexes().length, optionalArgs.numIndexesBefore);
                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
        },
    },
    dropRole: wcCommandsTests["dropRole"],
    dropSearchIndex: {skip: "does not accept write concern"},
    dropUser: wcCommandsTests["dropUser"],
    echo: {skip: "does not accept write concern"},
    enableSharding: {
        targetConfigServer: true,
        noop: {
            // Sharding already enabled
            req: {enableSharding: dbName},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().runCommand({dropDatabase: 1}));
                assert.commandWorked(coll.getDB().adminCommand({enableSharding: dbName}));
                assert.eq(
                    coll.getDB().getSiblingDB("config").databases.find({_id: dbName}).itcount(), 1);

                // TODO SERVER-97754 Do not stop the remaining secondary once enableSharding
                // no longer override user provided writeConcern
                cluster.configRS.stop(secondariesRunning[0]);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(
                    coll.getDB().getSiblingDB("config").databases.find({_id: dbName}).itcount(), 1);

                cluster.configRS.restart(secondariesRunning[0]);
            },
            admin: true,
        },
        success: {
            // Basic enable sharding
            req: {enableSharding: dbName},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.getDB().runCommand({dropDatabase: 1}));
                assert.eq(
                    coll.getDB().getSiblingDB("config").databases.find({_id: dbName}).itcount(), 0);

                // TODO SERVER-97754 Do not stop the remaining secondary once enableSharding
                // no longer override user provided writeConcern
                cluster.configRS.stop(secondariesRunning[0]);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);

                cluster.configRS.restart(secondariesRunning[0]);
            },
            admin: true,
        },
    },
    endSessions: {skip: "does not accept write concern"},
    explain: {skip: "does not accept write concern"},
    features: {skip: "does not accept write concern"},
    filemd5: {skip: "does not accept write concern"},
    find: {skip: "does not accept write concern"},
    findAndModify: {
        // noop and success cases are not applicable because findAndModify does single
        // updates and non-multi updates are not allowed on timeseries views
        failure: {
            req: {findAndModify: collName, query: {"meta.x": 1}, update: {time: "deadbeef"}},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1, y: 1}, time: timeValue}));
            },
            confirmFunc: (res, coll, cluster) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.includes([ErrorCodes.InvalidOptions, ErrorCodes.BadValue], res.code);
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.count({meta: {x: 1, y: 1}}), 1);
            },
        }
    },
    flushRouterConfig: {skip: "does not accept write concern"},
    forceerror: {skip: "test command"},
    fsync: {skip: "does not accept write concern"},
    fsyncUnlock: {skip: "does not accept write concern"},
    getAuditConfig: {skip: "does not accept write concern"},
    getChangeStreamState: {skip: "does not accept write concern"},
    getClusterParameter: {skip: "does not accept write concern"},
    getCmdLineOpts: {skip: "does not accept write concern"},
    getDatabaseVersion: {skip: "internal command"},
    getDefaultRWConcern: {skip: "does not accept write concern"},
    getDiagnosticData: {skip: "does not accept write concern"},
    getLog: {skip: "does not accept write concern"},
    getMore: {skip: "does not accept write concern"},
    getParameter: {skip: "does not accept write concern"},
    getQueryableEncryptionCountInfo: {skip: "does not accept write concern"},
    getShardMap: {skip: "internal command"},
    getShardVersion: {skip: "internal command"},
    godinsert: {skip: "for testing only"},
    grantPrivilegesToRole: wcCommandsTests["grantPrivilegesToRole"],
    grantRolesToRole: wcCommandsTests["grantRolesToRole"],
    grantRolesToUser: wcCommandsTests["grantRolesToUser"],
    handshake: {skip: "does not accept write concern"},
    hello: {skip: "does not accept write concern"},
    hostInfo: {skip: "does not accept write concern"},
    httpClientRequest: {skip: "does not accept write concern"},
    exportCollection: {skip: "internal command"},
    importCollection: {skip: "internal command"},
    insert: {
        // A no-op insert that returns success is not possible
        success: {
            // Basic insert
            req: {insert: collName, documents: [{meta: 11, time: timeValue}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 11, time: timeValue}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 1);
                assert.eq(coll.find().itcount(), 2);
            },
        },
    },
    internalRenameIfOptionsAndIndexesMatch: {skip: "internal command"},
    invalidateUserCache: {skip: "does not accept write concern"},
    isdbgrid: {skip: "does not accept write concern"},
    isMaster: {skip: "does not accept write concern"},
    killAllSessions: {skip: "does not accept write concern"},
    killAllSessionsByPattern: {skip: "does not accept write concern"},
    killCursors: {skip: "does not accept write concern"},
    killOp: {skip: "does not accept write concern"},
    killSessions: {skip: "does not accept write concern"},
    listCollections: {skip: "does not accept write concern"},
    listCommands: {skip: "does not accept write concern"},
    listDatabases: {skip: "does not accept write concern"},
    listDatabasesForAllTenants: {skip: "does not accept write concern"},
    listIndexes: {skip: "does not accept write concern"},
    listSearchIndexes: {skip: "does not accept write concern"},
    listShards: {skip: "does not accept write concern"},
    lockInfo: {skip: "does not accept write concern"},
    logApplicationMessage: {skip: "does not accept write concern"},
    logMessage: {skip: "does not accept write concern"},
    logRotate: {skip: "does not accept write concern"},
    logout: {skip: "does not accept write concern"},
    makeSnapshot: {skip: "does not accept write concern"},
    mapReduce: {skip: "deprecated"},
    mergeAllChunksOnShard: {skip: "does not accept write concern"},
    mergeChunks: {skip: "does not accept write concern"},
    moveChunk: {skip: "not applicable on timeseries views"},
    moveCollection: {skip: "does not accept write concern"},
    movePrimary: {
        noop: {
            // The destination shard is already the primary shard for the db
            req: (cluster) => ({movePrimary: dbName, to: getShardNames(cluster)[0]}),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.commandWorked(coll.getDB().adminCommand(
                    {movePrimary: dbName, to: getShardNames(cluster)[0]}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            admin: true,
        },
        success: {
            // Basic movePrimary
            req: (cluster) => ({movePrimary: dbName, to: getShardNames(cluster)[1]}),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.commandWorked(coll.getDB().adminCommand(
                    {movePrimary: dbName, to: getShardNames(cluster)[0]}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);

                // Change the primary back
                assert.commandWorked(
                    coll.getDB().adminCommand({movePrimary: dbName, to: cluster.shard0.shardName}));
                assert.eq(coll.getDB().getDatabasePrimaryShardId(), cluster.shard0.shardName);
            },
            admin: true,
        },
    },
    moveRange: {skip: "not applicable on timeseries views"},
    multicast: {skip: "does not accept write concern"},
    netstat: {skip: "internal command"},
    oidcListKeys: {skip: "does not accept write concern"},
    oidcRefreshKeys: {skip: "does not accept write concern"},
    pinHistoryReplicated: {skip: "internal command"},
    ping: {skip: "does not accept write concern"},
    planCacheClear: {skip: "does not accept write concern"},
    planCacheClearFilters: {skip: "does not accept write concern"},
    planCacheListFilters: {skip: "does not accept write concern"},
    planCacheSetFilter: {skip: "does not accept write concern"},
    prepareTransaction: {skip: "internal command"},
    profile: {skip: "does not accept write concern"},
    reIndex: {skip: "does not accept write concern"},
    reapLogicalSessionCacheNow: {skip: "does not accept write concern"},
    refineCollectionShardKey: {

        noop: {
            // Refine to same shard key
            req: (cluster, coll) =>
                ({refineCollectionShardKey: fullNs, key: getShardKey(coll, fullNs)}),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            admin: true,
        },
        success: {
            // Add additional field to shard key
            req: (cluster, coll) => ({
                refineCollectionShardKey: fullNs,
                key: Object.assign({}, getShardKey(coll, fullNs), {"meta.a": 1})
            }),
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                let sk = getShardKey(coll, fullNs);
                optionalArgs.origSk = sk;
                assert.eq(bsonWoCompare(getShardKey(coll, fullNs), sk), 0);

                assert.commandWorked(coll.getDB().runCommand({
                    createIndexes: collName,
                    indexes: [{
                        key: Object.assign({}, getShardKey(coll, fullNs), {"meta.a": 1}),
                        name: 'sk_1'
                    }],
                    commitQuorum: "majority"
                }));

                stopAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);
            },
            confirmFunc: (res, coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(
                    bsonWoCompare(getShardKey(coll, fullNs),
                                  Object.assign({}, getShardKey(coll, fullNs), {"meta.a": 1})),
                    0);

                restartAdditionalSecondariesIfSharded(clusterType, cluster, secondariesRunning);

                assert.commandWorked(coll.getDB().adminCommand(
                    {refineCollectionShardKey: fullNs, key: optionalArgs.sk}));
                assert.eq(bsonWoCompare(getShardKey(coll, fullNs), optionalArgs.sk), 0);
            },
            admin: true,
        },
    },
    refreshLogicalSessionCacheNow: {skip: "does not accept write concern"},
    refreshSessions: {skip: "does not accept write concern"},
    releaseMemory: {skip: "does not accept write concern"},
    removeShard: {skip: "unrelated"},
    removeShardFromZone: {skip: "does not accept write concern"},
    renameCollection: {skip: "not supported on timeseries views"},
    repairShardedCollectionChunksHistory: {skip: "does not accept write concern"},
    replicateSearchIndexCommand: {skip: "internal command for testing only"},
    replSetAbortPrimaryCatchUp: {skip: "does not accept write concern"},
    replSetFreeze: {skip: "does not accept write concern"},
    replSetGetConfig: {skip: "does not accept write concern"},
    replSetGetRBID: {skip: "does not accept write concern"},
    replSetGetStatus: {skip: "does not accept write concern"},
    replSetHeartbeat: {skip: "does not accept write concern"},
    replSetInitiate: {skip: "does not accept write concern"},
    replSetMaintenance: {skip: "does not accept write concern"},
    replSetReconfig: {skip: "does not accept write concern"},
    replSetRequestVotes: {skip: "does not accept write concern"},
    replSetResizeOplog: {skip: "does not accept write concern"},
    replSetStepDown: {skip: "does not accept write concern"},
    replSetStepUp: {skip: "does not accept write concern"},
    replSetSyncFrom: {skip: "does not accept write concern"},
    replSetTest: {skip: "does not accept write concern"},
    replSetTestEgress: {skip: "does not accept write concern"},
    replSetUpdatePosition: {skip: "does not accept write concern"},
    resetPlacementHistory: {skip: "internal command"},
    reshardCollection: {skip: "does not accept write concern"},
    revokePrivilegesFromRole: wcCommandsTests["revokePrivilegesFromRole"],
    revokeRolesFromRole: wcCommandsTests["revokeRolesFromRole"],
    revokeRolesFromUser: wcCommandsTests["revokeRolesFromUser"],
    rolesInfo: {skip: "does not accept write concern"},
    rotateCertificates: {skip: "does not accept write concern"},
    rotateFTDC: {skip: "does not accept write concern"},
    saslContinue: {skip: "does not accept write concern"},
    saslStart: {skip: "does not accept write concern"},
    sbe: {skip: "internal command"},
    serverStatus: {skip: "does not accept write concern"},
    setAllowMigrations: wcCommandsTests["setAllowMigrations"],
    setAuditConfig: {skip: "does not accept write concern"},
    setCommittedSnapshot: {skip: "internal command"},
    setDefaultRWConcern: wcCommandsTests["setDefaultRWConcern"],
    setFeatureCompatibilityVersion: wcCommandsTests["setFeatureCompatibilityVersion"],
    setProfilingFilterGlobally: {skip: "does not accept write concern"},
    setIndexCommitQuorum: {skip: "not supported on timeseries views"},
    setParameter: {skip: "does not accept write concern"},
    setShardVersion: {skip: "internal command"},
    setChangeStreamState: {skip: "does not accept write concern"},
    setClusterParameter: {skip: "does not accept write concern"},
    setQuerySettings: {skip: "does not accept write concern"},
    removeQuerySettings: {skip: "does not accept write concern"},
    setUserWriteBlockMode: {skip: "does not accept write concern"},
    shardCollection: wcCommandsTests["shardCollection"],
    shardingState: {skip: "does not accept write concern"},
    shutdown: {skip: "does not accept write concern"},
    sleep: {skip: "does not accept write concern"},
    split: {skip: "does not accept write concern"},
    splitChunk: {skip: "does not accept write concern"},
    splitVector: {skip: "internal command"},
    stageDebug: {skip: "does not accept write concern"},
    startRecordingTraffic: {skip: "does not accept write concern"},
    startSession: {skip: "does not accept write concern"},
    stopRecordingTraffic: {skip: "does not accept write concern"},
    sysprofile: {skip: "internal command"},
    testCommandFeatureFlaggedOnLatestFCV: {skip: "internal command"},
    testDeprecation: {skip: "test command"},
    testDeprecationInVersion2: {skip: "test command"},
    testInternalTransactions: {skip: "internal command"},
    testRemoval: {skip: "test command"},
    testReshardCloneCollection: {skip: "internal command"},
    testVersions1And2: {skip: "test command"},
    testVersion2: {skip: "test command"},
    timeseriesCatalogBucketParamsChanged: {skip: "internal command"},
    top: {skip: "does not accept write concern"},
    transitionFromDedicatedConfigServer: {skip: "unrelated"},
    transitionToDedicatedConfigServer: {skip: "unrelated"},
    transitionToShardedCluster: {skip: "internal command"},
    unshardCollection: {skip: "does not accept write concern"},
    untrackUnshardedCollection: {skip: "does not accept write concern"},
    update: {
        noop: {
            // The query will not match any doc
            req: {
                update: collName,
                updates: [{q: {"meta.x": 0}, u: {$set: {"meta.y": 1}}, multi: true}]
            },
            setupFunc: (coll) => {},
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 0);
                assert.eq(res.nModified, 0);
                assert.eq(coll.find().itcount(), 0);
            },
        },
        success: {
            // Basic update
            req: {
                update: collName,
                updates: [{q: {"meta.x": 1}, u: {$set: {"meta.y": 2}}, multi: true}]
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert([{meta: {x: 1, y: 1}, time: timeValue}]));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(coll.find({"meta.y": 2}).toArray().length, 1);
            },
        },
        failure: {
            req: {
                update: collName,
                updates:
                    [{q: {"meta.x": 1}, u: {$set: {"meta.y": 2, time: "deadbeef"}}, multi: true}]
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1, y: 1}, time: timeValue}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.writeErrors && res.writeErrors.length == 1);
                assert.includes([ErrorCodes.InvalidOptions, ErrorCodes.BadValue],
                                res.writeErrors[0]["code"]);
                assert.eq(res.n, 0);
                assert.eq(res.nModified, 0);
                assert.eq(coll.count({"meta.y": 1}), 1);
            },
        },
    },
    updateRole: wcCommandsTests["updateRole"],
    updateSearchIndex: {skip: "does not accept write concern"},
    updateUser: wcCommandsTests["updateRole"],
    updateZoneKeyRange: {skip: "does not accept write concern"},
    usersInfo: {skip: "does not accept write concern"},
    validate: {skip: "does not accept write concern"},
    validateDBMetadata: {skip: "does not accept write concern"},
    voteAbortIndexBuild: {skip: "internal command"},
    voteCommitImportCollection: {skip: "internal command"},
    voteCommitIndexBuild: {skip: "internal command"},
    waitForFailPoint: {skip: "test command"},
    getShardingReady: {skip: "internal command"},
    whatsmysni: {skip: "does not accept write concern"},
    whatsmyuri: {skip: "internal command"},
};

// A list of additional CRUD ops which exercise different write paths, and do error handling
// differently than the basic write path exercised in wcCommandsTestsT.
const additionalCRUDOpsTimeseriesViews = {
    "deleteMany": {
        noop: {
            req: {delete: collName, deletes: [{q: {"meta.x": {$lt: 0}}, limit: 0}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1}, time: timeValue}));
                assert.commandWorked(coll.insert({meta: {x: 2}, time: timeValue}));
                assert.eq(coll.find().itcount(), 2);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 0);
                assert.eq(coll.find().itcount(), 2);
            },
        },
        success: {
            req: {delete: collName, deletes: [{q: {"meta.x": {$gt: -5}}, limit: 0}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1}, time: timeValue}));
                assert.commandWorked(coll.insert({meta: {x: 2}, time: timeValue}));
                assert.eq(coll.find().itcount(), 2);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 2);
                assert.eq(coll.find().itcount(), 0);
            },
        },
    },
    "insertMany": {
        success: {
            req: {
                insert: collName,
                documents: [{meta: -2, time: timeValue}, {meta: 2, time: timeValue}]
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: 1, time: timeValue}));
                assert.eq(coll.find().itcount(), 1);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 2);
                assert.eq(coll.find().itcount(), 3);
            },
        },
        failure: {
            req: {insert: collName, documents: [{meta: -2, time: timeValue}, {meta: 2}]},
            setupFunc: (coll) => {},
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.writeErrors && res.writeErrors.length == 1);
                assert.includes([5743702, ErrorCodes.BadValue], res.writeErrors[0].code);
                assert.eq(res.n, 1);
                assert.eq(coll.find().itcount(), 1);
            },
        },
    },
    "updateMany": {
        noop: {
            req: {
                update: collName,
                updates: [{q: {"meta.x": {$gt: -21}}, u: {$set: {"meta.y": 1}}, multi: true}]
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22}, time: ISODate()},
                    {meta: {x: -20}, time: ISODate()},
                    {meta: {x: 20}, time: ISODate()},
                    {meta: {x: 21}, time: ISODate()}
                ]));
                assert.commandWorked(coll.remove({"meta.x": {$gt: -21}}));
                assert.eq(coll.find().itcount(), 1);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.nModified, 0);
                assert.eq(res.n, 0);
                assert.eq(coll.find({"meta.y": 1}).toArray().length, 0);
            },
        },
        success: {
            req: {
                update: collName,
                updates: [{q: {"meta.x": {$gt: -21}}, u: {$set: {"meta.y": 1}}, multi: true}]
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22}, time: ISODate()},
                    {meta: {x: -20}, time: ISODate()},
                    {meta: {x: 20}, time: ISODate()},
                    {meta: {x: 21}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.nModified, 3);
                assert.eq(res.n, 3);
                assert.eq(coll.find({"meta.y": 1}).toArray().length, 3);
            },
        },
        failure: {
            req: {
                update: collName,
                updates: [{q: {"meta.x": {$gt: -5}}, u: {$set: {time: "deadbeef"}}, multi: true}]
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1}, time: ISODate()}));
                assert.commandWorked(coll.insert({meta: {x: 2}, time: ISODate()}));
                assert.eq(coll.find().itcount(), 2);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.writeErrors && res.writeErrors.length == 1);
                assert.includes([ErrorCodes.BadValue, ErrorCodes.InvalidOptions],
                                res.writeErrors[0].code);
                assert.eq(res.n, 0);
                assert.eq(res.nModified, 0);
            },
        }
    },
    "findOneAndRemove": {
        noop: {
            req: {findAndModify: collName, query: {"meta.x": 1}, remove: true},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1}, time: timeValue}));
                assert.commandWorked(coll.remove({meta: {x: 1}, time: timeValue}));
                assert.eq(coll.find().itcount(), 0);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta.x": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                }
                assert.eq(res.value, null);
                assert.eq(coll.find().itcount(), 0);
            },
        },
        success: {
            req: {findAndModify: collName, query: {"meta.x": 1}, remove: true},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1}, time: timeValue}));
                assert.eq(coll.find().itcount(), 1);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta.x": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(res.value.meta.x, 1);
                    assert.eq(coll.find().itcount(), 0);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                    assert.eq(coll.find().itcount(), 1);
                }
            },
        }
    },
    "findOneAndUpdate": {
        // Modifier updates
        // No noop and success cases as we cannot perform a non-multi update on a
        // time-series
        // collection. findAndModify does not support multi updates.
        failure: {
            req: {
                findAndModify: collName,
                query: {"meta.x": 1},
                update: {time: "deadbeef"},
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({meta: {x: 1}, time: timeValue}));
                assert.eq(coll.find().itcount(), 1);
            },
            confirmFunc: (res, coll) => {
                assert.includes([ErrorCodes.BadValue, ErrorCodes.InvalidOptions], res.code);
            },
        },
    },
    "unorderedBatch": {
        noop: {
            // The two writes would execute the same update, so one will be a no-op.
            req: {
                update: collName,
                updates: [
                    {q: {"meta.x": 21}, u: {$set: {"meta.y": 21}}, multi: true},
                    {q: {"meta.x": {$gte: 20}}, u: {$set: {"meta.y": 21}}, multi: true}
                ],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22, y: 21}, time: ISODate()},
                    {meta: {x: -20, y: 21}, time: ISODate()},
                    {meta: {x: 20, y: 21}, time: ISODate()},
                    {meta: {x: 21}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert.eq(res.nModified, 1);
                assert.eq(coll.find({"meta.y": 21}).toArray().length, 4);
            },
        },
        success: {
            // All updates should succeed, but all should return WCEs.
            req: {
                update: collName,
                updates: [
                    {q: {"meta.x": -20}, u: {$set: {"meta.y": 1}}, multi: true},
                    {q: {"meta.x": 20}, u: {$set: {"meta.y": 1}}, multi: true},
                    {q: {"meta.x": 21}, u: {$set: {"meta.y": 1}}, multi: true}
                ],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22, y: 21}, time: ISODate()},
                    {meta: {x: -20, y: 21}, time: ISODate()},
                    {meta: {x: 20, y: 21}, time: ISODate()},
                    {meta: {x: 21}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta.x": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(res.n, 3);
                    assert.eq(res.nModified, 3);
                    assert.eq(coll.find({"meta.y": 1}).toArray().length, 3);
                } else {
                    // The two phase write path returns WriteConcernTimeout in the write
                    // errors array.
                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 3);
                    res.writeErrors.forEach((err) => {
                        assert.eq(err.code, ErrorCodes.WriteConcernTimeout);
                    });
                    assert.eq(res.nModified, 0);
                    assert.eq(coll.find({"meta.y": 1}).toArray().length, 3);
                }
            },
        },
        failure: {
            // The second update should fail. This is an unordered batch, so the
            // other two updates should succeed, but still return WCEs.
            req: {
                update: collName,
                updates: [
                    {q: {"meta.x": -20}, u: {$set: {"meta.y": 3}}, multi: true},
                    {q: {"meta.x": 20}, u: {$set: {time: "deadbeef"}}, multi: true},
                    {q: {"meta.x": 21}, u: {$set: {"meta.y": 3}}, multi: true}
                ],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22}, time: ISODate()},
                    {meta: {x: -20}, time: ISODate()},
                    {meta: {x: 20}, time: ISODate()},
                    {meta: {x: 21}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta.x": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 1);
                    assert.includes([ErrorCodes.BadValue, ErrorCodes.InvalidOptions],
                                    res.writeErrors[0].code);
                    assert.eq(res.nModified, 2);
                    assert.eq(coll.find().itcount(), 4);
                    assert.eq(coll.find({"meta.y": {$exists: true}}).toArray().length, 2);
                } else {
                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 3);
                    assert.eq(res.writeErrors[0].code, ErrorCodes.WriteConcernTimeout);
                    assert.includes([ErrorCodes.BadValue, ErrorCodes.InvalidOptions],
                                    res.writeErrors[1].code);
                    assert.eq(res.writeErrors[2].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(coll.find({"meta.y": {$exists: true}}).toArray().length, 0);
                }
            },
        }
    },
    "orderedBatch": {
        noop: {
            // The last update is a no-op.
            req: {
                update: collName,
                updates: [
                    {q: {"meta.x": {$gte: -21}}, u: {$set: {"meta.y": 1}}, multi: true},
                    {q: {"meta.x": 21}, u: {$set: {"meta.y": 1}}, multi: true}
                ],
                ordered: true
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22}, time: ISODate()},
                    {meta: {x: -20}, time: ISODate()},
                    {meta: {x: 20}, time: ISODate()},
                    {meta: {x: 21}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert.eq(res.nModified, 3);
                assert.eq(coll.find({"meta.y": 1}).itcount(), 3);
            },
        },
        success: {
            // All updates should succeed, but all should return WCEs.
            req: {
                update: collName,
                updates: [
                    {q: {"meta.x": -20}, u: {$set: {"meta.y": 1}}, multi: true},
                    {q: {"meta.x": 20}, u: {$set: {"meta.y": 1}}, multi: true},
                    {q: {"meta.x": 21}, u: {$set: {"meta.y": 1}}, multi: true}
                ],
                ordered: true
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22}, time: ISODate()},
                    {meta: {x: -20}, time: ISODate()},
                    {meta: {x: 20}, time: ISODate()},
                    {meta: {x: 21}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta.x": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(res.nModified, 3);
                    assert.eq(coll.find({"meta.y": 1}).toArray().length, 3);
                } else {
                    // We stop execution after the first write because it fails with a WCE

                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 1);
                    assert.eq(res.writeErrors[0].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nModified, 0);
                    assert.eq(coll.find({"meta.y": 1}).toArray().length, 0);
                }
            },
        },
        failure: {
            // The second update should fail. This is an ordered batch, so the
            // last update should not be executed. The first and second should still return
            // WCEs.
            req: {
                update: collName,
                updates: [
                    {q: {"meta.x": -20}, u: {$set: {"meta.y": 3}}, multi: true},
                    {q: {"meta.x": 20}, u: {$set: {time: 'deadbeef'}}, multi: true},
                    {q: {"meta.x": 21}, u: {$set: {"meta.y": 3}}, multi: true}
                ],
                ordered: true
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22}, time: ISODate()},
                    {meta: {x: -20}, time: ISODate()},
                    {meta: {x: 20}, time: ISODate()},
                    {meta: {x: 21}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta.x": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 1);
                    assert.includes([ErrorCodes.BadValue, ErrorCodes.InvalidOptions],
                                    res.writeErrors[0].code);
                    assert.eq(res.nModified, 1);
                    assert.eq(coll.find().itcount(), 4);
                    assert.eq(coll.find({"meta.y": {$exists: true}}).toArray().length, 1);
                } else {
                    // We stop execution after the first write because it fails with a WCE
                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 1);
                    assert.eq(res.writeErrors[0].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nModified, 0);
                    assert.eq(coll.find({"meta.y": 1}).toArray().length, 0);
                }
            },
        }
    },
    "bulkWriteUnordered": {
        // The two writes would execute the same delete, so one will be a no-op.
        noop: {
            req: {
                bulkWrite: 1,
                ops: [
                    {delete: 0, filter: {"meta.x": {$gte: -20}}, multi: true},
                    {delete: 0, filter: {"meta.x": -20}, multi: true}
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert(
                    [{meta: {x: -22}, time: ISODate()}, {meta: {x: -20}, time: ISODate()}]));
                assert.eq(coll.find().itcount(), 2);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.cursor.firstBatch.length, 2);

                assert.eq(res.cursor.firstBatch[0].ok, 1);
                assert.eq(res.cursor.firstBatch[0].n, 1);

                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta.x": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.eq(res.cursor.firstBatch[1].ok, 1);
                    assert.eq(res.nErrors, 0);
                } else {
                    assert.eq(res.cursor.firstBatch[1].ok, 0);
                    assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nErrors, 1);
                }
                assert.eq(res.cursor.firstBatch[1].n, 0);

                assert.eq(res.nDeleted, 1);
                assert.eq(coll.find().itcount(), 1);
            },
            admin: true,
        },
        success: {
            req: {
                bulkWrite: 1,
                ops: [
                    {insert: 0, document: {meta: 22, time: timeValue}},
                    {
                        update: 0,
                        filter: {"meta.x": -20},
                        updateMods: {$set: {"meta.y": 2}},
                        multi: true
                    }
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22}, time: ISODate()},
                    {meta: {x: -20}, time: ISODate()},
                    {meta: {x: 20}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 3);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);

                assert.eq(res.cursor.firstBatch.length, 2);
                assert.eq(res.cursor.firstBatch[0].ok, 1);

                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta.x": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.eq(res.cursor.firstBatch[1].ok, 1);
                    assert.eq(res.nErrors, 0);
                    assert.eq(res.nModified, 1);
                    assert.eq(coll.find({"meta.y": 2}).itcount(), 1);
                } else {
                    assert.eq(res.cursor.firstBatch[1].ok, 0);
                    assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nErrors, 1);
                    assert.eq(res.nModified, 0);
                    assert.eq(coll.find({"meta.y": 2}).itcount(), 0);
                }
                assert.eq(res.nInserted, 1);
                assert.eq(coll.find().itcount(), 4);
            },
            admin: true,
        },
        failure: {
            // The second update should fail, but this is an unordered batch so the other 2
            // updates should succeed.
            req: {
                bulkWrite: 1,
                ops: [
                    {
                        update: 0,
                        filter: {"meta.x": {$gte: -20}},
                        updateMods: {$set: {"meta.y": 5}},
                        multi: true
                    },
                    {
                        update: 0,
                        filter: {"meta.x": -22},
                        updateMods: {$set: {time: "deadbeef"}},
                        multi: true
                    },
                    {
                        update: 0,
                        filter: {"meta.x": -22},
                        updateMods: {$set: {"meta.y": 4}},
                        multi: true
                    }
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}, {ns: fullNs}],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22}, time: ISODate()},
                    {meta: {x: -20}, time: ISODate()},
                    {meta: {x: 20}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 3);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert.eq(res.cursor.firstBatch.length, 3);

                assert.eq(res.cursor.firstBatch[0].ok, 1);
                assert.eq(res.cursor.firstBatch[0].n, 2);

                assert.eq(res.cursor.firstBatch[1].ok, 0);
                assert.includes([ErrorCodes.BadValue, ErrorCodes.InvalidOptions],
                                res.cursor.firstBatch[1].code);

                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta.x": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.eq(res.cursor.firstBatch[2].ok, 1);
                    assert.eq(res.cursor.firstBatch[2].n, 1);
                    assert.eq(res.nErrors, 1);
                    assert.eq(res.nModified, 3);
                    assert.eq(coll.find({"meta.y": 4}).itcount(), 1);
                } else {
                    assert.eq(res.cursor.firstBatch[2].ok, 0);
                    assert.eq(res.cursor.firstBatch[2].n, 0);
                    assert.eq(res.cursor.firstBatch[2].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nErrors, 2);
                    assert.eq(res.nModified, 2);
                    assert.eq(coll.find({"meta.y": 4}).itcount(), 1);
                }

                assert.eq(coll.find().itcount(), 3);
                assert.eq(coll.find({"meta.y": 5}).itcount(), 2);
            },
            admin: true,
        },
    },
    "bulkWriteOrdered": {
        // The two deletes would remove the same doc, so one will be a no-op.
        noop: {
            req: {
                bulkWrite: 1,
                ops: [
                    {delete: 0, filter: {"meta.x": {$gte: -20}}, multi: true},
                    {delete: 1, filter: {"meta.x": -20}},
                    {insert: 0, document: {meta: 25, time: timeValue}}
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}, {ns: fullNs}],
                ordered: true
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert(
                    [{meta: {x: -22}, time: ISODate()}, {meta: {x: -20}, time: ISODate()}]));
                assert.eq(coll.find().itcount(), 2);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);

                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.eq(res.cursor.firstBatch.length, 3);

                    assert.eq(res.cursor.firstBatch[0].ok, 1);
                    assert.eq(res.cursor.firstBatch[0].n, 1);

                    assert.eq(res.cursor.firstBatch[1].ok, 1);
                    assert.eq(res.cursor.firstBatch[1].n, 0);

                    assert.eq(res.cursor.firstBatch[2].ok, 1);

                    assert.eq(res.nErrors, 0);
                    assert.eq(coll.find().itcount(), 2);
                } else {
                    // The write without shard key will execute a transaction and fail the
                    // write with WriteConcernTimeout, so the insert will not execute.
                    assert.eq(res.cursor.firstBatch.length, 2);

                    assert.eq(res.cursor.firstBatch[0].ok, 1);
                    assert.eq(res.cursor.firstBatch[0].n, 1);

                    assert.eq(res.cursor.firstBatch[1].ok, 0);
                    assert.eq(res.cursor.firstBatch[1].n, 0);
                    assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.WriteConcernTimeout);

                    assert.eq(res.nErrors, 1);
                    assert.eq(coll.find().itcount(), 1);
                }
            },
            admin: true,
        },
        success: {
            req: {
                bulkWrite: 1,
                ops: [
                    {
                        update: 0,
                        filter: {"meta.x": {$gte: -10}},
                        updateMods: {$set: {"meta.y": 1}},
                        multi: true
                    },
                    {
                        update: 1,
                        filter: {"meta.x": -20},
                        updateMods: {$set: {"meta.y": 2}},
                        multi: true
                    }
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}],
                ordered: true
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22}, time: ISODate()},
                    {meta: {x: -20}, time: ISODate()},
                    {meta: {x: 20}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 3);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);

                assert.eq(res.cursor.firstBatch.length, 2);
                assert.eq(res.cursor.firstBatch[0].ok, 1);

                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"meta.x": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.eq(res.cursor.firstBatch[1].ok, 1);
                    assert.eq(res.nErrors, 0);
                    assert.eq(res.nModified, 2);
                    assert.eq(coll.find({"meta.y": 2}).itcount(), 1);
                } else {
                    assert.eq(res.cursor.firstBatch[1].ok, 0);
                    assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nErrors, 1);
                    assert.eq(res.nModified, 1);
                    assert.eq(coll.find({"meta.y": 2}).itcount(), 0);
                }

                assert.eq(coll.find().itcount(), 3);
                assert.eq(coll.find({"meta.y": 1}).itcount(), 1);
            },
            admin: true,
        },
        // The second update will fail, and this is an ordered batch so the final update
        // should
        // not execute.
        failure: {
            req: {
                bulkWrite: 1,
                ops: [
                    {
                        update: 0,
                        filter: {"meta.x": {$gte: -20}},
                        updateMods: {$set: {"meta.y": 5}},
                        multi: true
                    },
                    {
                        update: 0,
                        filter: {"meta.x": -22},
                        updateMods: {$set: {time: "deadbeef"}},
                        multi: true
                    },
                    {
                        update: 0,
                        filter: {"meta.x": -22},
                        updateMods: {$set: {"meta.y": 4}},
                        multi: true
                    }
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}, {ns: fullNs}],
                ordered: true
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert([
                    {meta: {x: -22}, time: ISODate()},
                    {meta: {x: -20}, time: ISODate()},
                    {meta: {x: 20}, time: ISODate()}
                ]));
                assert.eq(coll.find().itcount(), 3);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert.eq(res.cursor.firstBatch.length, 2);

                assert.eq(res.cursor.firstBatch[0].ok, 1);
                assert.eq(res.cursor.firstBatch[0].n, 2);

                assert.eq(res.cursor.firstBatch[1].ok, 0);
                assert.includes([ErrorCodes.BadValue, ErrorCodes.InvalidOptions],
                                res.cursor.firstBatch[1].code);

                assert.eq(res.nErrors, 1);
                assert.eq(res.nModified, 2);
                assert.eq(coll.find().itcount(), 3);
                assert.eq(coll.find({"meta.y": 5}).itcount(), 2);
                assert.eq(coll.find({"meta.y": 4}).itcount(), 0);
            },
            admin: true,
        },
    },
};

// A list of additional CRUD ops which exercise different write paths, and do error handling
// differently than the basic write path exercised in the test cases above.
let additionalCRUDOps = {
    "deleteMany": {
        noop: {
            req: {delete: collName, deletes: [{q: {a: {$lt: 0}}, limit: 0}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(coll.insert({a: 2}));
                assert.eq(coll.find().itcount(), 2);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 0);
                assert.eq(coll.find().itcount(), 2);
            },
        },
        success: {
            req: {delete: collName, deletes: [{q: {a: {$gt: -5}}, limit: 0}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(coll.insert({a: -1}));
                assert.eq(coll.find().itcount(), 2);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 2);
                assert.eq(coll.find().itcount(), 0);
            },
        },
    },
    "insertMany": {
        success: {
            req: {insert: collName, documents: [{a: -2}, {a: 2}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({_id: 1}));
                assert.eq(coll.find().itcount(), 1);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.n, 2);
                assert.eq(coll.find().itcount(), 3);
            },
        },
        failure: {
            req: {insert: collName, documents: [{a: -2}, {a: 2}], ordered: false},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(
                    coll.getDB().runCommand({collMod: collName, validator: {x: {$exists: true}}}));
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.writeErrors && res.writeErrors.length == 2);
                assert.eq(res.writeErrors[0].code, ErrorCodes.DocumentValidationFailure);
                assert.eq(res.writeErrors[1].code, ErrorCodes.DocumentValidationFailure);
                assert.eq(res.n, 0);
                assert.eq(coll.find().itcount(), 1);
            },
        },
    },
    "updateMany": {
        noop: {
            req:
                {update: collName, updates: [{q: {a: {$gt: -21}}, u: {$set: {b: 1}}, multi: true}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}, {a: 20}, {a: 21}]));
                assert.commandWorked(coll.remove({a: {$gt: -21}}));
                assert.eq(coll.find().itcount(), 1);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.nModified, 0);
                assert.eq(res.n, 0);
                assert.eq(coll.find({b: 1}).toArray().length, 0);
            },
        },
        success: {
            req:
                {update: collName, updates: [{q: {a: {$gt: -21}}, u: {$set: {b: 1}}, multi: true}]},
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}, {a: 20}, {a: 21}]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.nModified, 3);
                assert.eq(res.n, 3);
                assert.eq(coll.find({b: 1}).toArray().length, 3);
            },
        },
        failure: {
            req: {update: collName, updates: [{q: {a: {$gt: -5}}, u: {$set: {b: 1}}, multi: true}]},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(coll.insert({a: 2}));
                assert.commandWorked(
                    coll.getDB().runCommand({collMod: collName, validator: {b: {$gt: 2}}}));
                assert.eq(coll.find().itcount(), 2);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.writeErrors && res.writeErrors.length == 1);
                assert.eq(res.writeErrors[0].code, ErrorCodes.DocumentValidationFailure);
                assert.eq(res.n, 0);
                assert.eq(res.nModified, 0);
                assert.eq(coll.count({b: 1}), 0);
            },
        }
    },
    "findOneAndRemove": {
        noop: {
            req: {findAndModify: collName, query: {a: 1}, remove: true},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(coll.remove({a: 1}));
                assert.eq(coll.find().itcount(), 0);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                }
                assert.eq(res.value, null);
                assert.eq(coll.find().itcount(), 0);
            },
        },
        success: {
            req: {findAndModify: collName, query: {a: 1}, remove: true},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.eq(coll.find().itcount(), 1);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(res.value.a, 1);
                    assert.eq(coll.find().itcount(), 0);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                    assert.eq(coll.find().itcount(), 1);
                }
            },
        }
    },
    "findOneAndUpdate": {
        // Modifier updates
        noop: {
            req: {findAndModify: collName, query: {a: 1}, update: {$set: {c: 2}}},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorkedIgnoringWriteConcernErrors(coll.getDB().runCommand(
                    {findAndModify: collName, query: {a: 1}, update: {$set: {c: 2}}}));
                assert.eq(coll.count({a: 1, c: 2}), 1);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(res.lastErrorObject.updatedExisting, true);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                }
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.count({a: 1, c: 2}), 1);
            },
        },
        success: {
            req: {findAndModify: collName, query: {a: 1}, update: {$set: {c: 2}}},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1}));
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(res.lastErrorObject.updatedExisting, true);
                    assert.eq(coll.find().itcount(), 1);
                    assert.eq(coll.count({a: 1, c: 2}), 1);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
                    assert.eq(coll.count({a: 1, c: 2}), 0);
                }
            },
        },
        failure: {
            req: {findAndModify: collName, query: {a: 1}, update: {$set: {value: 3}}},
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert({a: 1, value: 1}));
                assert.eq(coll.find().itcount(), 1);
                assert.commandWorked(
                    coll.getDB().runCommand({collMod: collName, validator: {value: {$gt: 4}}}));
            },
            confirmFunc: (res, coll) => {
                assert.commandFailedWithCode(res, ErrorCodes.DocumentValidationFailure);
                assert.eq(coll.find({a: 1, value: 1}).itcount(), 1);
            },
        }
    },
    "unorderedBatch": {
        noop: {
            // The two writes would execute the same update, so one will be a no-op.
            req: {
                update: collName,
                updates: [
                    {q: {a: 21}, u: {$set: {b: 1}}},
                    {q: {a: {$gte: -20}}, u: {$set: {b: 1}}, multi: true}
                ],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(
                    coll.insert([{a: -22, b: 1}, {a: -20, b: 1}, {a: 20, b: 1}, {a: 21}]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert.eq(res.nModified, 1);
                assert.eq(coll.find({b: 1}).toArray().length, 4);
            },
        },
        success: {
            // All updates should succeed, but all should return WCEs.
            req: {
                update: collName,
                updates: [
                    {q: {a: -20}, u: {$set: {b: 1}}},
                    {q: {a: 20}, u: {$set: {b: 1}}},
                    {q: {a: 21}, u: {$set: {b: 1}}}
                ],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}, {a: 20}, {a: 21}]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(res.n, 3);
                    assert.eq(res.nModified, 3);
                    assert.eq(coll.find({b: 1}).toArray().length, 3);
                } else {
                    // The two phase write path returns WriteConcernTimeout in the write
                    // errors array.
                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 3);
                    res.writeErrors.forEach((err) => {
                        assert.eq(err.code, ErrorCodes.WriteConcernTimeout);
                    });
                    assert.eq(res.nModified, 0);
                    assert.eq(coll.find({b: 1}).toArray().length, 0);
                }
            },
        },
        failure: {
            // The second update should fail the validator. This is an unordered batch, so the
            // other two updates should succeed, but still return WCEs.
            req: {
                update: collName,
                updates: [
                    {q: {a: -20}, u: {$set: {b: 3}}},
                    {q: {a: 20}, u: {$set: {b: 1}}},
                    {q: {a: 21}, u: {$set: {b: 3}}}
                ],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}, {a: 20}, {a: 21}]));
                assert.commandWorked(
                    coll.getDB().runCommand({collMod: collName, validator: {b: {$gt: 2}}}));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 1);
                    assert.eq(res.writeErrors[0].code, ErrorCodes.DocumentValidationFailure);
                    assert.eq(res.nModified, 2);
                    assert.eq(coll.find().itcount(), 4);
                    assert.eq(coll.find({b: {$exists: true}}).toArray().length, 2);
                } else {
                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 3);
                    assert.eq(res.writeErrors[0].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.writeErrors[1].code, ErrorCodes.DocumentValidationFailure);
                    assert.eq(res.writeErrors[2].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(coll.find({b: {$exists: true}}).toArray().length, 0);
                }
            },
        }
    },
    "orderedBatch": {
        noop: {
            // The last update is a no-op.
            req: {
                update: collName,
                updates: [
                    {q: {a: {$gte: -21}}, u: {$set: {b: 1}}, multi: true},
                    {q: {a: 21}, u: {$set: {b: 1}}}
                ],
                ordered: true
            },
            setupFunc: (coll) => {
                assert.commandWorked(
                    coll.insert([{a: -22, b: 1}, {a: -20, b: 1}, {a: 20, b: 1}, {a: 21}]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert.eq(res.nModified, 1);
                assert.eq(coll.find({b: 1}).itcount(), 4);
            },
        },
        success: {
            // All updates should succeed, but all should return WCEs.
            req: {
                update: collName,
                updates: [
                    {q: {a: -20}, u: {$set: {b: 1}}},
                    {q: {a: 20}, u: {$set: {b: 1}}},
                    {q: {a: 21}, u: {$set: {b: 1}}}
                ],
                ordered: true
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}, {a: 20}, {a: 21}]));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(res.nModified, 3);
                    assert.eq(coll.find({b: 1}).toArray().length, 3);
                } else {
                    // We stop execution after the first write because it fails with a WCE

                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 1);
                    assert.eq(res.writeErrors[0].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nModified, 0);
                    assert.eq(coll.find({b: 1}).toArray().length, 0);
                }
            },
        },
        failure: {
            // The second update should fail the validator. This is an ordered batch, so the
            // last update should not be executed. The first and second should still return
            // WCEs.
            req: {
                update: collName,
                updates: [
                    {q: {a: -20}, u: {$set: {b: 3}}},
                    {q: {a: 20}, u: {$set: {b: 1}}},
                    {q: {a: 21}, u: {$set: {b: 3}}}
                ],
                ordered: true
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}, {a: 20}, {a: 21}]));
                assert.commandWorked(
                    coll.getDB().runCommand({collMod: collName, validator: {b: {$gt: 2}}}));
                assert.eq(coll.find().itcount(), 4);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 1);
                    assert.eq(res.writeErrors[0].code, ErrorCodes.DocumentValidationFailure);
                    assert.eq(res.nModified, 1);
                    assert.eq(coll.find().itcount(), 4);
                    assert.eq(coll.find({b: {$exists: true}}).toArray().length, 1);
                } else {
                    // We stop execution after the first write because it fails with a WCE
                    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                    assert(res.writeErrors && res.writeErrors.length == 1);
                    assert.eq(res.writeErrors[0].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nModified, 0);
                    assert.eq(coll.find({b: 1}).toArray().length, 0);
                }
            },
        }
    },
    "bulkWriteUnordered": {
        // The two writes would execute the same delete, so one will be a no-op.
        noop: {
            req: {
                bulkWrite: 1,
                ops: [
                    {delete: 0, filter: {a: {$gte: -20}}, multi: true},
                    {delete: 0, filter: {a: -20}}
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}]));
                assert.eq(coll.find().itcount(), 2);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert.eq(res.cursor.firstBatch.length, 2);

                assert.eq(res.cursor.firstBatch[0].ok, 1);
                assert.eq(res.cursor.firstBatch[0].n, 1);

                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.eq(res.cursor.firstBatch[1].ok, 1);
                    assert.eq(res.nErrors, 0);
                } else {
                    assert.eq(res.cursor.firstBatch[1].ok, 0);
                    assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nErrors, 1);
                }
                assert.eq(res.cursor.firstBatch[1].n, 0);

                assert.eq(res.nDeleted, 1);
                assert.eq(coll.find().itcount(), 1);
            },
            admin: true,
        },
        success: {
            req: {
                bulkWrite: 1,
                ops: [
                    {insert: 0, document: {a: 22}},
                    {update: 0, filter: {a: -20}, updateMods: {$set: {b: 2}}}
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}, {a: 20}]));
                assert.eq(coll.find().itcount(), 3);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);

                assert.eq(res.cursor.firstBatch.length, 2);
                assert.eq(res.cursor.firstBatch[0].ok, 1);

                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.eq(res.cursor.firstBatch[1].ok, 1);
                    assert.eq(res.nErrors, 0);
                    assert.eq(res.nModified, 1);
                    assert.eq(coll.find({b: 2}).itcount(), 1);
                } else {
                    assert.eq(res.cursor.firstBatch[1].ok, 0);
                    assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nErrors, 1);
                    assert.eq(res.nModified, 0);
                    assert.eq(coll.find({b: 2}).itcount(), 0);
                }
                assert.eq(res.nInserted, 1);
                assert.eq(coll.find().itcount(), 4);
            },
            admin: true,
        },
        failure: {
            // The second update should fail, but this is an unordered batch so the other 2
            // updates should succeed.
            req: {
                bulkWrite: 1,
                ops: [
                    {update: 0, filter: {a: {$gte: -20}}, updateMods: {$set: {b: 5}}, multi: true},
                    {update: 0, filter: {a: -20}, updateMods: {$set: {b: 2}}},
                    {update: 0, filter: {a: -22}, updateMods: {$set: {b: 4}}}
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}, {ns: fullNs}],
                ordered: false
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}, {a: 20}]));
                assert.commandWorked(
                    coll.getDB().runCommand({collMod: collName, validator: {b: {$gt: 2}}}));
                assert.eq(coll.find().itcount(), 3);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert.eq(res.cursor.firstBatch.length, 3);

                assert.eq(res.cursor.firstBatch[0].ok, 1);
                assert.eq(res.cursor.firstBatch[0].n, 2);

                assert.eq(res.cursor.firstBatch[1].ok, 0);
                assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.DocumentValidationFailure);

                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.eq(res.cursor.firstBatch[2].ok, 1);
                    assert.eq(res.cursor.firstBatch[2].n, 1);
                    assert.eq(res.nErrors, 1);
                    assert.eq(res.nModified, 3);
                    assert.eq(coll.find({b: 4}).itcount(), 1);
                } else {
                    assert.eq(res.cursor.firstBatch[2].ok, 0);
                    assert.eq(res.cursor.firstBatch[2].n, 0);
                    assert.eq(res.cursor.firstBatch[2].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nErrors, 2);
                    assert.eq(res.nModified, 2);
                    assert.eq(coll.find({b: 4}).itcount(), 0);
                }

                assert.eq(coll.find().itcount(), 3);
                assert.eq(coll.find({b: 5}).itcount(), 2);
                assert.eq(coll.find({b: 2}).itcount(), 0);
            },
            admin: true,
        },
    },
    "bulkWriteOrdered": {
        // The two deletes would remove the same doc, so one will be a no-op.
        noop: {
            req: {
                bulkWrite: 1,
                ops: [
                    {delete: 0, filter: {a: {$gte: -20}}, multi: true},
                    {delete: 1, filter: {a: -20}},
                    {insert: 0, document: {a: 25}}
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}, {ns: fullNs}],
                ordered: true
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}]));
                assert.eq(coll.find().itcount(), 2);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);

                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.eq(res.cursor.firstBatch.length, 3);

                    assert.eq(res.cursor.firstBatch[0].ok, 1);
                    assert.eq(res.cursor.firstBatch[0].n, 1);

                    assert.eq(res.cursor.firstBatch[1].ok, 1);
                    assert.eq(res.cursor.firstBatch[1].n, 0);

                    assert.eq(res.cursor.firstBatch[2].ok, 1);

                    assert.eq(res.nErrors, 0);
                    assert.eq(coll.find().itcount(), 2);
                } else {
                    // The write without shard key will execute a transaction and fail the
                    // write with WriteConcernTimeout, so the insert will not execute.
                    assert.eq(res.cursor.firstBatch.length, 2);

                    assert.eq(res.cursor.firstBatch[0].ok, 1);
                    assert.eq(res.cursor.firstBatch[0].n, 1);

                    assert.eq(res.cursor.firstBatch[1].ok, 0);
                    assert.eq(res.cursor.firstBatch[1].n, 0);
                    assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.WriteConcernTimeout);

                    assert.eq(res.nErrors, 1);
                    assert.eq(coll.find().itcount(), 1);
                }
            },
            admin: true,
        },
        success: {
            req: {
                bulkWrite: 1,
                ops: [
                    {update: 0, filter: {a: {$gte: -10}}, updateMods: {$set: {b: 1}}, multi: true},
                    {update: 1, filter: {a: -20}, updateMods: {$set: {b: 2}}}
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}],
                ordered: true
            },
            setupFunc: (coll) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}, {a: 20}]));
                assert.eq(coll.find().itcount(), 3);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);

                assert.eq(res.cursor.firstBatch.length, 2);
                assert.eq(res.cursor.firstBatch[0].ok, 1);

                let sk = getShardKey(coll, fullNs);
                let writeWithoutSkOrId =
                    (bsonWoCompare(sk, {"a": 1}) != 0) && (bsonWoCompare(sk, {}) != 0);
                if (clusterType != "sharded" || !writeWithoutSkOrId) {
                    assert.eq(res.cursor.firstBatch[1].ok, 1);
                    assert.eq(res.nErrors, 0);
                    assert.eq(res.nModified, 2);
                    assert.eq(coll.find({b: 2}).itcount(), 1);
                } else {
                    assert.eq(res.cursor.firstBatch[1].ok, 0);
                    assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.WriteConcernTimeout);
                    assert.eq(res.nErrors, 1);
                    assert.eq(res.nModified, 1);
                    assert.eq(coll.find({b: 2}).itcount(), 0);
                }

                assert.eq(coll.find().itcount(), 3);
                assert.eq(coll.find({b: 1}).itcount(), 1);
            },
            admin: true,
        },
        // The second update will fail, and this is an ordered batch so the final update
        // should
        // not execute.
        failure: {
            req: {
                bulkWrite: 1,
                ops: [
                    {update: 0, filter: {a: {$gte: -20}}, updateMods: {$set: {b: 5}}, multi: true},
                    {update: 0, filter: {a: -20}, updateMods: {$set: {b: 2}}},
                    {update: 0, filter: {a: -22}, updateMods: {$set: {b: 4}}}
                ],
                nsInfo: [{ns: fullNs}, {ns: fullNs}, {ns: fullNs}],
                ordered: true
            },
            setupFunc: (coll, cluster, clusterType, secondariesRunning, optionalArgs) => {
                assert.commandWorked(coll.insert([{a: -22}, {a: -20}, {a: 20}]));
                assert.commandWorked(
                    coll.getDB().runCommand({collMod: collName, validator: {b: {$gt: 2}}}));
                assert.eq(coll.find().itcount(), 3);
            },
            confirmFunc: (res, coll, cluster, clusterType) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert.eq(res.cursor.firstBatch.length, 2);

                assert.eq(res.cursor.firstBatch[0].ok, 1);
                assert.eq(res.cursor.firstBatch[0].n, 2);

                assert.eq(res.cursor.firstBatch[1].ok, 0);
                assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.DocumentValidationFailure);

                assert.eq(res.nErrors, 1);
                assert.eq(res.nModified, 2);
                assert.eq(coll.find().itcount(), 3);
                assert.eq(coll.find({b: 5}).itcount(), 2);
                assert.eq(coll.find({b: 4}).itcount(), 0);
            },
            admin: true,
        },
    },
};

export function stopSecondaries(cluster, clusterType) {
    if (clusterType == "rs") {
        assert.eq(cluster.nodeList().length, 3);

        // Stop one of the secondaries so that w:3 will fail
        const secondary = cluster.getSecondaries()[0];
        cluster.stop(secondary);
        return [secondary];
    } else {
        assert.eq(clusterType, "sharded");

        let secondariesRunning = [];
        const shards = cluster.getAllShards();
        shards.forEach((rs) => {
            assert.eq(rs.nodeList().length, 3);

            // Stop one of the secondaries so that w:3 will fail. Some commands override user
            // write concern to be w: majority. For those specific test cases, we'll need to
            // shut down the other secondary to ensure waiting for write concern will still
            // fail, so we track the other secondary.
            const secondaries = rs.getSecondaries();
            rs.stop(secondaries[0]);
            secondariesRunning.push(secondaries[1]);
        });
        return secondariesRunning;
    }
}

export function restartSecondaries(cluster, clusterType) {
    if (clusterType == "rs") {
        // Restart the secondary
        cluster.restart(cluster.getSecondaries()[0]);
        cluster.waitForPrimary();
        cluster.awaitSecondaryNodes();
    } else {
        // Restart the secondaries
        const shards = cluster.getAllShards();
        shards.forEach((rs) => {
            const secondaries = rs.getSecondaries();
            rs.restart(secondaries[0]);
            rs.waitForPrimary();
            rs.awaitSecondaryNodes();
        });
    }
}

/**
 * Assert that the result contains a write concern error. Typically we expect WCEs to be
 * reported in the WriteConcernErrors field, but some commands nest the WCE and other actually
 * return the error as a top-level command error.
 */
export function assertHasWCE(res, cmd) {
    try {
        assertWriteConcernError(res);
    } catch (e) {
        try {
            // Some commands return WCEs in a nested "raw" field
            if (res.raw) {
                let hasWCE = false;
                Object.keys(res.raw).forEach((key) => {
                    if (res.raw[key].writeConcernError) {
                        assertWriteConcernError(res.raw[key]);
                        hasWCE = true;
                    }
                });
                assert(hasWCE);
            } else {
                // Some commands fail the command with WriteConcernTimeout as a top-level error,
                // and do not include a separate WCE field.
                assert.commandFailedWithCode(res, ErrorCodes.WriteConcernTimeout);
            }
        } catch (err) {
            throw e;
        }
    }
}

function runCommandTest(
    testCase, conn, coll, cluster, clusterType, preSetup, secondariesRunning, forceUseMajorityWC) {
    const dbName = coll.getDB().getName();

    // Drop collection.
    coll.drop();
    assert.eq(0, coll.find().itcount(), "test collection not empty");

    jsTestLog("Testing command: " + tojson(testCase.req));

    // Create environment for command to run in.
    if (preSetup) {
        preSetup(conn, cluster, dbName, collName);
    }

    let optionalArgs = {};
    testCase.setupFunc(coll, cluster, clusterType, secondariesRunning, optionalArgs);

    const request = (typeof (testCase.req) === "function" ? testCase.req(cluster, coll)
                                                          : Object.assign({}, testCase.req, {}));

    // Provide a small wtimeout that we expect to time out.
    if (forceUseMajorityWC) {
        request.writeConcern = {w: "majority", wtimeout: 1000};
    } else {
        request.writeConcern = {w: 3, wtimeout: 1000};
    }

    // We run the command on a different connection. If the the command were run on the
    // same connection, then the client last op for the noop write would be set by the setup
    // operation. By using a fresh connection the client last op begins as null.
    // This test explicitly tests that write concern for noop writes works when the
    // client last op has not already been set by a duplicate operation.
    const freshConn = new Mongo(conn.host);

    // We check the error code of 'res' in the 'confirmFunc'.
    const res = testCase.admin ? freshConn.adminCommand(request)
                               : freshConn.getDB(dbName).runCommand(request);

    try {
        // Tests that the command receives a write concern error.
        assertHasWCE(res, testCase.req);

        // Validate post-conditions of the commands.
        jsTestLog(tojson(getCommandName(request)) + " command returned " + tojson(res));
        testCase.confirmFunc(res, coll, cluster, clusterType, secondariesRunning, optionalArgs);
    } catch (e) {
        // Make sure that we print out the response.
        printjson(res);
        throw e;
    }

    // Kill the implicit session to abort any idle transactions to "force" reap.
    assert.commandWorkedOrFailedWithCode(
        freshConn.adminCommand(
            {killSessions: [freshConn.getDB(dbName).getSession().getSessionId()]}),
        ErrorCodes.HostUnreachable);
}

// TODO SERVER-97736 Modify `shouldSkipTestCase` to ensure these commands are not skipped once
// they no longer hang until the majority of the shards involved in DDL are available and return
// WCE on timing out.
const shardedDDLCommandsRequiringMajorityCommit = [
    "create",
    "changePrimary",
    "collMod",
    "convertToCapped",
    "drop",
    "dropDatabase",
    "movePrimary",
    "refineCollectionShardKey",
    "renameCollection",
    "setAllowMigrations",
    "shardCollection"
];

function shouldSkipTestCase(
    clusterType, command, testCase, shardedCollection, writeWithoutSk, coll) {
    if (!shardedCollection &&
        (command == "moveChunk" || command == "moveRange" ||
         command == "refineCollectionShardKey" || command == "setAllowMigrations" ||
         command == "updateDocSk")) {
        jsTestLog(
            "Skipping " + command +
            " because requires sharded collection, and the current collection is not sharded.");
        return true;
    }

    if (shardedCollection && command == "updateDocSk" &&
        bsonWoCompare(getShardKey(coll, fullNs), {"_id": 1}) == 0) {
        jsTestLog(
            "Skipping updating a document's shard key because the shard key is {_id: 1}, and the _id field is immutable.");
        return true;
    }

    if (shardedCollection && command == "shardCollection") {
        jsTestLog(
            "Skipping " + command +
            " because requires an unsharded collection, and the current collection is sharded.");
        return true;
    }

    if (testCase == "noop") {
        // TODO SERVER-100937 dropIndexes does not return WCE

        // TODO SERVER-100309 adapt/enable setFeatureCompatibilityVersion no-op case once the
        // upgrade procedure will not proactively shard the sessions collection.
        if (clusterType == "sharded" &&
            (shardedDDLCommandsRequiringMajorityCommit.includes(command) ||
             command == "dropIndexes" || command == "setFeatureCompatibilityVersion")) {
            jsTestLog("Skipping " + command + " test for no-op case.");
            return true;
        }
    }

    if (testCase == "success") {
        if (clusterType == "sharded" &&
            (shardedDDLCommandsRequiringMajorityCommit.includes(command))) {
            jsTestLog("Skipping " + command + " test for success case.");
            return true;
        }
    }

    if (testCase == "failure") {
        // TODO SERVER-100942 setDefaultRWConcern does not return WCE

        // TODO SERVER-100938 createIndexes does not return WCE

        // TODO SERVER-98461 findOneAndUpdate when query does not have shard key does not return WCE
        // TODO SERVER-9XXXX findAndModify when query has shard key does not return WCE
        if (clusterType == "sharded" &&
            (shardedDDLCommandsRequiringMajorityCommit.includes(command) ||
             command == "createIndexes" || command == "setDefaultRWConcern" ||
             command == "findOneAndUpdate" || command == "findAndModify")) {
            jsTestLog("Skipping " + command + " test for failure case.");
            return true;
        }

        if (clusterType == "rs" &&
            (command == "setDefaultRWConcern" || command == "createIndexes")) {
            jsTestLog("Skipping " + command + " test for failure case.");
            return true;
        }
    }
}

// These commands only accept w:1 or w:majority.
let umcRequireMajority = [
    "createRole",
    "createUser",
    "dropAllRolesFromDatabase",
    "dropAllUsersFromDatabase",
    "dropRole",
    "dropUser",
    "grantPrivilegesToRole",
    "grantRolesToRole",
    "grantRolesToUser",
    "revokePrivilegesFromRole",
    "revokeRolesFromRole",
    "revokeRolesFromUser",
    "updateRole",
    "updateUser"
];

function executeWriteConcernBehaviorTests(conn,
                                          coll,
                                          cluster,
                                          clusterType,
                                          preSetup,
                                          commandsToRun,
                                          masterCommandsList,
                                          secondariesRunning,
                                          shardedCollection,
                                          writeWithoutSk) {
    commandsToRun.forEach((command) => {
        let cmd = masterCommandsList[command];

        if (!cmd.skip && !cmd.noop && !cmd.success && !cmd.failure) {
            throw "Must implement test case for command " + command +
                ", or explain why it should be skipped.";
        }

        // Some commands only allow w:1 or w:majority in a sharded cluster, so we must choose
        // majority
        let forceUseMajorityWC = clusterType == "sharded" && umcRequireMajority.includes(command);

        if (cmd.noop) {
            if (!shouldSkipTestCase(
                    clusterType, command, "noop", shardedCollection, writeWithoutSk, coll))
                runCommandTest(cmd.noop,
                               conn,
                               coll,
                               cluster,
                               clusterType,
                               preSetup,
                               secondariesRunning,
                               forceUseMajorityWC);
        }

        if (cmd.success) {
            if (!shouldSkipTestCase(
                    clusterType, command, "success", shardedCollection, writeWithoutSk, coll))
                runCommandTest(cmd.success,
                               conn,
                               coll,
                               cluster,
                               clusterType,
                               preSetup,
                               secondariesRunning,
                               forceUseMajorityWC);
        }

        if (cmd.failure) {
            if (!shouldSkipTestCase(
                    clusterType, command, "failure", shardedCollection, writeWithoutSk, coll))
                runCommandTest(cmd.failure,
                               conn,
                               coll,
                               cluster,
                               clusterType,
                               preSetup,
                               secondariesRunning,
                               forceUseMajorityWC,
                               writeWithoutSk);
        }
    });
}

export function checkWriteConcernBehaviorForAllCommands(
    conn, cluster, clusterType, preSetup, shardedCollection, limitToTimeseriesViews = false) {
    jsTestLog("Checking write concern behavior for all commands");
    const commandsToTest =
        limitToTimeseriesViews ? wcTimeseriesViewsCommandsTests : wcCommandsTests;
    const commandsList = AllCommandsTest.checkCommandCoverage(conn, commandsToTest);

    let coll = conn.getDB(dbName).getCollection(collName);

    if (clusterType == "rs") {
        jsTestLog("Running tests against replica set");

        stopSecondaries(cluster, clusterType);

        executeWriteConcernBehaviorTests(
            conn, coll, cluster, clusterType, preSetup, commandsList, commandsToTest);

        restartSecondaries(cluster, clusterType);

        return;
    }

    assert.eq(clusterType, "sharded");

    // In a sharded cluster, some commands are coordinated through the configsvr, and others
    // through the shard(s). For commands that target the configsvr, we'll prevent replication
    // on the configsvr. Otherwise, we'll prevent replication on the shard(s). To speed up the
    // execution of this test, we'll run all commands that target the configsvr first and then
    // those that shards target shards so that we don't need to stop and restart nodes for each
    // test case.
    let cmdsTargetConfigServer = [];
    let cmdsTargetShards = [];
    commandsList.forEach((command) => {
        let cmd = commandsToTest[command];
        if (cmd.targetConfigServer) {
            cmdsTargetConfigServer.push(command);
        } else {
            cmdsTargetShards.push(command);
        }
    });

    if (FeatureFlagUtil.isPresentAndEnabled(cluster.configRS.getPrimary(),
                                            "CreateDatabaseDDLCoordinator")) {
        shardedDDLCommandsRequiringMajorityCommit.push("enableSharding");
    }

    // Run test cases for commands that target the configsvr
    (() => {
        jsTestLog("Running commands that target the configsvr");
        assert.eq(cluster.configRS.nodeList().length, 3);

        // Stop one of the secondaries so that w:3 will fail. Some commands override user write
        // concern to be w: majority. For those specific test cases, we'll need to shut down the
        // other secondary to ensure wiating for write concern will still fail.
        const csrsSecondaries = cluster.configRS.getSecondaries();
        cluster.configRS.stop(csrsSecondaries[0]);

        executeWriteConcernBehaviorTests(conn,
                                         coll,
                                         cluster,
                                         clusterType,
                                         preSetup,
                                         cmdsTargetConfigServer,
                                         commandsToTest,
                                         [csrsSecondaries[1]],
                                         shardedCollection);

        cluster.configRS.restart(csrsSecondaries[0]);
    })();

    // Run test cases for commands the target shard(s)
    (() => {
        jsTestLog("Running commands that target shards");

        let secondariesRunning = stopSecondaries(cluster, clusterType);

        executeWriteConcernBehaviorTests(conn,
                                         coll,
                                         cluster,
                                         clusterType,
                                         preSetup,
                                         cmdsTargetShards,
                                         commandsToTest,
                                         secondariesRunning,
                                         shardedCollection);

        restartSecondaries(cluster, clusterType);
    })();
}

export function checkWriteConcernBehaviorAdditionalCRUDOps(conn,
                                                           cluster,
                                                           clusterType,
                                                           preSetup,
                                                           shardedCollection,
                                                           writeWithoutSk,
                                                           limitToTimeseriesViews = false) {
    jsTestLog("Checking write concern behavior for additional CRUD commands");

    let coll = conn.getDB(dbName).getCollection(collName);

    const commandsToTest =
        limitToTimeseriesViews ? additionalCRUDOpsTimeseriesViews : additionalCRUDOps;

    stopSecondaries(cluster, clusterType);

    executeWriteConcernBehaviorTests(conn,
                                     coll,
                                     cluster,
                                     clusterType,
                                     preSetup,
                                     Object.keys(commandsToTest),
                                     commandsToTest,
                                     [] /* secondariesRunning */,
                                     shardedCollection);

    restartSecondaries(cluster, clusterType);
}

export function checkWriteConcernBehaviorUpdatingDocShardKey(conn,
                                                             cluster,
                                                             clusterType,
                                                             preSetup,
                                                             shardedCollection,
                                                             writeWithoutSk,
                                                             limitToTimeseriesViews = false) {
    jsTestLog("Checking write concern behavior for updating a document's shard key");

    let coll = conn.getDB(dbName).getCollection(collName);

    stopSecondaries(cluster, clusterType);

    // Note we don't need to check the transaction cases, because we will not wait for write concern
    // on the individual transaction statements, but rather only on the commit. We test commit
    // already separately above. We test the retryable write case because the server internally
    // executes commit in this case, and builds a response object from the commit response.
    let testCases = {
        "updateDocSk": {
            noop: {
                req: () => ({
                    update: collName,
                    updates:
                        [{q: {a: 1}, u: {$set: {[Object.keys(getShardKey(coll, fullNs))[0]]: -1}}}],
                    lsid: getLSID(),
                    txnNumber: getTxnNumber()
                }),
                setupFunc: (coll) => {
                    let sk = getShardKey(coll, fullNs);
                    if (bsonWoCompare(sk, {a: 1}) == 0) {
                        assert.commandWorked(coll.insert({a: 1}));
                        assert.commandWorked(coll.remove({a: 1}));
                    } else {
                        // Make sure doc has "a" to query on and the shard key
                        assert.commandWorked(coll.insert({a: 1, [Object.keys(sk)[0]]: 2}));
                        assert.commandWorked(coll.remove({[Object.keys(sk)[0]]: 2}));
                    }

                    assert.eq(coll.find().itcount(), 0);
                },
                confirmFunc: (res, coll) => {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(coll.find().itcount(), 0);
                    genNextTxnNumber();
                }
            },
            success: {
                req: () => ({
                    update: collName,
                    updates:
                        [{q: {a: 1}, u: {$set: {[Object.keys(getShardKey(coll, fullNs))[0]]: -1}}}],
                    lsid: getLSID(),
                    txnNumber: getTxnNumber()
                }),
                setupFunc: (coll) => {
                    let sk = getShardKey(coll, fullNs);
                    if (bsonWoCompare(sk, {a: 1}) == 0) {
                        assert.commandWorked(coll.insert({a: 1}));
                    } else {
                        // Make sure doc has "a" to query on and the shard key
                        assert.commandWorked(coll.insert({a: 1, [Object.keys(sk)[0]]: 2}));
                    }

                    assert.eq(coll.find().itcount(), 1);
                },
                confirmFunc: (res, coll) => {
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    assert.eq(
                        coll.find({[Object.keys(getShardKey(coll, fullNs))[0]]: -1}).itcount(), 1);
                    genNextTxnNumber();
                },
            },
            // We don't test a failure case, because if the insert or delete (executed internally by
            // the server) fails for any reason, mongod will not wait for write concern at all
            // (again, because it only waits on commit or abort, and in this case we won't try to
            // commit). So, this becomes a similar case as the commit case in the tests above -
            // there isn't a sequence of events that would mutate the data that would cause the
            // commit to fail in such a way that the WCE is important.
        }
    };

    executeWriteConcernBehaviorTests(conn,
                                     coll,
                                     cluster,
                                     clusterType,
                                     preSetup,
                                     Object.keys(testCases),
                                     testCases,
                                     [] /* secondariesRunning */,
                                     shardedCollection,
                                     writeWithoutSk,
                                     limitToTimeseriesViews);

    restartSecondaries(cluster, clusterType);
}

export function precmdShardKey(shardKey, conn, cluster, dbName, collName) {
    let db = conn.getDB(dbName);
    let nss = dbName + "." + collName;

    assert.commandWorked(
        db.adminCommand({enableSharding: dbName, primary: cluster.shard0.shardName}));
    assert.commandWorked(db.adminCommand({shardCollection: nss, key: {[shardKey]: 1}}));
    assert.commandWorked(db.adminCommand({split: nss, middle: {[shardKey]: 0}}));
    assert.commandWorked(db.adminCommand({
        moveChunk: nss,
        find: {[shardKey]: -1},
        to: cluster.shard0.shardName,
        _waitForDelete: true
    }));
    assert.commandWorked(db.adminCommand({
        moveChunk: nss,
        find: {[shardKey]: 1},
        to: cluster.shard1.shardName,
        _waitForDelete: true
    }));
};
