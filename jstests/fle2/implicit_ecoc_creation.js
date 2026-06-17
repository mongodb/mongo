/**
 * Checks that implicit ECOC collection creation adds a clustered index.
 *
 * Steps:
 * - Creates a QE collection
 * - Start QE compaction, block it via failpoint before re-creating the ECOC collection
 * - Insert a document, implicitly creating the ECOC collection
 * - Check that the collection has been created as clustered
 * - Unblock QE compaction and wait for it to complete
 *
 * @tags: [
 *   no_selinux,
 *   does_not_support_transactions,
 *   does_not_support_stepdowns,
 *   # Assuming stable db primary shard for failpoint
 *   assumes_stable_shard_list,
 *   uses_parallel_shell,
 * ]
 */
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

if (!isEnterpriseShell()) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

const dbName = jsTestName();
const collName = "coll";
const testDb = db.getSiblingDB(dbName);

function getEcocOptions() {
    return assert.commandWorked(
        testDb.runCommand({listCollections: 1, filter: {name: "enxcol_." + collName + ".ecoc"}}),
    ).cursor.firstBatch[0].options;
}

const client = new EncryptedClient(db.getMongo(), dbName);

// Create the main collection and the metadata collections, including the ECOC
assert.commandWorked(
    client.createEncryptionCollection(collName, {
        encryptedFields: {
            fields: [{path: "ssn", bsonType: "string", queries: {queryType: "equality"}}],
        },
    }),
);

// Verify that the original ECOC collection exists and is clustered
assert.neq(undefined, getEcocOptions().clusteredIndex);

// Configure failpoint to block compact right before recreating the ECOC collection
const fpName = FixtureHelpers.isMongos(db)
    ? "fleCompactHangBeforeECOCCreate"
    : "fleCompactHangBeforeECOCCreateUnsharded";
const fp = configureFailPoint(FixtureHelpers.getPrimaryForNodeHostingDatabase(testDb), fpName);

// Run compactStructuredEncryptionData in a parallel shell
const runCompact = function (dbName, collName) {
    const conn = db.getMongo();
    conn.setAutoEncryption({
        kmsProviders: {
            local: {
                key: BinData(
                    0,
                    "/tu9jUCBqZdwCelwE/EAm/4WqdxrSMi04B8e9uAV+m30rI1J2nhKZZtQjdvsSCwuI4erR6IEcEK+5eGUAODv43NDNIR9QheT2edWFewUfHKsl9cnzTc86meIzOmYl6dr",
                ),
            },
        },
        keyVaultNamespace: dbName + ".keystore",
        schemaMap: {},
    });
    conn.toggleAutoEncryption(true);
    const reply = assert.commandWorked(db.getSiblingDB(dbName)[collName].compact());
    conn.toggleAutoEncryption(false);
    conn.unsetAutoEncryption();
};

const joinCompact = startParallelShell(
    funWithArgs(runCompact, dbName, collName),
    db.getMongo().port,
);

fp.wait();

// The ECOC collection doesn't exist yet because it has been renamed halfway during compaction
assert.eq(
    0,
    assert.commandWorked(
        testDb.runCommand({listCollections: 1, filter: {name: "enxcol_." + collName + ".ecoc"}}),
    ).cursor.firstBatch.length,
);

// Implicitly recreate the ECOC collection via insert (accepting error caused by SERVER-128430)
let implicitCreationSucceeded = true;
client.runEncryptionOperation(() => {
    const ecoll = client.getDB()[collName];
    try {
        assert.commandWorked(ecoll.insertOne({_id: 0, ssn: "000-00-0000"}));
    } catch (e) {
        if (
            e.code != ErrorCodes.OperationNotSupportedInTransaction ||
            !e.message.match(/Cannot create new collections.*inside distributed transactions/)
        ) {
            throw e;
        }
        implicitCreationSucceeded = false;
    }
});

if (implicitCreationSucceeded) {
    // Verify that the new implicitly created ECOC collection exists and is clustered
    assert.neq(
        undefined,
        getEcocOptions().clusteredIndex,
        "ECOC collection missing clustered index",
    );
} else {
    // TODO SERVER-128430 remove `implicitCreationSucceeded` and this whole else body
    // When the implicit ECOC creation fails because it is not supported in distributed
    // transactions, the main collection must not be fully placed on the db primary shard.
    const collNs = dbName + "." + collName;
    const dbPrimaryShardId = db.getSiblingDB("config").databases.findOne({_id: dbName}).primary;
    const collEntry = db.getSiblingDB("config").collections.findOne({_id: collNs});
    const chunks = db.getSiblingDB("config").chunks.find({uuid: collEntry.uuid}).toArray();
    assert(
        !chunks.every((chunk) => chunk.shard === dbPrimaryShardId),
        "Expected collection to not be fully placed on db primary shard",
        {collNs, dbPrimaryShardId, chunks},
    );
}

fp.off();
joinCompact();
