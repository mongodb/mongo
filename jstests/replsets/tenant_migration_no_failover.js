/**
 * Tests a full tenant migration, assuming no failover.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern]
 */

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const numDocs = 20;
const data = [];
for (let i = 0; i < numDocs; ++i) {
    data.push({_id: i, x: i});
}
const insertDonorDB = (primary, dbName, collName) => {
    const db = primary.getDB(dbName);
    const coll = db.getCollection(collName);

    assert.commandWorked(coll.insertMany(data));
};

const verifyReceipientDB = (primary, dbName, collName, shouldMigrate) => {
    const db = primary.getDB(dbName);
    const coll = db.getCollection(collName);

    const findRes = coll.find();
    const numDocsFound = findRes.count();

    if (!shouldMigrate) {
        assert.eq(0,
                  numDocsFound,
                  `Find command on recipient collection ${collName} of DB ${
                      dbName} should return 0 docs, instead has count of ${numDocsFound}`);
        return;
    }

    assert.eq(numDocs,
              numDocsFound,
              `Find command on recipient collection ${collName} of DB ${dbName} should return ${
                  numDocs} docs, instead has count of ${numDocsFound}`);

    const docsReturned = findRes.sort({_id: 1}).toArray();
    assert(arrayEq(docsReturned, data),
           () => (`${tojson(docsReturned)} is not equal to ${tojson(data)}`));
};

const name = jsTestName();
const donorRst = new ReplSetTest(
    {name: `${name}_donor`, nodes: 1, nodeOptions: {setParameter: {enableTenantMigrations: true}}});
const recipientRst = new ReplSetTest({
    name: `${name}_recipient`,
    nodes: 1,
    nodeOptions: {
        setParameter: {
            enableTenantMigrations: true,
            // TODO SERVER-51734: Remove the failpoint 'returnResponseOkForRecipientSyncDataCmd'.
            'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'})
        }
    }
});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const tenantId = 'testTenantId';

const dbNames = ["db0", "db1", "db2"];
const tenantDBs = dbNames.map(dbName => TenantMigrationUtil.tenantDB(tenantId, dbName));
const nonTenantDBs = dbNames.map(dbName => TenantMigrationUtil.nonTenantDB(tenantId, dbName));
const collNames = ["coll0", "coll1"];

const donorPrimary = donorRst.getPrimary();
const recipientPrimary = recipientRst.getPrimary();

for (const db of [...tenantDBs, ...nonTenantDBs]) {
    for (const coll of collNames) {
        insertDonorDB(donorPrimary, db, coll);
    }
}

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: recipientRst.getURL(),
    tenantId,
    readPreference: {mode: "primary"}
};

const res =
    assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
assert.eq(res.state, "committed");

for (const coll of collNames) {
    for (const db of tenantDBs) {
        // TODO (SERVER-51734): Change shouldMigrate to true.
        verifyReceipientDB(recipientPrimary, db, coll, false /* shouldMigrate */);
    }

    for (const db of nonTenantDBs) {
        verifyReceipientDB(recipientPrimary, db, coll, false /* shouldMigrate */);
    }
}

donorRst.stopSet();
recipientRst.stopSet();
})();
