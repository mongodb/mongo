/**
 * Ensure that transaction rollback succeeds after an interrupted index ttl update or upgrade
 * collMod command.
 */
let dbpath = MongoRunner.dataPath + "setFCV_collmod_transaction_rollback";
resetDbpath(dbpath);

const latest = "latest";

let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
assert.neq(null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
let adminDB = conn.getDB("admin");

let collName = "collModTest";
let coll = adminDB.getCollection(collName);
let ttlBeforeRollback = 50;

assert.commandWorked(coll.createIndex({b: 1}, {"name": "index1", "expireAfterSeconds": ttlBeforeRollback}));

// The failpoint causes an interrupt in the collMod's WriteUnitOfWork, thus triggers a rollback.
assert.commandWorked(adminDB.adminCommand({configureFailPoint: "assertAfterIndexUpdate", mode: "alwaysOn"}));

// Test transaction rollback after index ttl update collMod.
assert.commandFailedWithCode(
    adminDB.runCommand({"collMod": collName, "index": {"name": "index1", "expireAfterSeconds": 100}}),
    50970,
);

const index = coll.getIndexes();
let ttlAfterRollback = index[1].expireAfterSeconds;
assert.eq(ttlAfterRollback, ttlBeforeRollback);

MongoRunner.stopMongod(conn);
