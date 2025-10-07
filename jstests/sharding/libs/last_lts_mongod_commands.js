// These commands were removed from mongod since the last LTS version, but will still appear in the
// listCommands output of a last LTS version mongod. To increase test coverage and allow us
// to run on same- and mixed-version suites, we allow these commands to have a test defined without
// always existing on the servers being used.
export const commandsRemovedFromMongodSinceLastLTS = [
    "_getAuditConfigGeneration",  // Removed in 8.1
    "_configsvrRefineCollectionShardKey",
    "_shardsvrCommitToShardLocalCatalog",  // Removed in 8.2
    "stageDebug",
    "_configsvrRemoveShardCommit",
    "_configsvrAddShardCoordinator",
    "_shardsvrCommitIndexParticipant",
    "_shardsvrDropIndexCatalogEntryParticipant",
    "_shardsvrRegisterIndex",
    "_shardsvrUnregisterIndex",
    "_configsvrCommitIndex",
    "_configsvrDropIndexCatalogEntry",
    "testCommandFeatureFlaggedOnLatestFCV",
];

// These commands were added in mongod since the last LTS version, so will not appear in the
// listCommands output of a last LTS version mongod. We will allow these commands to have a
// test defined without always existing on the mongod being used.
export const commandsAddedToMongodSinceLastLTS = [
    "releaseMemory",
    "_configsvrStartShardDraining",
    "_configsvrShardDrainingStatus",
    "testCommandFeatureFlaggedOnLatestFCV82",
];
