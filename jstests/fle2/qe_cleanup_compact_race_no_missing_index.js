/**
 * Regression test for SERVER-126384.
 *
 * QE cleanup/compact operations rename the live ECOC out of the way, release the collection
 * lock, then re-create the ECOC with the required {clusteredIndex: {key: {_id: 1}, unique: true}}
 * option. A concurrent user write that arrives in the gap between rename and re-create can
 * IMPLICITLY create the ECOC namespace with DEFAULT options (i.e. without the clustered index).
 * The internal create issued by compact/cleanup then silently no-ops because the namespace
 * already exists, the command returns success, and the ECOC is left permanently missing its
 * clustered index — degrading subsequent compactions and queries.
 *
 * This test drives the race for BOTH compactStructuredEncryptionData and
 * cleanupStructuredEncryptionData (one collection each), runs concurrent encrypted inserts in
 * the foreground while the maintenance command runs in a parallel shell, and asserts that
 * post-race the ECOC namespace always exists with its clustered index option preserved.
 *
 * Pairs with the TLA+ specification at:
 *   src/mongo/tla_plus/FLE/ECOCCompactCleanupRace/ECOCCompactCleanupRace.tla
 *
 * @tags: [
 *   no_selinux,
 *   does_not_support_transactions,
 *   does_not_support_stepdowns,
 *   assumes_unsharded_collection,
 *   assumes_balancer_off,
 *   requires_fcv_70,
 * ]
 */
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

if (!isEnterpriseShell()) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

const dbName = "qe_cleanup_compact_race_no_missing_index_db";
const encryptedFields = {
    fields: [
        {
            path: "ssn",
            bsonType: "string",
            queries: {queryType: "equality"},
        },
    ],
};

// Number of concurrent encrypted inserts the foreground shell will fire while the maintenance
// command runs in the parallel shell. Each insert sleeps briefly so it has a real chance of
// landing inside the rename->create window.
const kNumConcurrentInserts = 10;
const kInterInsertSleepMs = 200;

/**
 * Reads the live ECOC's collection options from listCollections and returns the options object,
 * or undefined if the ECOC doesn't exist. Used both pre- and post-race.
 */
function getEcocOptions(testDB, ecocName) {
    const infos = testDB.getCollectionInfos({name: ecocName});
    if (infos.length === 0) {
        return undefined;
    }
    return infos[0].options;
}

/**
 * Asserts the ECOC exists with a clustered index. Failure message includes provenance hints
 * so the SERVER-126384 race is identifiable in test logs.
 */
function assertEcocHasClusteredIndex(testDB, ecocName, phase) {
    const opts = getEcocOptions(testDB, ecocName);
    assert.neq(undefined, opts, `[${phase}] ECOC namespace '${ecocName}' does not exist`);
    assert.neq(
        undefined,
        opts.clusteredIndex,
        `[${phase}] SERVER-126384: ECOC namespace '${ecocName}' is missing its clustered index. ` +
            `Options observed: ${tojson(opts)}`,
    );
}

/**
 * Body of the parallel shell that runs the maintenance command. Bootstraps its own encrypted
 * connection against the same local KMS key + key vault, then issues either
 * compactStructuredEncryptionData or cleanupStructuredEncryptionData against the target
 * collection.
 */
const runMaintenance = function (dbName, collName, command) {
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
    const sessionDB = db.getSiblingDB(dbName);
    let reply;
    if (command === "compact") {
        reply = assert.commandWorked(sessionDB[collName].compact());
    } else if (command === "cleanup") {
        reply = assert.commandWorked(sessionDB.runCommand({cleanupStructuredEncryptionData: collName}));
    } else {
        throw new Error(`Unknown maintenance command: ${command}`);
    }
    jsTestLog(`Parallel ${command}StructuredEncryptionData reply: ${tojson(reply)}`);
    conn.toggleAutoEncryption(false);
    conn.unsetAutoEncryption();
};

/**
 * Drives one race iteration: kicks off the maintenance command in a parallel shell, fires
 * kNumConcurrentInserts encrypted inserts in the foreground with brief pauses, joins, and
 * checks the ECOC's clustered index option.
 */
function runRace(client, collName, maintenanceCmd, port) {
    const testDB = client.getDB();
    const ecocName = client.getStateCollectionNamespaces(collName).ecoc;

    // Sanity: pre-race the ECOC must already have its clustered index.
    assertEcocHasClusteredIndex(testDB, ecocName, `pre-${maintenanceCmd}`);

    const joinMaintenance = startParallelShell(
        funWithArgs(runMaintenance, dbName, collName, maintenanceCmd),
        port,
    );

    client.runEncryptionOperation(() => {
        const ecoll = testDB[collName];
        for (let i = 0; i < kNumConcurrentInserts; i++) {
            const ssn = `${String(i).padStart(3, "0")}-00-${maintenanceCmd === "cleanup" ? "1111" : "0000"}`;
            assert.commandWorked(
                ecoll.insertOne({
                    _id: `${maintenanceCmd}-${i}`,
                    ssn: ssn,
                }),
            );
            sleep(kInterInsertSleepMs);
        }
    });

    jsTestLog(`Waiting for parallel ${maintenanceCmd}StructuredEncryptionData to finish`);
    joinMaintenance();

    // Core SERVER-126384 assertion: the ECOC must STILL have its clustered index after the
    // rename/release/create dance has interleaved with user inserts.
    assertEcocHasClusteredIndex(testDB, ecocName, `post-${maintenanceCmd}`);
}

const testDB = db.getSiblingDB(dbName);
testDB.dropDatabase();

const client = new EncryptedClient(db.getMongo(), dbName);
const port = db.getMongo().port;

// Drive the race against compactStructuredEncryptionData.
const compactCollName = "compact_race_coll";
assert.commandWorked(client.createEncryptionCollection(compactCollName, {encryptedFields: encryptedFields}));
runRace(client, compactCollName, "compact", port);

// Drive the race against cleanupStructuredEncryptionData (same rename/release/create pattern).
const cleanupCollName = "cleanup_race_coll";
assert.commandWorked(client.createEncryptionCollection(cleanupCollName, {encryptedFields: encryptedFields}));
runRace(client, cleanupCollName, "cleanup", port);

jsTestLog("SERVER-126384 regression check passed: ECOC clustered index survived concurrent compact + cleanup + writes.");
