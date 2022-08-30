// These commands were removed from mongos since the last LTS version, but will still appear in the
// listCommands output of a last LTS version mongos. A last-lts mongos will be unable to
// run a command on a latest version shard that no longer supports that command. To increase test
// coverage and allow us to run on same- and mixed-version suites, we allow these commands to have a
// test defined without always existing on the servers being used.
const commandsRemovedFromMongosSinceLastLTS = [
    "repairShardedCollectionChunksHistory",
    "configureCollectionAutoSplitter",  // TODO SERVER-62374: remove this once 5.3 becomes
                                        // last-continuos
    "availableQueryOptions",            // TODO SERVER-67689: remove this once 7.0 becomes last-lts

];
// These commands were added in mongos since the last LTS version, so will not appear in the
// listCommands output of a last LTS version mongos. We will allow these commands to have a test
// defined without always existing on the mongos being used.
const commandsAddedToMongosSinceLastLTS = [
    "abortReshardCollection",
    "analyze",
    "analyzeShardKey",  // TODO (SERVER-68977): Remove upgrade/downgrade for PM-1858.
    "appendOplogNote",
    "cleanupReshardCollection",
    "commitReshardCollection",
    "compactStructuredEncryptionData",
    "configureCollectionBalancing",
    "configureQueryAnalyzer",  // TODO (SERVER-68977): Remove upgrade/downgrade for PM-1858.
    "coordinateCommitTransaction",
    "getClusterParameter",
    "moveRange",
    "reshardCollection",
    "rotateCertificates",
    "setAllowMigrations",
    "setClusterParameter",
    "setUserWriteBlockMode",
    "testDeprecation",
    "testDeprecationInVersion2",
    "testInternalTransactions",
    "testRemoval",
    "testVersions1And2",
    "testVersion2",
];
