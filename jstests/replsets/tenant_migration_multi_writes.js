/**
 * Stress test verifies that non-idempotent multi updates made during tenant migration
 * were not retried on migration abort, which would create duplicate updates. Partially
 * updated collection where each update is applied no more than once is still an expected result.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const donorRst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    name: "TenantMigrationTest_donor",
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor, {
        setParameter: {
            // Set the delay before a donor state doc is garbage collected to be short to speed up
            // the test.
            tenantMigrationGarbageCollectionDelayMS: 3 * 1000,

            // Set the TTL monitor to run at a smaller interval to speed up the test.
            ttlMonitorSleepSecs: 1,
        }
    })
});
donorRst.startSet();
donorRst.initiateWithHighElectionTimeout();
const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst: donorRst});

if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const recipientRst = tenantMigrationTest.getRecipientRst();
const donorPrimary = donorRst.getPrimary();

const kTenantIdPrefix = "testTenantId";
const kCollName = "testColl";
const kTenantDefinedDbName = "0";
const kTenantId = `${kTenantIdPrefix}-multiWrites`;
const kDbName = tenantMigrationTest.tenantDB(kTenantId, kTenantDefinedDbName);

const kRecords = 2000;
const kUpdateCycles = 600;

function prepareDatabase(dbName) {
    let db = donorPrimary.getDB(dbName);
    try {
        db.dropDatabase();
    } catch (err) {
        // First time the DB doesn't exist.
    }
    assert.commandWorked(db.runCommand({create: kCollName}));
    let bulk = db[kCollName].initializeUnorderedBulkOp();
    for (let i = 0; i < kRecords; ++i) {
        bulk.insert({_id: i, x: 0, a: 1});
    }
    assert.commandWorked(bulk.execute());
}

function doMultiUpdate(
    primaryHost, dbName, collName, records, updateCycles, readConcern, writeConcern) {
    const donorPrimary = new Mongo(primaryHost);
    let db = donorPrimary.getDB(dbName);
    let completedCycles;

    for (completedCycles = 0; completedCycles < updateCycles; ++completedCycles) {
        try {
            let bulk = db[collName].initializeUnorderedBulkOp();
            bulk.find({a: 1}).update({$inc: {x: 1}});
            bulk.execute({w: writeConcern});
        } catch (err) {
            jsTestLog(`Received error ${err}`);
            assert.commandFailedWithCode(err, ErrorCodes.Interrupted);
            assert.lte(err["nModified"], records);
            let actualNumModified = 0;
            let findResult = assert.commandWorked(
                db.runCommand({find: collName, readConcern: {level: readConcern}}));
            var cursor = new DBCommandCursor(db, findResult);
            cursor.forEach(doc => {
                assert(doc.x == completedCycles || doc.x == completedCycles + 1,
                       "expected each doc to be updated at most once");
                actualNumModified += (doc.x == completedCycles + 1 ? 1 : 0);
            });
            // TODO(SERVER-15292): uncomment this when the bug is fixed, and reconcile with the
            // block after the commented section.
            //    assert.eq(err["nModified"], actualNumModified,
            //           `expected the count of incremented values to match nModified: ${
            //               err} during iteration # ${completedCycles}, actually modified ${
            //               actualNumModified} in ${JSON.stringify(findResult)}`);
            // if (actualNumModified == records) {
            //     // All records were modified, we can continue.
            //     continue;
            // }
            // break;
            if (err["nModified"] != actualNumModified) {
                jsTestLog(`expected the count of incremented values to match nModified: ${
                    err} during iteration # ${completedCycles}, actually modified ${
                    actualNumModified} in ${JSON.stringify(findResult)}`);
            }
            break;
        }
    }

    jsTestLog(`All updates completed without consistency error, in ${completedCycles} cycles`);
}

function testMultiWritesWhileInBlockingState(readConcern, writeConcern) {
    prepareDatabase(kDbName);

    let writesThread = new Thread(
        // Start non-idempotent writes in a thread.
        doMultiUpdate,
        donorPrimary.host,
        kDbName,
        kCollName,
        kRecords,
        kUpdateCycles,
        readConcern,
        writeConcern);
    writesThread.start();

    for (let i = 0; i < 10; ++i) {
        const migrationId = UUID();
        const migrationOpts = {
            migrationIdString: extractUUIDFromObject(migrationId),
            tenantId: kTenantId,
            recipientConnString: tenantMigrationTest.getRecipientConnString(),
        };
        let abortFp =
            configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState", {
                blockTimeMS: Math.floor(Math.random() * 10),
            });
        assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

        const stateRes = assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(
            migrationOpts, false /* retryOnRetryableErrors */));
        assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);

        abortFp.wait();
        abortFp.off();

        assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

        jsTest.log('Drop DB and wait for garbage collection after test cycle.');
        assert.commandWorked(recipientRst.getPrimary().getDB(kDbName).dropDatabase());
    }

    writesThread.join();
}

let readWriteConcerns = [];
readWriteConcerns.push({writeConcern: 1, readConcern: 'local'});
readWriteConcerns.push({writeConcern: 'majority', readConcern: 'majority'});

readWriteConcerns.forEach(concerns => {
    jsTest.log(`Test sending multi write while in migration blocking state with ${concerns}`);
    testMultiWritesWhileInBlockingState(concerns.readConcern, concerns.writeConcern);
});

tenantMigrationTest.stop();
donorRst.stopSet();
})();
