/**
 * Commands supporting read and write concern.
 */
export var kCommandsSupportingReadConcern = new Set([
    "aggregate",
    "count",
    "distinct",
    "find",
]);

/**
 * Write commands supporting snapshot readConcern in a transaction.
 */
export var kWriteCommandsSupportingSnapshotInTransaction = new Set([
    "delete",
    "findAndModify",
    "findandmodify",
    "insert",
    "update",
]);

/**
 * Commands supporting snapshot readConcern outside of transactions.
 */
export var kCommandsSupportingSnapshot = new Set([
    "aggregate",
    "distinct",
    "find",
]);

export var kCommandsSupportingWriteConcern = new Set([
    "_configsvrAddShard",
    "_configsvrAddShardToZone",
    "_configsvrCommitChunksMerge",
    "_configsvrCommitChunkMigration",
    "_configsvrCommitChunkSplit",
    "_configsvrCommitIndex",
    "_configsvrCommitMergeAllChunksOnShard",
    "_configsvrCreateDatabase",
    "_configsvrDropIndexCatalogEntry",
    "_configsvrMoveRange",
    "_configsvrRemoveShard",
    "_configsvrRemoveShardFromZone",
    "_configsvrUpdateZoneKeyRange",
    "_mergeAuthzCollections",
    "_recvChunkStart",
    "abortTransaction",
    "appendOplogNote",
    "applyOps",
    "aggregate",
    "cleanupOrphaned",
    "clone",
    "cloneCollectionAsCapped",
    "collMod",
    "commitTransaction",
    "convertToCapped",
    "create",
    "createIndexes",
    "createRole",
    "createUser",
    "delete",
    "deleteIndexes",
    "drop",
    "dropAllRolesFromDatabase",
    "dropAllUsersFromDatabase",
    "dropDatabase",
    "dropIndexes",
    "dropRole",
    "dropUser",
    "findAndModify",
    "findandmodify",
    "godinsert",
    "grantPrivilegesToRole",
    "grantRolesToRole",
    "grantRolesToUser",
    "insert",
    "mapReduce",
    "mapreduce",
    "moveChunk",
    "renameCollection",
    "revokePrivilegesFromRole",
    "revokeRolesFromRole",
    "revokeRolesFromUser",
    "setFeatureCompatibilityVersion",
    "testInternalTransactions",
    "update",
    "updateRole",
    "updateUser",
]);

export var kCommandsSupportingWriteConcernInTransaction =
    new Set(["abortTransaction", "commitTransaction"]);
