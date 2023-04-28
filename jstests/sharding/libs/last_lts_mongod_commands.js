// These commands were removed from mongod since the last LTS version, but will still appear in the
// listCommands output of a last LTS version mongod. To increase test coverage and allow us
// to run on same- and mixed-version suites, we allow these commands to have a test defined without
// always existing on the servers being used.
const commandsRemovedFromMongodSinceLastLTS = [
    "_configsvrCommitChunkMerge",
    "_configsvrCreateCollection",
    "_configsvrMoveChunk",
    "_configsvrRepairShardedCollectionChunksHistory",
    "_configsvrTransitionToCatalogShard",
    "mapreduce.shardedfinish",
    "getLastError",
    "driverOIDTest",
];
// These commands were added in mongod since the last LTS version, so will not appear in the
// listCommands output of a last LTS version mongod. We will allow these commands to have a
// test defined without always existing on the mongod being used.
const commandsAddedToMongodSinceLastLTS = [
    "_refreshQueryAnalyzerConfiguration",  // TODO (SERVER-68977): Remove upgrade/downgrade for
                                           // PM-1858.
    "analyzeShardKey",  // TODO (SERVER-68977): Remove upgrade/downgrade for PM-1858.
    "clusterAbortTransaction",
    "clusterAggregate",
    "clusterCommitTransaction",
    "clusterCount",
    "clusterDelete",
    "clusterFind",
    "clusterGetMore",
    "clusterInsert",
    "clusterUpdate",
    "configureQueryAnalyzer",  // TODO (SERVER-68977): Remove upgrade/downgrade for PM-1858.
    "createSearchIndexes",     // TODO (SERVER-73309): Remove once 7.0 becomes last LTS.
    "dropSearchIndex",         // TODO (SERVER-73309): Remove once 7.0 becomes last LTS.
    "getChangeStreamState",
    "getClusterParameter",
    "listDatabasesForAllTenants",
    "listSearchIndexes",  // TODO (SERVER-73309): Remove once 7.0 becomes last LTS.
    "oidcListKeys",
    "oidcRefreshKeys",
    "rotateCertificates",
    "setChangeStreamState",
    "setClusterParameter",
    "setUserWriteBlockMode",
    "updateSearchIndex",  // TODO (SERVER-73309): Remove once 7.0 becomes last LTS.
];
