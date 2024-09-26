/*
 * Tests that the 'changeStreamPreAndPostImages' option cannot be set on collections in the 'local',
 * 'admin', 'config' databases.
 * @tags: [
 * requires_replication,
 * # Internal databases are not replicated.
 * does_not_support_stepdowns,
 * # Cannot use internal databases in the sharded cluster.
 * assumes_against_mongod_not_mongos,
 * # Cannot use internal databases in multi-tenancy mode.
 * command_not_supported_in_serverless,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const collName = 'changeStreamPreAndPostImages';

if (!TestData.testingReplicaSetEndpoint) {
    const adminDB = db.getSiblingDB("admin");
    const localDB = db.getSiblingDB("local");
    const configDB = db.getSiblingDB("config");

    // Check that we cannot set 'changeStreamPreAndPostImages' on the local, admin and config
    // databases.
    for (const db of [localDB, adminDB, configDB]) {
        assertDropCollection(db, collName);
        assert.commandFailedWithCode(
            db.runCommand({create: collName, changeStreamPreAndPostImages: {enabled: true}}),
            ErrorCodes.InvalidOptions);

        assert.commandWorked(db.runCommand({create: collName}));
        assert.commandFailedWithCode(
            db.runCommand({collMod: collName, changeStreamPreAndPostImages: {enabled: true}}),
            ErrorCodes.InvalidOptions);
    }
}